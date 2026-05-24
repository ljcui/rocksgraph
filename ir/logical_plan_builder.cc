#include "ir/logical_plan_builder.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

std::vector<std::string> Sorted(
    const std::unordered_set<std::string> &symbols) {
  std::vector<std::string> out(symbols.begin(), symbols.end());
  std::sort(out.begin(), out.end());
  return out;
}

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

std::vector<std::string> ProjectionAliases(
    const std::vector<ProjectionItem> &items) {
  std::vector<std::string> aliases;
  aliases.reserve(items.size());
  for (const auto &item : items) {
    aliases.push_back(item.alias);
  }
  return aliases;
}

std::vector<std::string> ProjectionAliases(
    const std::vector<ProjectionItem> &lhs,
    const std::vector<ProjectionItem> &rhs) {
  std::vector<std::string> aliases = ProjectionAliases(lhs);
  aliases.reserve(lhs.size() + rhs.size());
  for (const auto &item : rhs) {
    aliases.push_back(item.alias);
  }
  return aliases;
}

std::vector<LogicalProjectionItem> LogicalProjectionItems(
    const std::vector<ProjectionItem> &items) {
  std::vector<LogicalProjectionItem> logical_items;
  logical_items.reserve(items.size());
  for (const auto &item : items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "projection expression is null");
    CHECK(!item.alias.empty(), common::InvalidArgumentError,
          "projection alias is empty");
    logical_items.push_back(
        {.expression = item.expression, .alias = item.alias});
  }
  return logical_items;
}

LogicalOrderDirection ToLogicalOrderDirection(OrderDirection direction) {
  switch (direction) {
    case OrderDirection::kAscending:
      return LogicalOrderDirection::kAscending;
    case OrderDirection::kDescending:
      return LogicalOrderDirection::kDescending;
  }
  THROW(common::InternalError, "unknown order direction");
}

std::vector<LogicalSortItem> SortItems(const RequiredOrder &required_order) {
  std::vector<LogicalSortItem> items;
  items.reserve(required_order.items.size());
  for (const auto &item : required_order.items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "sort expression is null");
    items.push_back({.expression = item.expression,
                     .direction = ToLogicalOrderDirection(item.direction)});
  }
  return items;
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

bool IsUnsupportedPredicateKind(const Predicate &predicate) {
  return predicate.kind == PredicateKind::kExistsSubquery ||
         predicate.subquery != nullptr || !predicate.nested_expressions.empty();
}

std::vector<std::size_t> NormalizedRelationshipKey(
    std::vector<std::size_t> relationship_indices) {
  std::sort(relationship_indices.begin(), relationship_indices.end());
  relationship_indices.erase(
      std::unique(relationship_indices.begin(), relationship_indices.end()),
      relationship_indices.end());
  return relationship_indices;
}

bool ContainsRelationshipIndex(const std::vector<std::size_t> &indices,
                               std::size_t index) {
  return std::find(indices.begin(), indices.end(), index) != indices.end();
}

std::vector<std::size_t> WithRelationshipIndex(std::vector<std::size_t> indices,
                                               std::size_t index) {
  indices.push_back(index);
  return NormalizedRelationshipKey(std::move(indices));
}

inline constexpr double kInitialEstimatedRows = 1.0;
inline constexpr double kInitialCost = 1.0;

struct CostEstimate {
  double estimated_rows = kInitialEstimatedRows;
  double cost = kInitialCost;
};

struct PlanCandidate {
  std::unique_ptr<LogicalPlan> plan;
  std::vector<std::size_t> relationship_indices;
  std::unordered_set<std::string> covered_symbols;
  std::unordered_set<const Predicate *> planned_predicates;
  double estimated_rows = kInitialEstimatedRows;
  double cost = kInitialCost;
};

class CostModel {
 public:
  [[nodiscard]] CostEstimate EstimateArgument(std::size_t symbol_count) const {
    return {.estimated_rows = 1.0, .cost = 0.1 + 0.01 * symbol_count};
  }

  [[nodiscard]] CostEstimate EstimateNodeScan(
      const std::unordered_set<std::string> &labels) const {
    const double rows = labels.empty() ? 1000.0 : 100.0;
    return {.estimated_rows = rows, .cost = rows};
  }

  [[nodiscard]] CostEstimate EstimateExpand(
      const PlanCandidate &input,
      const std::vector<std::string> &relationship_types) const {
    const double fanout = relationship_types.empty() ? 10.0 : 3.0;
    const double rows = input.estimated_rows * fanout;
    return {.estimated_rows = rows, .cost = input.cost + rows};
  }

  [[nodiscard]] CostEstimate EstimateExpandInto(
      const PlanCandidate &input,
      const std::vector<std::string> &relationship_types) const {
    const double selectivity = relationship_types.empty() ? 0.5 : 0.2;
    const double rows = std::max(1.0, input.estimated_rows * selectivity);
    return {.estimated_rows = rows,
            .cost = input.cost + input.estimated_rows * selectivity};
  }

  [[nodiscard]] CostEstimate ApplyFilter(CostEstimate input) const {
    return {.estimated_rows = std::max(1.0, input.estimated_rows * 0.1),
            .cost = input.cost + input.estimated_rows * 0.1};
  }
};

CostEstimate ApplyFilterEstimates(CostEstimate estimate,
                                  std::size_t filter_count,
                                  const CostModel &cost_model) {
  for (std::size_t i = 0; i < filter_count; ++i) {
    estimate = cost_model.ApplyFilter(estimate);
  }
  return estimate;
}

PlanCandidate MakePlanCandidate(
    std::unique_ptr<LogicalPlan> plan,
    std::vector<std::size_t> relationship_indices, CostEstimate estimate = {},
    std::unordered_set<const Predicate *> planned_predicates = {}) {
  CHECK(plan != nullptr, common::InternalError, "candidate plan is null");

  PlanCandidate candidate;
  candidate.covered_symbols = plan->SolvedSymbols();
  candidate.plan = std::move(plan);
  candidate.relationship_indices =
      NormalizedRelationshipKey(std::move(relationship_indices));
  candidate.planned_predicates = std::move(planned_predicates);
  candidate.estimated_rows = estimate.estimated_rows;
  candidate.cost = estimate.cost;
  return candidate;
}

std::unique_ptr<LogicalPlan> CloneComponentPlan(const LogicalPlan &plan) {
  switch (plan.Type()) {
    case LogicalPlanNodeType::kArgument:
      return std::make_unique<ArgumentPlan>(plan.OutputColumns());
    case LogicalPlanNodeType::kAllNodeScan: {
      const auto &scan = static_cast<const AllNodeScanPlan &>(plan);
      return std::make_unique<AllNodeScanPlan>(scan.Variable());
    }
    case LogicalPlanNodeType::kNodeByLabelScan: {
      const auto &scan = static_cast<const NodeByLabelScanPlan &>(plan);
      return std::make_unique<NodeByLabelScanPlan>(scan.Variable(),
                                                   scan.Label());
    }
    case LogicalPlanNodeType::kExpand: {
      const auto &expand = static_cast<const ExpandPlan &>(plan);
      return std::make_unique<ExpandPlan>(
          CloneComponentPlan(expand.Child(0)), expand.FromNode(),
          expand.Relationship(), expand.ToNode(), expand.Direction(),
          expand.Types());
    }
    case LogicalPlanNodeType::kExpandInto: {
      const auto &expand = static_cast<const ExpandIntoPlan &>(plan);
      return std::make_unique<ExpandIntoPlan>(
          CloneComponentPlan(expand.Child(0)), expand.FromNode(),
          expand.Relationship(), expand.ToNode(), expand.Direction(),
          expand.Types());
    }
    case LogicalPlanNodeType::kFilter: {
      const auto &filter = static_cast<const FilterPlan &>(plan);
      return std::make_unique<FilterPlan>(CloneComponentPlan(filter.Child(0)),
                                          filter.Predicate());
    }
    default:
      THROW(common::InternalError,
            "unsupported component plan clone: " + std::string(plan.Name()));
  }
}

PlanCandidate CloneCandidate(const PlanCandidate &candidate) {
  CHECK(candidate.plan != nullptr, common::InternalError,
        "candidate plan is null");
  PlanCandidate clone;
  clone.plan = CloneComponentPlan(*candidate.plan);
  clone.relationship_indices = candidate.relationship_indices;
  clone.covered_symbols = candidate.covered_symbols;
  clone.planned_predicates = candidate.planned_predicates;
  clone.estimated_rows = candidate.estimated_rows;
  clone.cost = candidate.cost;
  return clone;
}

class PlanTable {
 public:
  void PutBest(PlanCandidate candidate) {
    CHECK(candidate.plan != nullptr, common::InternalError,
          "candidate plan is null");
    candidate.relationship_indices =
        NormalizedRelationshipKey(std::move(candidate.relationship_indices));

    auto found = FindEntry(candidate.relationship_indices);
    if (found == entries_.end()) {
      entries_.push_back(std::move(candidate));
      return;
    }
    if (candidate.cost < found->cost) {
      *found = std::move(candidate);
    }
  }

  PlanCandidate TakeBest(std::vector<std::size_t> relationship_indices) {
    relationship_indices =
        NormalizedRelationshipKey(std::move(relationship_indices));
    auto found = FindEntry(relationship_indices);
    CHECK(found != entries_.end(), common::InternalError,
          "missing plan candidate");
    PlanCandidate candidate = std::move(*found);
    entries_.erase(found);
    return candidate;
  }

  [[nodiscard]] const PlanCandidate *Best(
      std::vector<std::size_t> relationship_indices) const {
    relationship_indices =
        NormalizedRelationshipKey(std::move(relationship_indices));
    auto found = FindEntry(relationship_indices);
    if (found == entries_.end()) {
      return nullptr;
    }
    return &*found;
  }

  [[nodiscard]] std::vector<std::vector<std::size_t>> KeysWithSize(
      std::size_t relationship_count) const {
    std::vector<std::vector<std::size_t>> keys;
    for (const auto &candidate : entries_) {
      if (candidate.relationship_indices.size() == relationship_count) {
        keys.push_back(candidate.relationship_indices);
      }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
  }

 private:
  std::vector<PlanCandidate>::iterator FindEntry(
      const std::vector<std::size_t> &relationship_indices) {
    return std::find_if(
        entries_.begin(), entries_.end(), [&](const PlanCandidate &candidate) {
          return candidate.relationship_indices == relationship_indices;
        });
  }

  std::vector<PlanCandidate>::const_iterator FindEntry(
      const std::vector<std::size_t> &relationship_indices) const {
    return std::find_if(
        entries_.begin(), entries_.end(), [&](const PlanCandidate &candidate) {
          return candidate.relationship_indices == relationship_indices;
        });
  }

  std::vector<PlanCandidate> entries_;
};

class QueryGraphPlanningContext {
 public:
  explicit QueryGraphPlanningContext(
      std::unordered_set<const Predicate *> *planned_predicates)
      : planned_predicates_(planned_predicates) {
    CHECK(planned_predicates_ != nullptr, common::InternalError,
          "planned predicate set is null");
  }

  std::unique_ptr<LogicalPlan> BuildNodeLeaf(const QueryGraph &query_graph,
                                             std::string_view variable) {
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

  std::size_t ApplyAvailableFilters(const Selections &selections,
                                    std::unique_ptr<LogicalPlan> *plan) {
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
        if (!DependenciesMet(predicate.dependencies,
                             (*plan)->SolvedSymbols())) {
          continue;
        }
        CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
              "selection predicate expression is null");
        *plan = std::make_unique<FilterPlan>(std::move(*plan),
                                             predicate.expression);
        planned_predicates_->insert(&predicate);
        ++applied_count;
        changed = true;
      }
    }
    return applied_count;
  }

  void ValidateAllPredicatesPlanned(const Selections &selections) const {
    for (const auto &predicate : selections.predicates) {
      CHECK(planned_predicates_->contains(&predicate),
            common::InvalidArgumentError,
            Unsupported("selection predicate with unmet dependencies"));
    }
  }

  [[nodiscard]] std::unordered_set<const Predicate *> Snapshot() const {
    return *planned_predicates_;
  }

  void Restore(std::unordered_set<const Predicate *> planned_predicates) {
    *planned_predicates_ = std::move(planned_predicates);
  }

 private:
  const Predicate *FirstConsumableNodeLabelPredicate(
      const Selections &selections, std::string_view variable) const {
    for (const Predicate *predicate :
         selections.NodeLabelPredicates(variable)) {
      if (predicate != nullptr && predicate->expression != nullptr &&
          predicate->labels.size() == 1) {
        return predicate;
      }
    }
    return nullptr;
  }

  std::unordered_set<const Predicate *> *planned_predicates_;
};

class ComponentPlanner {
 public:
  ComponentPlanner() = default;
  ComponentPlanner(const ComponentPlanner &) = delete;
  ComponentPlanner &operator=(const ComponentPlanner &) = delete;
  virtual ~ComponentPlanner() = default;

  virtual std::unique_ptr<LogicalPlan> Plan(
      const QueryGraph &query_graph, const QueryGraphComponent &component,
      QueryGraphPlanningContext *context) const = 0;
};

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
        CostEstimate estimate =
            cost_model_.EstimateExpandInto(candidate, relationship.types);
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

      CostEstimate estimate =
          cost_model_.EstimateExpand(candidate, relationship.types);
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
      estimate = cost_model_.EstimateExpandInto(candidate, relationship.types);
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
      estimate = cost_model_.EstimateExpand(candidate, relationship.types);
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

class LogicalPlanBuilder {
 public:
  explicit LogicalPlanBuilder(const LogicalPlanBuilderOptions &options)
      : component_planner_(MakeComponentPlanner(options.component_planner)) {}

  std::unique_ptr<LogicalPlan> Build(const PlannerQuery &planner_query) {
    switch (planner_query.Kind()) {
      case PlannerQueryKind::kSingle:
        return Build(planner_query.RequireSingle());
      case PlannerQueryKind::kUnion:
        THROW(common::InvalidArgumentError, Unsupported("UNION logical plan"));
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  std::unique_ptr<LogicalPlan> Build(const SinglePlannerQuery &planner_query) {
    std::unique_ptr<LogicalPlan> plan =
        BuildQueryGraph(planner_query.query_graph);
    plan = ApplyHorizon(std::move(plan), planner_query.horizon);
    for (const SinglePlannerQuery *tail = planner_query.tail.get();
         tail != nullptr; tail = tail->tail.get()) {
      plan = BuildTailSegment(std::move(plan), *tail);
    }
    return plan;
  }

 private:
  std::unique_ptr<LogicalPlan> BuildTailSegment(
      std::unique_ptr<LogicalPlan> input, const SinglePlannerQuery &segment) {
    CHECK(input != nullptr, common::InternalError, "tail input plan is null");
    ValidateTailArgumentsAvailable(*input, segment.query_graph);

    std::unique_ptr<LogicalPlan> plan = std::move(input);
    if (segment.query_graph.HasLocalWork()) {
      std::unique_ptr<LogicalPlan> rhs =
          BuildQueryGraph(segment.query_graph, false);
      plan = std::make_unique<ApplyPlan>(std::move(plan), std::move(rhs));
      QueryGraphPlanningContext context(&planned_predicates_);
      context.ApplyAvailableFilters(segment.query_graph.selections, &plan);
      context.ValidateAllPredicatesPlanned(segment.query_graph.selections);
    } else {
      planned_predicates_.clear();
      ValidateSupportedQueryGraph(segment.query_graph);
    }
    return ApplyHorizon(std::move(plan), segment.horizon);
  }

  std::unique_ptr<LogicalPlan> BuildQueryGraph(
      const QueryGraph &query_graph, bool validate_all_predicates = true) {
    planned_predicates_.clear();
    ValidateSupportedQueryGraph(query_graph);
    QueryGraphPlanningContext context(&planned_predicates_);

    std::vector<QueryGraphComponent> components =
        query_graph.ConnectedComponents();
    std::unique_ptr<LogicalPlan> plan;
    if (components.empty()) {
      plan = std::make_unique<ArgumentPlan>(Sorted(query_graph.argument_ids));
      context.ApplyAvailableFilters(query_graph.selections, &plan);
      if (validate_all_predicates) {
        context.ValidateAllPredicatesPlanned(query_graph.selections);
      }
      return plan;
    }

    for (const auto &component : components) {
      std::unique_ptr<LogicalPlan> component_plan =
          component_planner_->Plan(query_graph, component, &context);
      context.ApplyAvailableFilters(query_graph.selections, &component_plan);
      if (plan == nullptr) {
        plan = std::move(component_plan);
      } else {
        plan = std::make_unique<CartesianProductPlan>(
            std::move(plan), std::move(component_plan));
        context.ApplyAvailableFilters(query_graph.selections, &plan);
      }
    }

    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    if (validate_all_predicates) {
      context.ValidateAllPredicatesPlanned(query_graph.selections);
    }
    return plan;
  }

  void ValidateTailArgumentsAvailable(const LogicalPlan &input,
                                      const QueryGraph &query_graph) const {
    for (const auto &argument : query_graph.argument_ids) {
      CHECK(StringVectorContains(input.OutputColumns(), argument),
            common::InvalidArgumentError,
            "tail argument is not available: " + argument);
    }
  }

  void ValidateSupportedQueryGraph(const QueryGraph &query_graph) const {
    CHECK(query_graph.pattern_paths.empty(), common::InvalidArgumentError,
          Unsupported("named path logical plan"));
    CHECK(query_graph.optional_matches.empty(), common::InvalidArgumentError,
          Unsupported("OPTIONAL MATCH logical plan"));
    CHECK(query_graph.hints.empty(), common::InvalidArgumentError,
          Unsupported("planner hint logical plan"));
    CHECK(query_graph.mutating_patterns.empty(), common::InvalidArgumentError,
          Unsupported("updating logical plan"));
    CHECK(query_graph.assert_is_node_variables.empty(),
          common::InvalidArgumentError,
          Unsupported("argument node assertion logical plan"));

    for (const auto &relationship : query_graph.pattern_relationships) {
      CHECK(!relationship.length.variable, common::InvalidArgumentError,
            Unsupported("variable-length expand logical plan"));
    }
    for (const auto &predicate : query_graph.selections.predicates) {
      CHECK(!IsUnsupportedPredicateKind(predicate),
            common::InvalidArgumentError,
            Unsupported("nested predicate logical plan"));
    }
  }

  std::unique_ptr<LogicalPlan> ApplyHorizon(std::unique_ptr<LogicalPlan> plan,
                                            const QueryHorizon &horizon) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    switch (horizon.kind) {
      case QueryHorizonKind::kRegularProjection:
        return ApplyRegularProjection(std::move(plan),
                                      horizon.RequireRegularProjection());
      case QueryHorizonKind::kDistinctProjection:
        return ApplyDistinctProjection(std::move(plan),
                                       horizon.RequireDistinctProjection());
      case QueryHorizonKind::kAggregatingProjection:
        return ApplyAggregatingProjection(
            std::move(plan), horizon.RequireAggregatingProjection());
      case QueryHorizonKind::kUnwind:
        THROW(common::InvalidArgumentError, Unsupported("UNWIND logical plan"));
      case QueryHorizonKind::kProcedureCall:
        THROW(common::InvalidArgumentError,
              Unsupported("procedure call logical plan"));
      case QueryHorizonKind::kPassthrough:
        return plan;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  std::unique_ptr<LogicalPlan> ApplyRegularProjection(
      std::unique_ptr<LogicalPlan> plan,
      const RegularQueryProjection &projection) {
    std::vector<std::string> aliases = ProjectionAliases(projection.items);
    plan = std::make_unique<ProjectionPlan>(
        std::move(plan), LogicalProjectionItems(projection.items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyDistinctProjection(
      std::unique_ptr<LogicalPlan> plan,
      const DistinctQueryProjection &projection) {
    std::vector<std::string> aliases =
        ProjectionAliases(projection.grouping_items);
    plan = std::make_unique<DistinctPlan>(
        std::move(plan), LogicalProjectionItems(projection.grouping_items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyAggregatingProjection(
      std::unique_ptr<LogicalPlan> plan,
      const AggregatingQueryProjection &projection) {
    std::vector<std::string> aliases = ProjectionAliases(
        projection.grouping_items, projection.aggregation_items);
    plan = std::make_unique<AggregationPlan>(
        std::move(plan), LogicalProjectionItems(projection.grouping_items),
        LogicalProjectionItems(projection.aggregation_items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyProjectionTail(
      std::unique_ptr<LogicalPlan> plan, const QueryProjection &projection,
      std::vector<std::string> aliases) {
    CHECK(projection.selections.empty(), common::InvalidArgumentError,
          Unsupported("projection selection logical plan"));
    CHECK(projection.nested_expressions.empty(), common::InvalidArgumentError,
          Unsupported("nested projection expression logical plan"));

    if (!projection.required_order.empty()) {
      plan = std::make_unique<SortPlan>(std::move(plan),
                                        SortItems(projection.required_order));
    }
    if (projection.pagination.skip != nullptr) {
      plan = std::make_unique<SkipPlan>(std::move(plan),
                                        projection.pagination.skip);
    }
    if (projection.pagination.limit != nullptr) {
      plan = std::make_unique<LimitPlan>(std::move(plan),
                                         projection.pagination.limit);
    }
    if (projection.position == ProjectionPosition::kFinal) {
      plan = std::make_unique<ProduceResultsPlan>(std::move(plan),
                                                  std::move(aliases));
    }
    return plan;
  }

  std::unique_ptr<ComponentPlanner> component_planner_;
  std::unordered_set<const Predicate *> planned_predicates_;
};

}  // namespace

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query) {
  return CreateLogicalPlan(planner_query, LogicalPlanBuilderOptions{});
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query) {
  return CreateLogicalPlan(planner_query, LogicalPlanBuilderOptions{});
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query,
    const LogicalPlanBuilderOptions &options) {
  LogicalPlanBuilder builder(options);
  return builder.Build(planner_query);
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query,
    const LogicalPlanBuilderOptions &options) {
  LogicalPlanBuilder builder(options);
  return builder.Build(planner_query);
}

}  // namespace ir
