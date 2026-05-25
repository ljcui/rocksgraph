#include "ir/planner/logical_plan_builder.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "ir/planner/component_planner.h"
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

std::vector<LogicalUnionMapping> LogicalUnionMappings(
    const std::vector<UnionPlannerQuery::UnionMapping> &mappings) {
  std::vector<LogicalUnionMapping> logical_mappings;
  logical_mappings.reserve(mappings.size());
  for (const auto &mapping : mappings) {
    CHECK(!mapping.output_variable.empty(), common::InvalidArgumentError,
          "UNION output variable is empty");
    logical_mappings.push_back({.output_variable = mapping.output_variable,
                                .lhs_variable = mapping.lhs_variable,
                                .rhs_variable = mapping.rhs_variable});
  }
  return logical_mappings;
}

class LogicalPlanBuilder {
 public:
  explicit LogicalPlanBuilder(const LogicalPlanBuilderOptions &options)
      : options_(options), component_planner_(MakeComponentPlanner(options)) {}

  std::unique_ptr<LogicalPlan> Build(const PlannerQuery &planner_query) {
    switch (planner_query.Kind()) {
      case PlannerQueryKind::kSingle:
        return Build(planner_query.RequireSingle());
      case PlannerQueryKind::kUnion:
        return BuildUnion(planner_query.RequireUnion(),
                          /*produce_results=*/true);
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
  std::unique_ptr<LogicalPlan> BuildUnionInput(
      const PlannerQuery &planner_query) {
    switch (planner_query.Kind()) {
      case PlannerQueryKind::kSingle:
        return Build(planner_query.RequireSingle());
      case PlannerQueryKind::kUnion:
        return BuildUnion(planner_query.RequireUnion(),
                          /*produce_results=*/false);
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  std::unique_ptr<LogicalPlan> BuildUnion(const UnionPlannerQuery &union_query,
                                          bool produce_results) {
    CHECK(union_query.lhs != nullptr, common::InvalidArgumentError,
          "UNION lhs planner query is null");
    std::vector<LogicalUnionMapping> mappings =
        LogicalUnionMappings(union_query.mappings);
    std::vector<std::string> output_columns;
    output_columns.reserve(mappings.size());
    for (const auto &mapping : mappings) {
      output_columns.push_back(mapping.output_variable);
    }
    std::unique_ptr<LogicalPlan> plan = std::make_unique<UnionPlan>(
        BuildUnionInput(*union_query.lhs), Build(union_query.rhs),
        std::move(mappings), union_query.all);
    if (produce_results) {
      plan = std::make_unique<ProduceResultsPlan>(std::move(plan),
                                                  std::move(output_columns));
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> BuildTailSegment(
      std::unique_ptr<LogicalPlan> input, const SinglePlannerQuery &segment) {
    CHECK(input != nullptr, common::InternalError, "tail input plan is null");
    ValidateTailArgumentsAvailable(*input, segment.query_graph);

    std::unique_ptr<LogicalPlan> plan = std::move(input);
    if (segment.query_graph.HasLocalWork()) {
      std::unique_ptr<LogicalPlan> rhs =
          BuildQueryGraph(segment.query_graph, false);
      plan = std::make_unique<ApplyPlan>(std::move(plan), std::move(rhs));
      QueryGraphPlanningContext context(&planned_predicates_, &options_);
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
    QueryGraphPlanningContext context(&planned_predicates_, &options_);

    std::vector<QueryGraphComponent> components =
        query_graph.ConnectedComponents();
    std::unique_ptr<LogicalPlan> plan;
    if (components.empty()) {
      plan = std::make_unique<ArgumentPlan>(Sorted(query_graph.argument_ids));
      context.ApplyAvailableFilters(query_graph.selections, &plan);
    } else {
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
    }

    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    plan = ApplyPathBuilds(std::move(plan), query_graph.pattern_paths);
    plan = ApplyAssertIsNode(std::move(plan),
                             query_graph.assert_is_node_variables);
    context.ApplyAvailableFilters(query_graph.selections, &plan);
    plan = ApplyOptionalMatches(std::move(plan), query_graph, &context);
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
    CHECK(query_graph.hints.empty(), common::InvalidArgumentError,
          Unsupported("planner hint logical plan"));
    CHECK(query_graph.mutating_patterns.empty(), common::InvalidArgumentError,
          Unsupported("updating logical plan"));
    for (const auto &predicate : query_graph.selections.predicates) {
      if (predicate.kind == PredicateKind::kExistsSubquery) {
        CHECK(predicate.subquery != nullptr, common::InvalidArgumentError,
              "EXISTS predicate subquery is null");
      }
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
        return ApplyUnwind(std::move(plan), horizon.RequireUnwind());
      case QueryHorizonKind::kProcedureCall:
        THROW(common::InvalidArgumentError,
              Unsupported("procedure call logical plan"));
      case QueryHorizonKind::kPassthrough:
        return plan;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  std::unique_ptr<LogicalPlan> ApplyPathBuilds(
      std::unique_ptr<LogicalPlan> plan,
      const std::unordered_set<LogicalVariable> &path_variables) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    for (const auto &path_variable : Sorted(path_variables)) {
      plan = std::make_unique<PathBuildPlan>(std::move(plan), path_variable);
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyAssertIsNode(
      std::unique_ptr<LogicalPlan> plan,
      const std::unordered_set<LogicalVariable> &variables) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    if (variables.empty()) {
      return plan;
    }
    return std::make_unique<AssertIsNodePlan>(std::move(plan),
                                              Sorted(variables));
  }

  std::unique_ptr<LogicalPlan> ApplyOptionalMatches(
      std::unique_ptr<LogicalPlan> plan, const QueryGraph &query_graph,
      QueryGraphPlanningContext *context) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    for (const auto &optional_match : query_graph.optional_matches) {
      std::unordered_set<const Predicate *> outer_predicates =
          planned_predicates_;
      std::unique_ptr<LogicalPlan> optional_plan =
          BuildQueryGraph(optional_match);
      planned_predicates_ = std::move(outer_predicates);
      plan = std::make_unique<OptionalApplyPlan>(std::move(plan),
                                                 std::move(optional_plan));
      context->ApplyAvailableFilters(query_graph.selections, &plan);
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyRegularProjection(
      std::unique_ptr<LogicalPlan> plan,
      const RegularQueryProjection &projection) {
    plan =
        ApplyNestedExpressions(std::move(plan), projection.nested_expressions);
    std::vector<std::string> aliases = ProjectionAliases(projection.items);
    plan = std::make_unique<ProjectionPlan>(
        std::move(plan), LogicalProjectionItems(projection.items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyDistinctProjection(
      std::unique_ptr<LogicalPlan> plan,
      const DistinctQueryProjection &projection) {
    plan =
        ApplyNestedExpressions(std::move(plan), projection.nested_expressions);
    std::vector<std::string> aliases =
        ProjectionAliases(projection.grouping_items);
    plan = std::make_unique<DistinctPlan>(
        std::move(plan), LogicalProjectionItems(projection.grouping_items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyAggregatingProjection(
      std::unique_ptr<LogicalPlan> plan,
      const AggregatingQueryProjection &projection) {
    plan =
        ApplyNestedExpressions(std::move(plan), projection.nested_expressions);
    std::vector<std::string> aliases = ProjectionAliases(
        projection.grouping_items, projection.aggregation_items);
    plan = std::make_unique<AggregationPlan>(
        std::move(plan), LogicalProjectionItems(projection.grouping_items),
        LogicalProjectionItems(projection.aggregation_items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyUnwind(std::unique_ptr<LogicalPlan> plan,
                                           const UnwindHorizon &unwind) {
    CHECK(unwind.expression != nullptr, common::InvalidArgumentError,
          "UNWIND expression is null");
    CHECK(!unwind.alias.empty(), common::InvalidArgumentError,
          "UNWIND alias is empty");
    return std::make_unique<UnwindPlan>(std::move(plan), unwind.expression,
                                        unwind.alias);
  }

  std::unique_ptr<LogicalPlan> ApplyNestedExpressions(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<NestedIRExpression> &nested_expressions) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    std::unordered_set<const Predicate *> planned_predicates;
    QueryGraphPlanningContext context(&planned_predicates, &options_);
    context.ApplyNestedExpressions(nested_expressions, &plan);
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyProjectionSelections(
      std::unique_ptr<LogicalPlan> plan, const Selections &selections) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    std::unordered_set<const Predicate *> planned_predicates;
    QueryGraphPlanningContext context(&planned_predicates, &options_);
    context.ApplyAvailableFilters(selections, &plan);
    context.ValidateAllPredicatesPlanned(selections);
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyProjectionTail(
      std::unique_ptr<LogicalPlan> plan, const QueryProjection &projection,
      std::vector<std::string> aliases) {
    plan = ApplyProjectionSelections(std::move(plan), projection.selections);

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

  LogicalPlanBuilderOptions options_;
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
