#include "ir/logical_plan_builder.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "ir/logical_plan_component_planner.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

std::vector<std::string> Sorted(
    const std::unordered_set<std::string> &symbols) {
  std::vector<std::string> out(symbols.begin(), symbols.end());
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

bool IsUnsupportedPredicateKind(const Predicate &predicate) {
  return predicate.kind == PredicateKind::kExistsSubquery ||
         predicate.subquery != nullptr || !predicate.nested_expressions.empty();
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
