#include "planner/component_planner.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "ir/query_ir_internal.h"
#include "planner/cost_model.h"
#include "planner/idp.h"
#include "planner/plan_clone.h"

namespace ir {
namespace {

constexpr std::string_view kLogicalPlanStage = "logical plan";

const PlannerCatalog &PlannerCatalogFor(
    const LogicalPlanBuilderOptions &options) {
  static const HeuristicPlannerCatalog kDefaultCatalog;
  return options.planner_catalog == nullptr ? kDefaultCatalog
                                            : *options.planner_catalog;
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

std::vector<std::string> SortedUniqueStrings(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::unordered_set<std::string> SingleSymbolSet(std::string_view symbol) {
  std::unordered_set<std::string> out;
  if (!symbol.empty()) {
    out.emplace(symbol);
  }
  return out;
}

bool PredicateDependsOnlyOn(const Predicate &predicate,
                            std::string_view variable) {
  return DependenciesMet(predicate.dependencies, SingleSymbolSet(variable));
}

std::vector<const ast::Expression *> PredicateExpressions(
    const std::vector<const Predicate *> &predicates) {
  std::vector<const ast::Expression *> expressions;
  expressions.reserve(predicates.size());
  for (const Predicate *predicate : predicates) {
    CHECK(predicate != nullptr && predicate->expression != nullptr,
          common::InvalidArgumentError, "predicate expression is null");
    expressions.push_back(predicate->expression);
  }
  return expressions;
}

std::vector<LogicalPrecomputedExpression> PrecomputedExpressions(
    const std::vector<NestedIRExpression> &nested_expressions) {
  std::vector<LogicalPrecomputedExpression> out;
  out.reserve(nested_expressions.size());
  for (const auto &nested : nested_expressions) {
    if (nested.expression == nullptr) {
      continue;
    }
    switch (nested.kind) {
      case NestedIRExpressionKind::kExists:
        if (!nested.value_variable.empty()) {
          out.push_back({.expression = nested.expression,
                         .variable = nested.value_variable});
        }
        break;
      case NestedIRExpressionKind::kList:
        if (!nested.collection_variable.empty()) {
          out.push_back({.expression = nested.expression,
                         .variable = nested.collection_variable});
        }
        break;
    }
  }
  return out;
}

const ast::Expression *PredicateValueExpression(const Predicate &predicate) {
  CHECK(predicate.property_value != nullptr, common::InvalidArgumentError,
        "index seek predicate value is null");
  return predicate.property_value;
}

void AddRangePredicateGroup(std::vector<std::vector<const Predicate *>> *groups,
                            std::vector<const Predicate *> predicates) {
  CHECK(groups != nullptr, common::InternalError,
        "range predicate groups are null");
  if (predicates.empty()) {
    return;
  }
  const std::string &property_key = predicates.front()->property_key;
  for (auto &group : *groups) {
    CHECK(!group.empty(), common::InternalError,
          "range predicate group is empty");
    if (group.front()->property_key == property_key) {
      group.insert(group.end(), predicates.begin(), predicates.end());
      return;
    }
  }
  groups->push_back(std::move(predicates));
}

std::vector<std::string> LabelsFromPredicates(
    const std::vector<const Predicate *> &predicates) {
  std::vector<std::string> labels;
  for (const Predicate *predicate : predicates) {
    CHECK(predicate != nullptr, common::InternalError, "predicate is null");
    labels.insert(labels.end(), predicate->labels.begin(),
                  predicate->labels.end());
  }
  return SortedUniqueStrings(std::move(labels));
}

std::vector<std::string> IntersectSortedStrings(
    const std::vector<std::string> &lhs, const std::vector<std::string> &rhs) {
  std::vector<std::string> result;
  std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                        std::back_inserter(result));
  return result;
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

LogicalVariableLength ToLogicalVariableLength(const PatternLength &length) {
  CHECK(length.variable, common::InvalidArgumentError,
        "relationship length is not variable");
  return {.min = length.min, .max = length.max};
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

bool RelationshipSetsDisjoint(const std::vector<std::size_t> &lhs,
                              const std::vector<std::size_t> &rhs) {
  for (std::size_t index : lhs) {
    if (ContainsRelationshipIndex(rhs, index)) {
      return false;
    }
  }
  return true;
}

std::vector<std::size_t> MergeRelationshipIndices(
    std::vector<std::size_t> lhs, const std::vector<std::size_t> &rhs) {
  lhs.insert(lhs.end(), rhs.begin(), rhs.end());
  return NormalizedRelationshipKey(std::move(lhs));
}

std::unordered_set<const Predicate *> UnionPredicates(
    const std::unordered_set<const Predicate *> &lhs,
    const std::unordered_set<const Predicate *> &rhs) {
  std::unordered_set<const Predicate *> out = lhs;
  out.insert(rhs.begin(), rhs.end());
  return out;
}

std::vector<std::string> SharedNodeSymbols(const QueryGraph &query_graph,
                                           const PlanCandidate &lhs,
                                           const PlanCandidate &rhs) {
  std::vector<std::string> keys;
  for (const auto &node : query_graph.pattern_nodes) {
    if (lhs.covered_symbols.contains(node) &&
        rhs.covered_symbols.contains(node)) {
      keys.push_back(node);
    }
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

CostEstimate EstimateLeafPlan(const LogicalPlan &plan,
                              const CostModel &cost_model) {
  switch (plan.Type()) {
    case LogicalPlanNodeType::kArgument:
      return cost_model.EstimateArgument(plan.OutputColumns().size());
    case LogicalPlanNodeType::kAllNodeScan:
      return cost_model.EstimateNodeScan({});
    case LogicalPlanNodeType::kNodeByLabelScan: {
      const auto &scan = static_cast<const NodeByLabelScanPlan &>(plan);
      return cost_model.EstimateNodeScan(std::unordered_set<std::string>(
          scan.Labels().begin(), scan.Labels().end()));
    }
    case LogicalPlanNodeType::kNodeIndexSeek: {
      const auto &seek = static_cast<const NodeIndexSeekPlan &>(plan);
      return cost_model.EstimateNodeIndexSeek(
          std::unordered_set<std::string>(seek.Labels().begin(),
                                          seek.Labels().end()),
          seek.PropertyKey(), seek.Unique());
    }
    case LogicalPlanNodeType::kNodeIndexRangeSeek: {
      const auto &seek = static_cast<const NodeIndexRangeSeekPlan &>(plan);
      return cost_model.EstimateNodeIndexRangeSeek(
          std::unordered_set<std::string>(seek.Labels().begin(),
                                          seek.Labels().end()),
          seek.PropertyKey(), seek.Predicates().size());
    }
    case LogicalPlanNodeType::kRelationshipTypeScan: {
      const auto &scan = static_cast<const RelationshipTypeScanPlan &>(plan);
      return cost_model.EstimateRelationshipTypeScan(scan.Types());
    }
    case LogicalPlanNodeType::kRelationshipIndexSeek: {
      const auto &seek = static_cast<const RelationshipIndexSeekPlan &>(plan);
      return cost_model.EstimateRelationshipIndexSeek(
          seek.Types(), seek.PropertyKey(), seek.Unique());
    }
    case LogicalPlanNodeType::kRelationshipIndexRangeSeek: {
      const auto &seek =
          static_cast<const RelationshipIndexRangeSeekPlan &>(plan);
      return cost_model.EstimateRelationshipIndexRangeSeek(
          seek.Types(), seek.PropertyKey(), seek.Predicates().size());
    }
    default:
      THROW(common::InternalError,
            "unsupported leaf estimate: " + std::string(plan.Name()));
  }
}

class IdpComponentPlanner final : public ComponentPlanner {
 public:
  explicit IdpComponentPlanner(
      std::size_t max_candidates_per_relationship_count,
      const PlannerStatistics *statistics)
      : cost_model_(statistics),
        max_candidates_per_relationship_count_(
            max_candidates_per_relationship_count) {
    CHECK(max_candidates_per_relationship_count_ > 0,
          common::InvalidArgumentError,
          "max IDP candidates per relationship count must be positive");
  }

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
            UnsupportedInStage(kLogicalPlanStage,
                               "multi-node disconnected component"));
      PlanTable plan_table;
      PutInitialNodeCandidate(query_graph, nodes.front(), base_predicates,
                              context, &plan_table);
      const std::vector<PlanKey> keys = plan_table.KeysWithRelationshipCount(0);
      CHECK(keys.size() == 1, common::InternalError,
            "expected one leaf plan candidate");
      PlanCandidate final_candidate = plan_table.TakeBest(keys.front());
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
    PruneCandidates(0, &plan_table);
    PutInitialRelationshipCandidates(query_graph, component, base_predicates,
                                     context, &plan_table);

    const std::size_t relationship_count =
        component.pattern_relationship_indices.size();
    for (std::size_t target_count = 1; target_count <= relationship_count;
         ++target_count) {
      PutExpandCandidates(query_graph, component, target_count - 1, context,
                          &plan_table);
      PutJoinCandidates(query_graph, target_count, context, &plan_table);
      PruneCandidates(target_count, &plan_table);
    }

    PlanCandidate final_candidate = plan_table.TakeBest(
        BestKeyWithRelationshipCount(plan_table, relationship_count));
    context->Restore(std::move(final_candidate.planned_predicates));
    return std::move(final_candidate.plan);
  }

 private:
  [[nodiscard]] PlanKey BestKeyWithRelationshipCount(
      const PlanTable &plan_table, std::size_t relationship_count) const {
    bool found = false;
    PlanKey best_key;
    double best_cost = 0.0;
    for (const PlanKey &key :
         plan_table.KeysWithRelationshipCount(relationship_count)) {
      const PlanCandidate *candidate = plan_table.Best(key);
      if (candidate == nullptr) {
        continue;
      }
      if (!found || candidate->cost < best_cost ||
          (candidate->cost == best_cost && key < best_key)) {
        found = true;
        best_key = key;
        best_cost = candidate->cost;
      }
    }
    CHECK(found, common::InternalError, "missing final plan candidate");
    return best_key;
  }

  void PruneCandidates(std::size_t relationship_count,
                       PlanTable *plan_table) const {
    CHECK(plan_table != nullptr, common::InternalError, "plan table is null");
    plan_table->PruneRelationshipCount(relationship_count,
                                       max_candidates_per_relationship_count_);
  }

  void PutExpandCandidates(const QueryGraph &query_graph,
                           const QueryGraphComponent &component,
                           std::size_t input_relationship_count,
                           QueryGraphPlanningContext *context,
                           PlanTable *plan_table) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    CHECK(plan_table != nullptr, common::InternalError, "plan table is null");

    const std::vector<PlanKey> keys =
        plan_table->KeysWithRelationshipCount(input_relationship_count);
    for (const auto &key : keys) {
      const PlanCandidate *stored_candidate = plan_table->Best(key);
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
        plan_table->PutBest(ExpandCandidate(query_graph, candidate,
                                            relationship_index, context));
      }
    }
  }

  void PutJoinCandidates(const QueryGraph &query_graph,
                         std::size_t target_relationship_count,
                         QueryGraphPlanningContext *context,
                         PlanTable *plan_table) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    CHECK(plan_table != nullptr, common::InternalError, "plan table is null");

    for (std::size_t left_count = 1; left_count < target_relationship_count;
         ++left_count) {
      const std::size_t right_count = target_relationship_count - left_count;
      const std::vector<PlanKey> left_keys =
          plan_table->KeysWithRelationshipCount(left_count);
      const std::vector<PlanKey> right_keys =
          plan_table->KeysWithRelationshipCount(right_count);
      for (const auto &left_key : left_keys) {
        for (const auto &right_key : right_keys) {
          if (!(left_key < right_key)) {
            continue;
          }
          const PlanCandidate *left = plan_table->Best(left_key);
          const PlanCandidate *right = plan_table->Best(right_key);
          if (left == nullptr || right == nullptr) {
            continue;
          }
          if (!RelationshipSetsDisjoint(left->relationship_indices,
                                        right->relationship_indices)) {
            continue;
          }
          const std::vector<std::string> join_keys =
              SharedNodeSymbols(query_graph, *left, *right);
          if (join_keys.empty()) {
            continue;
          }
          plan_table->PutBest(
              JoinCandidates(query_graph, *left, *right, join_keys, context));
        }
      }
    }
  }

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
    std::vector<LeafPlanCandidate> leaf_candidates =
        context->BuildNodeLeafCandidates(query_graph, node);
    CHECK(!leaf_candidates.empty(), common::InternalError,
          "missing node leaf candidate");
    for (auto &leaf_candidate : leaf_candidates) {
      CHECK(leaf_candidate.plan != nullptr, common::InternalError,
            "node leaf candidate plan is null");
      context->Restore(std::move(leaf_candidate.planned_predicates));
      CostEstimate estimate =
          EstimateLeafPlan(*leaf_candidate.plan, cost_model_);
      const std::size_t filter_count = context->ApplyAvailableFilters(
          query_graph.selections, &leaf_candidate.plan);
      plan_table->PutBest(MakePlanCandidate(
          std::move(leaf_candidate.plan), {},
          ApplyFilterEstimates(estimate, filter_count, cost_model_),
          context->Snapshot()));
    }
  }

  void PutInitialRelationshipCandidates(
      const QueryGraph &query_graph, const QueryGraphComponent &component,
      const std::unordered_set<const Predicate *> &base_predicates,
      QueryGraphPlanningContext *context, PlanTable *plan_table) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    CHECK(plan_table != nullptr, common::InternalError, "plan table is null");

    for (std::size_t relationship_index :
         component.pattern_relationship_indices) {
      context->Restore(base_predicates);
      std::vector<LeafPlanCandidate> leaf_candidates =
          context->BuildRelationshipLeafCandidates(query_graph,
                                                   relationship_index);
      for (auto &leaf_candidate : leaf_candidates) {
        CHECK(leaf_candidate.plan != nullptr, common::InternalError,
              "relationship leaf candidate plan is null");
        context->Restore(std::move(leaf_candidate.planned_predicates));
        CostEstimate estimate =
            EstimateLeafPlan(*leaf_candidate.plan, cost_model_);
        const std::size_t filter_count = context->ApplyAvailableFilters(
            query_graph.selections, &leaf_candidate.plan);
        plan_table->PutBest(MakePlanCandidate(
            std::move(leaf_candidate.plan), {relationship_index},
            ApplyFilterEstimates(estimate, filter_count, cost_model_),
            context->Snapshot()));
      }
    }
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
    const std::vector<std::string> relationship_types =
        context->ConsumeRelationshipTypes(query_graph.selections, relationship);

    CostEstimate estimate;
    if (relationship.length.variable) {
      const std::string from_node =
          left_solved ? relationship.left_node : relationship.right_node;
      const std::string to_node =
          left_solved ? relationship.right_node : relationship.left_node;
      const Direction direction = left_solved ? relationship.direction
                                              : Reverse(relationship.direction);
      estimate = left_solved && right_solved
                     ? cost_model_.EstimateExpandInto(
                           CandidateEstimate(candidate), relationship_types)
                     : cost_model_.EstimateExpand(CandidateEstimate(candidate),
                                                  relationship_types);
      candidate.plan = std::make_unique<VarExpandPlan>(
          std::move(candidate.plan), from_node, relationship.variable, to_node,
          ToExpandDirection(direction), relationship_types,
          ToLogicalVariableLength(relationship.length));
    } else if (left_solved && right_solved) {
      estimate = cost_model_.EstimateExpandInto(CandidateEstimate(candidate),
                                                relationship_types);
      candidate.plan = std::make_unique<ExpandIntoPlan>(
          std::move(candidate.plan), relationship.left_node,
          relationship.variable, relationship.right_node,
          ToExpandDirection(relationship.direction), relationship_types);
    } else {
      const std::string from_node =
          left_solved ? relationship.left_node : relationship.right_node;
      const std::string to_node =
          left_solved ? relationship.right_node : relationship.left_node;
      const Direction direction = left_solved ? relationship.direction
                                              : Reverse(relationship.direction);
      estimate = cost_model_.EstimateExpand(CandidateEstimate(candidate),
                                            relationship_types);
      candidate.plan = std::make_unique<ExpandPlan>(
          std::move(candidate.plan), from_node, relationship.variable, to_node,
          ToExpandDirection(direction), relationship_types);
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

  PlanCandidate JoinCandidates(const QueryGraph &query_graph,
                               const PlanCandidate &left_input,
                               const PlanCandidate &right_input,
                               const std::vector<std::string> &join_keys,
                               QueryGraphPlanningContext *context) const {
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    PlanCandidate left = CloneCandidate(left_input);
    PlanCandidate right = CloneCandidate(right_input);
    std::unordered_set<const Predicate *> planned_predicates =
        UnionPredicates(left.planned_predicates, right.planned_predicates);
    context->Restore(planned_predicates);

    CostEstimate estimate = cost_model_.EstimateNodeHashJoin(
        CandidateEstimate(left), CandidateEstimate(right), join_keys.size());
    std::unique_ptr<LogicalPlan> plan = std::make_unique<NodeHashJoinPlan>(
        std::move(left.plan), std::move(right.plan), join_keys);
    const std::size_t filter_count =
        context->ApplyAvailableFilters(query_graph.selections, &plan);
    return MakePlanCandidate(
        std::move(plan),
        MergeRelationshipIndices(left.relationship_indices,
                                 right.relationship_indices),
        ApplyFilterEstimates(estimate, filter_count, cost_model_),
        context->Snapshot());
  }

  CostModel cost_model_;
  std::size_t max_candidates_per_relationship_count_ = 128;
};

}  // namespace

QueryGraphPlanningContext::QueryGraphPlanningContext(
    std::unordered_set<const Predicate *> *planned_predicates,
    const LogicalPlanBuilderOptions *options)
    : planned_predicates_(planned_predicates), options_(options) {
  CHECK(planned_predicates_ != nullptr, common::InternalError,
        "planned predicate set is null");
  CHECK(options_ != nullptr, common::InternalError,
        "logical plan builder options are null");
}

std::vector<LeafPlanCandidate>
QueryGraphPlanningContext::BuildNodeLeafCandidates(
    const QueryGraph &query_graph, std::string_view variable) {
  CHECK(!variable.empty(), common::InvalidArgumentError,
        "node leaf variable is empty");
  const std::unordered_set<const Predicate *> base_predicates = Snapshot();
  std::vector<LeafPlanCandidate> candidates;

  if (query_graph.argument_ids.contains(std::string(variable))) {
    auto plan = std::make_unique<ArgumentPlan>(
        std::vector<std::string>{std::string(variable)});
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
    return candidates;
  }

  const std::vector<const Predicate *> label_predicates =
      ConsumableNodeLabelPredicates(query_graph.selections, variable);
  const std::vector<std::string> labels =
      LabelsFromPredicates(label_predicates);

  for (const IndexedPredicate &seek : IndexSeekPredicates(
           query_graph.selections, variable, IndexEntityKind::kNode, labels)) {
    CHECK(seek.predicate != nullptr, common::InternalError,
          "node index seek predicate is null");
    Restore(base_predicates);
    MarkPlanned(label_predicates);
    planned_predicates_->insert(seek.predicate);
    auto plan = std::make_unique<NodeIndexSeekPlan>(
        std::string(variable), labels, seek.index.property_key,
        PredicateValueExpression(*seek.predicate), seek.index.unique);
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
  }

  for (const std::vector<const Predicate *> &range_predicates :
       IndexRangePredicateGroups(query_graph.selections, variable,
                                 IndexEntityKind::kNode, labels)) {
    Restore(base_predicates);
    const std::string property_key = range_predicates.front()->property_key;
    MarkPlanned(label_predicates);
    MarkPlanned(range_predicates);
    auto plan = std::make_unique<NodeIndexRangeSeekPlan>(
        std::string(variable), labels, property_key,
        PredicateExpressions(range_predicates));
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
  }

  Restore(base_predicates);
  if (!labels.empty()) {
    MarkPlanned(label_predicates);
    auto plan =
        std::make_unique<NodeByLabelScanPlan>(std::string(variable), labels);
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
    return candidates;
  }
  auto plan = std::make_unique<AllNodeScanPlan>(std::string(variable));
  candidates.push_back(
      {.plan = std::move(plan), .planned_predicates = Snapshot()});
  return candidates;
}

std::vector<LeafPlanCandidate>
QueryGraphPlanningContext::BuildRelationshipLeafCandidates(
    const QueryGraph &query_graph, std::size_t relationship_index) {
  CHECK(relationship_index < query_graph.pattern_relationships.size(),
        common::InvalidArgumentError, "relationship leaf index out of range");
  std::vector<LeafPlanCandidate> candidates;
  const PatternRelationship &relationship =
      query_graph.pattern_relationships[relationship_index];
  if (relationship.length.variable) {
    return candidates;
  }

  const std::unordered_set<const Predicate *> base_predicates = Snapshot();
  std::vector<std::string> types =
      ConsumeRelationshipTypes(query_graph.selections, relationship);
  const std::unordered_set<const Predicate *> type_predicates = Snapshot();

  for (const IndexedPredicate &seek :
       IndexSeekPredicates(query_graph.selections, relationship.variable,
                           IndexEntityKind::kRelationship, types)) {
    CHECK(seek.predicate != nullptr, common::InternalError,
          "relationship index seek predicate is null");
    Restore(type_predicates);
    planned_predicates_->insert(seek.predicate);
    auto plan = std::make_unique<RelationshipIndexSeekPlan>(
        relationship.left_node, relationship.variable, relationship.right_node,
        ToExpandDirection(relationship.direction), types,
        seek.index.property_key, PredicateValueExpression(*seek.predicate),
        seek.index.unique);
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
  }

  for (const std::vector<const Predicate *> &range_predicates :
       IndexRangePredicateGroups(query_graph.selections, relationship.variable,
                                 IndexEntityKind::kRelationship, types)) {
    Restore(type_predicates);
    const std::string property_key = range_predicates.front()->property_key;
    MarkPlanned(range_predicates);
    auto plan = std::make_unique<RelationshipIndexRangeSeekPlan>(
        relationship.left_node, relationship.variable, relationship.right_node,
        ToExpandDirection(relationship.direction), types, property_key,
        PredicateExpressions(range_predicates));
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
  }

  Restore(type_predicates);
  if (!types.empty()) {
    auto plan = std::make_unique<RelationshipTypeScanPlan>(
        relationship.left_node, relationship.variable, relationship.right_node,
        ToExpandDirection(relationship.direction), std::move(types));
    candidates.push_back(
        {.plan = std::move(plan), .planned_predicates = Snapshot()});
  }
  Restore(base_predicates);
  return candidates;
}

std::vector<std::string> QueryGraphPlanningContext::ConsumeRelationshipTypes(
    const Selections &selections, const PatternRelationship &relationship) {
  std::vector<std::string> types = SortedUniqueStrings(relationship.types);
  bool constrained = !types.empty();

  for (const Predicate *predicate : ConsumableRelationshipTypePredicates(
           selections, relationship.variable)) {
    CHECK(predicate != nullptr, common::InternalError, "predicate is null");
    std::vector<std::string> predicate_types =
        SortedUniqueStrings(predicate->relationship_types);
    if (predicate_types.empty()) {
      continue;
    }
    if (!constrained) {
      types = std::move(predicate_types);
      constrained = true;
      planned_predicates_->insert(predicate);
      continue;
    }
    std::vector<std::string> intersection =
        IntersectSortedStrings(types, predicate_types);
    if (!intersection.empty()) {
      types = std::move(intersection);
      planned_predicates_->insert(predicate);
    }
  }
  return types;
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
      if (predicate.kind == PredicateKind::kExistsSubquery) {
        CHECK(predicate.subquery != nullptr, common::InvalidArgumentError,
              "EXISTS predicate subquery is null");
        *plan = std::make_unique<SemiApplyPlan>(
            std::move(*plan), BuildNestedPlan(*predicate.subquery));
      } else if (predicate.kind == PredicateKind::kNotExistsSubquery) {
        CHECK(predicate.subquery != nullptr, common::InvalidArgumentError,
              "NOT EXISTS predicate subquery is null");
        *plan = std::make_unique<AntiSemiApplyPlan>(
            std::move(*plan), BuildNestedPlan(*predicate.subquery));
      } else {
        ApplyNestedExpressions(predicate.nested_expressions, plan);
        *plan = std::make_unique<FilterPlan>(
            std::move(*plan), predicate.expression,
            PrecomputedExpressions(predicate.nested_expressions));
      }
      planned_predicates_->insert(&predicate);
      ++applied_count;
      changed = true;
    }
  }
  return applied_count;
}

void QueryGraphPlanningContext::ApplyNestedExpressions(
    const std::vector<NestedIRExpression> &nested_expressions,
    std::unique_ptr<LogicalPlan> *plan) const {
  CHECK(plan != nullptr && *plan != nullptr, common::InternalError,
        "logical plan is null");
  for (const auto &nested : nested_expressions) {
    CHECK(nested.query != nullptr, common::InvalidArgumentError,
          "nested IR expression query is null");
    switch (nested.kind) {
      case NestedIRExpressionKind::kExists:
        CHECK(!nested.value_variable.empty(), common::InvalidArgumentError,
              "EXISTS nested value variable is empty");
        *plan = std::make_unique<LetSemiApplyPlan>(
            std::move(*plan), BuildNestedPlan(*nested.query),
            nested.value_variable);
        break;
      case NestedIRExpressionKind::kList:
        CHECK(!nested.collection_variable.empty(), common::InvalidArgumentError,
              "list nested collection variable is empty");
        CHECK(!nested.value_variable.empty(), common::InvalidArgumentError,
              "list nested value variable is empty");
        *plan = std::make_unique<RollUpApplyPlan>(
            std::move(*plan), BuildNestedPlan(*nested.query),
            nested.collection_variable, nested.value_variable);
        break;
    }
  }
}

void QueryGraphPlanningContext::ValidateAllPredicatesPlanned(
    const Selections &selections) const {
  for (const auto &predicate : selections.predicates) {
    CHECK(planned_predicates_->contains(&predicate),
          common::InvalidArgumentError,
          UnsupportedInStage(kLogicalPlanStage,
                             "selection predicate with unmet dependencies"));
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

std::vector<const Predicate *>
QueryGraphPlanningContext::ConsumableNodeLabelPredicates(
    const Selections &selections, std::string_view variable) const {
  std::vector<const Predicate *> out;
  for (const Predicate *predicate : selections.NodeLabelPredicates(variable)) {
    if (predicate != nullptr && predicate->expression != nullptr &&
        PredicateDependsOnlyOn(*predicate, variable) &&
        !predicate->labels.empty()) {
      out.push_back(predicate);
    }
  }
  return out;
}

std::vector<const Predicate *>
QueryGraphPlanningContext::ConsumableRelationshipTypePredicates(
    const Selections &selections, std::string_view variable) const {
  std::vector<const Predicate *> out;
  for (const Predicate *predicate :
       selections.RelationshipTypePredicates(variable)) {
    if (predicate != nullptr && predicate->expression != nullptr &&
        PredicateDependsOnlyOn(*predicate, variable) &&
        !predicate->relationship_types.empty()) {
      out.push_back(predicate);
    }
  }
  return out;
}

std::optional<QueryGraphPlanningContext::IndexDescriptor>
QueryGraphPlanningContext::FindIndex(IndexEntityKind entity_kind,
                                     const std::vector<std::string> &qualifiers,
                                     std::string_view property_key) const {
  if (property_key.empty()) {
    return std::nullopt;
  }
  const PlannerCatalog &catalog = PlannerCatalogFor(*options_);
  switch (entity_kind) {
    case IndexEntityKind::kNode: {
      std::optional<NodeIndexDescriptor> descriptor =
          catalog.FindNodeIndex(qualifiers, property_key);
      if (!descriptor.has_value()) {
        return std::nullopt;
      }
      return IndexDescriptor{.property_key = descriptor->property_key,
                             .unique = descriptor->unique};
    }
    case IndexEntityKind::kRelationship: {
      std::optional<RelationshipIndexDescriptor> descriptor =
          catalog.FindRelationshipIndex(qualifiers, property_key);
      if (!descriptor.has_value()) {
        return std::nullopt;
      }
      return IndexDescriptor{.property_key = descriptor->property_key,
                             .unique = descriptor->unique};
    }
  }
  THROW(common::InternalError, "unknown index entity kind");
}

std::vector<QueryGraphPlanningContext::IndexedPredicate>
QueryGraphPlanningContext::IndexSeekPredicates(
    const Selections &selections, std::string_view variable,
    IndexEntityKind entity_kind,
    const std::vector<std::string> &qualifiers) const {
  std::vector<IndexedPredicate> out;
  for (const auto &predicate : selections.predicates) {
    if (predicate.kind != PredicateKind::kPropertyEquality ||
        !StringEquals(predicate.variable, variable) ||
        predicate.property_key.empty() || predicate.expression == nullptr ||
        !PredicateDependsOnlyOn(predicate, variable)) {
      continue;
    }
    if (predicate.property_value == nullptr) {
      continue;
    }
    std::optional<IndexDescriptor> index =
        FindIndex(entity_kind, qualifiers, predicate.property_key);
    if (!index.has_value()) {
      continue;
    }
    out.push_back({.predicate = &predicate, .index = std::move(*index)});
  }
  std::stable_sort(
      out.begin(), out.end(),
      [](const IndexedPredicate &lhs, const IndexedPredicate &rhs) {
        CHECK(lhs.predicate != nullptr && rhs.predicate != nullptr,
              common::InternalError, "index seek predicate is null");
        return lhs.predicate->property_key < rhs.predicate->property_key;
      });
  return out;
}

std::vector<std::vector<const Predicate *>>
QueryGraphPlanningContext::IndexRangePredicateGroups(
    const Selections &selections, std::string_view variable,
    IndexEntityKind entity_kind,
    const std::vector<std::string> &qualifiers) const {
  std::vector<std::vector<const Predicate *>> groups;
  for (PropertyInequalityGroup group : selections.PropertyInequalityGroups()) {
    if (!StringEquals(group.variable, variable) || group.property_key.empty()) {
      continue;
    }
    if (!FindIndex(entity_kind, qualifiers, group.property_key).has_value()) {
      continue;
    }
    std::vector<const Predicate *> predicates;
    predicates.insert(predicates.end(), group.lower_bounds.begin(),
                      group.lower_bounds.end());
    predicates.insert(predicates.end(), group.upper_bounds.begin(),
                      group.upper_bounds.end());
    predicates.erase(std::remove_if(predicates.begin(), predicates.end(),
                                    [&](const Predicate *predicate) {
                                      return predicate == nullptr ||
                                             predicate->expression == nullptr ||
                                             !PredicateDependsOnlyOn(*predicate,
                                                                     variable);
                                    }),
                     predicates.end());
    if (predicates.empty()) {
      continue;
    }
    AddRangePredicateGroup(&groups, std::move(predicates));
  }
  for (const auto &predicate : selections.predicates) {
    if (predicate.kind != PredicateKind::kPropertyStringPredicate ||
        !StringEquals(predicate.variable, variable) ||
        predicate.property_key.empty() || predicate.property_value == nullptr ||
        predicate.expression == nullptr ||
        !StringEquals(predicate.comparison_op, "STARTS WITH") ||
        !PredicateDependsOnlyOn(predicate, variable) ||
        !FindIndex(entity_kind, qualifiers, predicate.property_key)
             .has_value()) {
      continue;
    }
    AddRangePredicateGroup(&groups, {&predicate});
  }
  std::stable_sort(groups.begin(), groups.end(),
                   [](const std::vector<const Predicate *> &lhs,
                      const std::vector<const Predicate *> &rhs) {
                     CHECK(!lhs.empty() && !rhs.empty(), common::InternalError,
                           "index range predicate group is empty");
                     return lhs.front()->property_key <
                            rhs.front()->property_key;
                   });
  return groups;
}

void QueryGraphPlanningContext::MarkPlanned(
    const std::vector<const Predicate *> &predicates) {
  for (const Predicate *predicate : predicates) {
    if (predicate != nullptr) {
      planned_predicates_->insert(predicate);
    }
  }
}

std::unique_ptr<LogicalPlan> QueryGraphPlanningContext::BuildNestedPlan(
    const QueryIR &query) const {
  CHECK(options_ != nullptr, common::InternalError,
        "logical plan builder options are null");
  return CreateLogicalPlan(query, *options_);
}

std::unique_ptr<ComponentPlanner> MakeComponentPlanner(
    const LogicalPlanBuilderOptions &options) {
  return std::make_unique<IdpComponentPlanner>(
      options.max_idp_candidates_per_relationship_count,
      options.planner_statistics);
}

}  // namespace ir
