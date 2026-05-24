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

class LogicalPlanBuilder {
 public:
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
    CHECK(planner_query.tail == nullptr, common::InvalidArgumentError,
          Unsupported("tail logical plan"));

    planned_predicates_.clear();
    std::unique_ptr<LogicalPlan> plan =
        BuildQueryGraph(planner_query.query_graph);
    plan = ApplyHorizon(std::move(plan), planner_query.horizon);
    return plan;
  }

 private:
  std::unique_ptr<LogicalPlan> BuildQueryGraph(const QueryGraph &query_graph) {
    ValidateSupportedQueryGraph(query_graph);

    std::vector<QueryGraphComponent> components =
        query_graph.ConnectedComponents();
    std::unique_ptr<LogicalPlan> plan;
    if (components.empty()) {
      plan = std::make_unique<ArgumentPlan>(Sorted(query_graph.argument_ids));
      ApplyAvailableFilters(query_graph.selections, &plan);
      ValidateAllPredicatesPlanned(query_graph.selections);
      return plan;
    }

    for (const auto &component : components) {
      std::unique_ptr<LogicalPlan> component_plan =
          BuildComponent(query_graph, component);
      ApplyAvailableFilters(query_graph.selections, &component_plan);
      if (plan == nullptr) {
        plan = std::move(component_plan);
      } else {
        plan = std::make_unique<CartesianProductPlan>(
            std::move(plan), std::move(component_plan));
        ApplyAvailableFilters(query_graph.selections, &plan);
      }
    }

    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    ValidateAllPredicatesPlanned(query_graph.selections);
    return plan;
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

  std::unique_ptr<LogicalPlan> BuildComponent(
      const QueryGraph &query_graph, const QueryGraphComponent &component) {
    if (component.pattern_relationship_indices.empty()) {
      const std::vector<std::string> nodes = SortedComponentNodes(component);
      CHECK(nodes.size() == 1, common::InvalidArgumentError,
            Unsupported("multi-node disconnected component logical plan"));
      return BuildNodeLeaf(query_graph, nodes.front());
    }

    const PatternRelationship &first_relationship =
        query_graph
            .pattern_relationships[component.pattern_relationship_indices[0]];
    std::string start_node = first_relationship.left_node;
    for (const auto &node : SortedComponentNodes(component)) {
      if (query_graph.argument_ids.contains(node)) {
        start_node = node;
        break;
      }
    }

    std::unique_ptr<LogicalPlan> plan = BuildNodeLeaf(query_graph, start_node);
    ApplyAvailableFilters(query_graph.selections, &plan);

    std::vector<std::size_t> remaining = component.pattern_relationship_indices;
    while (!remaining.empty()) {
      auto found = std::find_if(
          remaining.begin(), remaining.end(), [&](std::size_t index) {
            const PatternRelationship &relationship =
                query_graph.pattern_relationships[index];
            return plan->SolvedSymbols().contains(relationship.left_node) ||
                   plan->SolvedSymbols().contains(relationship.right_node);
          });
      CHECK(found != remaining.end(), common::InvalidArgumentError,
            Unsupported("disconnected relationship expansion logical plan"));

      const PatternRelationship &relationship =
          query_graph.pattern_relationships[*found];
      const bool left_solved =
          plan->SolvedSymbols().contains(relationship.left_node);
      const std::string from_node =
          left_solved ? relationship.left_node : relationship.right_node;
      const std::string to_node =
          left_solved ? relationship.right_node : relationship.left_node;
      const Direction direction = left_solved ? relationship.direction
                                              : Reverse(relationship.direction);

      plan = std::make_unique<ExpandPlan>(
          std::move(plan), from_node, relationship.variable, to_node,
          ToExpandDirection(direction), relationship.types);
      remaining.erase(found);
      ApplyAvailableFilters(query_graph.selections, &plan);
    }

    return plan;
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
      planned_predicates_.insert(label_predicate);
      return std::make_unique<NodeByLabelScanPlan>(
          std::string(variable), label_predicate->labels.front());
    }
    return std::make_unique<AllNodeScanPlan>(std::string(variable));
  }

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

  void ApplyAvailableFilters(const Selections &selections,
                             std::unique_ptr<LogicalPlan> *plan) {
    CHECK(plan != nullptr && *plan != nullptr, common::InternalError,
          "logical plan is null");
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &predicate : selections.predicates) {
        if (planned_predicates_.contains(&predicate)) {
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
        planned_predicates_.insert(&predicate);
        changed = true;
      }
    }
  }

  void ValidateAllPredicatesPlanned(const Selections &selections) const {
    for (const auto &predicate : selections.predicates) {
      CHECK(planned_predicates_.contains(&predicate),
            common::InvalidArgumentError,
            Unsupported("selection predicate with unmet dependencies"));
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

  std::unordered_set<const Predicate *> planned_predicates_;
};

}  // namespace

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query) {
  LogicalPlanBuilder builder;
  return builder.Build(planner_query);
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query) {
  LogicalPlanBuilder builder;
  return builder.Build(planner_query);
}

}  // namespace ir
