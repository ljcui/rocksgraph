#include "ir/planner/component_planner.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "ir/planner/cost_model.h"
#include "ir/planner/idp.h"
#include "ir/planner/plan_clone.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

std::vector<std::string> SortedComponentNodes(
    const QueryGraphComponent &component) {
  std::vector<std::string> out(component.pattern_nodes.begin(),
                               component.pattern_nodes.end());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> SortedComponentArgumentNodes(
    const QueryGraphComponent &component,
    const std::unordered_set<std::string> &argument_ids) {
  std::vector<std::string> out;
  for (const auto &node : component.pattern_nodes) {
    if (argument_ids.contains(node)) {
      out.push_back(node);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

ExpandDirection ToExpandDirection(Direction direction) {
  switch (direction) {
    case Direction::kIncoming:
      return ExpandDirection::kIncoming;
    case Direction::kOutgoing:
      return ExpandDirection::kOutgoing;
    case Direction::kBoth:
      return ExpandDirection::kBoth;
  }
  THROW(common::InternalError, "unknown relationship direction");
}

Direction Reverse(Direction direction) {
  switch (direction) {
    case Direction::kIncoming:
      return Direction::kOutgoing;
    case Direction::kOutgoing:
      return Direction::kIncoming;
    case Direction::kBoth:
      return Direction::kBoth;
  }
  THROW(common::InternalError, "unknown relationship direction");
}

class RuleBasedComponentPlanner final : public ComponentPlanner {
 public:
  std::unique_ptr<LogicalPlan> Plan(
      const QueryGraph &query_graph, const QueryGraphComponent &component,
      QueryGraphPlanningContext *context) const override {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");

    if (component.pattern_relationship_indices.empty()) {
      const std::vector<std::string> nodes = SortedComponentNodes(component);
      CHECK(nodes.size() == 1, common::InvalidArgumentError,
            Unsupported("multi-node disconnected component logical plan"));
      CostEstimate estimate =
          query_graph.argument_ids.contains(nodes.front())
              ? cost_model_.EstimateArgument(1)
              : cost_model_.EstimateNodeScan(
                    query_graph.LabelsOnNode(nodes.front()));
      std::unique_ptr<LogicalPlan> plan =
          context->BuildNodeLeaf(query_graph, nodes.front());
      const std::size_t filter_count =
          context->ApplyAvailableFilters(query_graph.selections, &plan);
      PlanTable plan_table;
      plan_table.PutBest(MakePlanCandidate(
          std::move(plan), {},
          ApplyFilterEstimates(estimate, filter_count, cost_model_)));
      return plan_table.TakeBest({}).plan;
    }

    PlanTable plan_table;
    std::unique_ptr<LogicalPlan> initial_plan;
    CostEstimate initial_estimate;
    const std::vector<std::string> argument_nodes =
        SortedComponentArgumentNodes(component, query_graph.argument_ids);
    if (!argument_nodes.empty()) {
      initial_plan = std::make_unique<ArgumentPlan>(argument_nodes);
      initial_estimate = cost_model_.EstimateArgument(argument_nodes.size());
    } else {
      const PatternRelationship &first_relationship =
          query_graph
              .pattern_relationships[component.pattern_relationship_indices[0]];
      initial_plan =
          context->BuildNodeLeaf(query_graph, first_relationship.left_node);
      initial_estimate = cost_model_.EstimateNodeScan(
          query_graph.LabelsOnNode(first_relationship.left_node));
    }
    const std::size_t initial_filter_count =
        context->ApplyAvailableFilters(query_graph.selections, &initial_plan);
    plan_table.PutBest(MakePlanCandidate(
        std::move(initial_plan), {},
        ApplyFilterEstimates(initial_estimate, initial_filter_count,
                             cost_model_)));

    std::vector<std::size_t> current_key;
    std::vector<std::size_t> remaining = component.pattern_relationship_indices;
    while (!remaining.empty()) {
      PlanCandidate candidate = plan_table.TakeBest(current_key);
      auto found = std::find_if(
          remaining.begin(), remaining.end(), [&](std::size_t index) {
            const PatternRelationship &relationship =
                query_graph.pattern_relationships[index];
            return candidate.covered_symbols.contains(relationship.left_node) ||
                   candidate.covered_symbols.contains(relationship.right_node);
          });
      CHECK(found != remaining.end(), common::InvalidArgumentError,
            Unsupported("disconnected relationship expansion logical plan"));

      const std::size_t relationship_index = *found;
      const PatternRelationship &relationship =
          query_graph.pattern_relationships[relationship_index];
      const bool left_solved =
          candidate.covered_symbols.contains(relationship.left_node);
      const bool right_solved =
          candidate.covered_symbols.contains(relationship.right_node);
      if (left_solved && right_solved) {
        CostEstimate estimate = cost_model_.EstimateExpandInto(
            CandidateEstimate(candidate), relationship.types);
        candidate.plan = std::make_unique<ExpandIntoPlan>(
            std::move(candidate.plan), relationship.left_node,
            relationship.variable, relationship.right_node,
            ToExpandDirection(relationship.direction), relationship.types);
        remaining.erase(found);
        const std::size_t filter_count = context->ApplyAvailableFilters(
            query_graph.selections, &candidate.plan);
        candidate.relationship_indices.push_back(relationship_index);
        PlanCandidate next_candidate = MakePlanCandidate(
            std::move(candidate.plan),
            std::move(candidate.relationship_indices),
            ApplyFilterEstimates(estimate, filter_count, cost_model_));
        current_key = next_candidate.relationship_indices;
        plan_table.PutBest(std::move(next_candidate));
        continue;
      }

      const std::string from_node =
          left_solved ? relationship.left_node : relationship.right_node;
      const std::string to_node =
          left_solved ? relationship.right_node : relationship.left_node;
      const Direction direction = left_solved ? relationship.direction
                                              : Reverse(relationship.direction);

      CostEstimate estimate = cost_model_.EstimateExpand(
          CandidateEstimate(candidate), relationship.types);
      candidate.plan = std::make_unique<ExpandPlan>(
          std::move(candidate.plan), from_node, relationship.variable, to_node,
          ToExpandDirection(direction), relationship.types);
      remaining.erase(found);
      const std::size_t filter_count = context->ApplyAvailableFilters(
          query_graph.selections, &candidate.plan);
      candidate.relationship_indices.push_back(relationship_index);
      PlanCandidate next_candidate = MakePlanCandidate(
          std::move(candidate.plan), std::move(candidate.relationship_indices),
          ApplyFilterEstimates(estimate, filter_count, cost_model_));
      current_key = next_candidate.relationship_indices;
      plan_table.PutBest(std::move(next_candidate));
    }

    return plan_table.TakeBest(current_key).plan;
  }

 private:
  CostModel cost_model_;
};

class IdpComponentPlanner final : public ComponentPlanner {
 public:
  std::unique_ptr<LogicalPlan> Plan(
      const QueryGraph &query_graph, const QueryGraphComponent &component,
      QueryGraphPlanningContext *context) const override {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");

    const std::unordered_set<const Predicate *> base_predicates =
        context->Snapshot();
    if (component.pattern_relationship_indices.empty()) {
      const std::vector<std::string> nodes = SortedComponentNodes(component);
      CHECK(nodes.size() == 1, common::InvalidArgumentError,
            Unsupported("multi-node disconnected component logical plan"));
      PlanTable plan_table;
      PutInitialNodeCandidate(query_graph, nodes.front(), base_predicates,
                              context, &plan_table);
      PlanCandidate final_candidate = plan_table.TakeBest({});
      context->Restore(std::move(final_candidate.planned_predicates));
      return std::move(final_candidate.plan);
    }

    PlanTable plan_table;
    const std::vector<std::string> argument_nodes =
        SortedComponentArgumentNodes(component, query_graph.argument_ids);
    if (!argument_nodes.empty()) {
      PutInitialArgumentCandidate(query_graph, argument_nodes, base_predicates,
                                  context, &plan_table);
    } else {
      for (const auto &node : SortedComponentNodes(component)) {
        PutInitialNodeCandidate(query_graph, node, base_predicates, context,
                                &plan_table);
      }
    }

    const std::size_t relationship_count =
        component.pattern_relationship_indices.size();
    for (std::size_t planned_count = 0; planned_count < relationship_count;
         ++planned_count) {
      const std::vector<std::vector<std::size_t>> keys =
          plan_table.KeysWithSize(planned_count);
      for (const auto &key : keys) {
        const PlanCandidate *stored_candidate = plan_table.Best(key);
        if (stored_candidate == nullptr) {
          continue;
        }
        PlanCandidate candidate = CloneCandidate(*stored_candidate);
        for (std::size_t relationship_index :
             component.pattern_relationship_indices) {
          if (ContainsRelationshipIndex(candidate.relationship_indices,
                                        relationship_index)) {
            continue;
          }
          if (!CanExpand(query_graph.pattern_relationships[relationship_index],
                         candidate)) {
            continue;
          }
          plan_table.PutBest(ExpandCandidate(query_graph, candidate,
                                             relationship_index, context));
        }
      }
    }

    PlanCandidate final_candidate =
        plan_table.TakeBest(component.pattern_relationship_indices);
    context->Restore(std::move(final_candidate.planned_predicates));
    return std::move(final_candidate.plan);
  }

 private:
  void PutInitialArgumentCandidate(
      const QueryGraph &query_graph,
      const std::vector<std::string> &argument_nodes,
      const std::unordered_set<const Predicate *> &base_predicates,
      QueryGraphPlanningContext *context, PlanTable *plan_table) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    CHECK(plan_table != nullptr, common::InternalError, "plan table is null");

    context->Restore(base_predicates);
    std::unique_ptr<LogicalPlan> plan =
        std::make_unique<ArgumentPlan>(argument_nodes);
    CostEstimate estimate = cost_model_.EstimateArgument(argument_nodes.size());
    const std::size_t filter_count =
        context->ApplyAvailableFilters(query_graph.selections, &plan);
    plan_table->PutBest(MakePlanCandidate(
        std::move(plan), {},
        ApplyFilterEstimates(estimate, filter_count, cost_model_),
        context->Snapshot()));
  }

  void PutInitialNodeCandidate(
      const QueryGraph &query_graph, const std::string &node,
      const std::unordered_set<const Predicate *> &base_predicates,
      QueryGraphPlanningContext *context, PlanTable *plan_table) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    CHECK(plan_table != nullptr, common::InternalError, "plan table is null");

    context->Restore(base_predicates);
    std::unique_ptr<LogicalPlan> plan =
        context->BuildNodeLeaf(query_graph, node);
    CostEstimate estimate =
        query_graph.argument_ids.contains(node)
            ? cost_model_.EstimateArgument(1)
            : cost_model_.EstimateNodeScan(query_graph.LabelsOnNode(node));
    const std::size_t filter_count =
        context->ApplyAvailableFilters(query_graph.selections, &plan);
    plan_table->PutBest(MakePlanCandidate(
        std::move(plan), {},
        ApplyFilterEstimates(estimate, filter_count, cost_model_),
        context->Snapshot()));
  }

  [[nodiscard]] bool CanExpand(const PatternRelationship &relationship,
                               const PlanCandidate &candidate) const {
    return candidate.covered_symbols.contains(relationship.left_node) ||
           candidate.covered_symbols.contains(relationship.right_node);
  }

  PlanCandidate ExpandCandidate(const QueryGraph &query_graph,
                                const PlanCandidate &input,
                                std::size_t relationship_index,
                                QueryGraphPlanningContext *context) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    PlanCandidate candidate = CloneCandidate(input);
    context->Restore(candidate.planned_predicates);

    const PatternRelationship &relationship =
        query_graph.pattern_relationships[relationship_index];
    const bool left_solved =
        candidate.covered_symbols.contains(relationship.left_node);
    const bool right_solved =
        candidate.covered_symbols.contains(relationship.right_node);
    CHECK(left_solved || right_solved, common::InternalError,
          "relationship is not expandable");

    CostEstimate estimate;
    if (left_solved && right_solved) {
      estimate = cost_model_.EstimateExpandInto(CandidateEstimate(candidate),
                                                relationship.types);
      candidate.plan = std::make_unique<ExpandIntoPlan>(
          std::move(candidate.plan), relationship.left_node,
          relationship.variable, relationship.right_node,
          ToExpandDirection(relationship.direction), relationship.types);
    } else {
      const std::string from_node =
          left_solved ? relationship.left_node : relationship.right_node;
      const std::string to_node =
          left_solved ? relationship.right_node : relationship.left_node;
      const Direction direction = left_solved ? relationship.direction
                                              : Reverse(relationship.direction);
      estimate = cost_model_.EstimateExpand(CandidateEstimate(candidate),
                                            relationship.types);
      candidate.plan = std::make_unique<ExpandPlan>(
          std::move(candidate.plan), from_node, relationship.variable, to_node,
          ToExpandDirection(direction), relationship.types);
    }

    const std::size_t filter_count =
        context->ApplyAvailableFilters(query_graph.selections, &candidate.plan);
    return MakePlanCandidate(
        std::move(candidate.plan),
        WithRelationshipIndex(candidate.relationship_indices,
                              relationship_index),
        ApplyFilterEstimates(estimate, filter_count, cost_model_),
        context->Snapshot());
  }

  CostModel cost_model_;
};

}  // namespace

QueryGraphPlanningContext::QueryGraphPlanningContext(
    std::unordered_set<const Predicate *> *planned_predicates)
    : planned_predicates_(planned_predicates) {
  CHECK(planned_predicates_ != nullptr, common::InternalError,
        "planned predicate set is null");
}

std::unique_ptr<LogicalPlan> QueryGraphPlanningContext::BuildNodeLeaf(
    const QueryGraph &query_graph, std::string_view variable) {
  CHECK(!variable.empty(), common::InvalidArgumentError,
        "node leaf variable is empty");
  if (query_graph.argument_ids.contains(std::string(variable))) {
    return std::make_unique<ArgumentPlan>(
        std::vector<std::string>{std::string(variable)});
  }

  const Predicate *label_predicate =
      FirstConsumableNodeLabelPredicate(query_graph.selections, variable);
  if (label_predicate != nullptr) {
    planned_predicates_->insert(label_predicate);
    return std::make_unique<NodeByLabelScanPlan>(
        std::string(variable), label_predicate->labels.front());
  }
  return std::make_unique<AllNodeScanPlan>(std::string(variable));
}

std::size_t QueryGraphPlanningContext::ApplyAvailableFilters(
    const Selections &selections, std::unique_ptr<LogicalPlan> *plan) {
  CHECK(plan != nullptr && *plan != nullptr, common::InternalError,
        "logical plan is null");
  std::size_t applied_count = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &predicate : selections.predicates) {
      if (planned_predicates_->contains(&predicate)) {
        continue;
      }
      if (!DependenciesMet(predicate.dependencies, (*plan)->SolvedSymbols())) {
        continue;
      }
      CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
            "selection predicate expression is null");
      *plan =
          std::make_unique<FilterPlan>(std::move(*plan), predicate.expression);
      planned_predicates_->insert(&predicate);
      ++applied_count;
      changed = true;
    }
  }
  return applied_count;
}

void QueryGraphPlanningContext::ValidateAllPredicatesPlanned(
    const Selections &selections) const {
  for (const auto &predicate : selections.predicates) {
    CHECK(planned_predicates_->contains(&predicate),
          common::InvalidArgumentError,
          Unsupported("selection predicate with unmet dependencies"));
  }
}

std::unordered_set<const Predicate *> QueryGraphPlanningContext::Snapshot()
    const {
  return *planned_predicates_;
}

void QueryGraphPlanningContext::Restore(
    std::unordered_set<const Predicate *> planned_predicates) {
  *planned_predicates_ = std::move(planned_predicates);
}

const Predicate *QueryGraphPlanningContext::FirstConsumableNodeLabelPredicate(
    const Selections &selections, std::string_view variable) const {
  for (const Predicate *predicate : selections.NodeLabelPredicates(variable)) {
    if (predicate != nullptr && predicate->expression != nullptr &&
        predicate->labels.size() == 1) {
      return predicate;
    }
  }
  return nullptr;
}

std::unique_ptr<ComponentPlanner> MakeComponentPlanner(
    LogicalPlanComponentPlannerKind kind) {
  switch (kind) {
    case LogicalPlanComponentPlannerKind::kRuleBased:
      return std::make_unique<RuleBasedComponentPlanner>();
    case LogicalPlanComponentPlannerKind::kIdp:
      return std::make_unique<IdpComponentPlanner>();
  }
  THROW(common::InternalError, "unknown logical component planner kind");
}

}  // namespace ir
