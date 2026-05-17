#include "ir/logical_planner.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/expression_dependency.h"
#include "common/exception.h"

namespace ir {

namespace {

std::string Unsupported(std::string_view feature) {
  return std::string(feature) + " is not supported";
}

std::vector<ProjectItem> BuildProjectItems(const Projection &projection) {
  CHECK(!projection.items.empty(), common::InvalidArgumentError,
        "projection items are empty");
  std::vector<ProjectItem> items;
  items.reserve(projection.items.size());
  for (const auto &item : projection.items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "projection item expression is null");
    CHECK(!item.alias.empty(), common::InvalidArgumentError,
          "projection item alias is empty");
    items.push_back(ProjectItem{item.expression, item.alias});
  }
  return items;
}

std::vector<std::string> BuildProduceColumns(const Projection &projection) {
  std::vector<std::string> columns;
  columns.reserve(projection.items.size());
  for (const auto &item : projection.items) {
    CHECK(!item.alias.empty(), common::InvalidArgumentError,
          "projection item alias is empty");
    columns.push_back(item.alias);
  }
  CHECK(!columns.empty(), common::InvalidArgumentError,
        "produce result columns are empty");
  return columns;
}

std::vector<OrderItem> BuildOrderItems(const Projection &projection) {
  std::vector<OrderItem> items;
  items.reserve(projection.order_by.size());
  for (const auto &item : projection.order_by) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "order by expression is null");
    items.push_back(OrderItem{item.expression, item.ascending});
  }
  return items;
}

void CheckExpressionDependencies(
    const ast::Expression &expression,
    const std::unordered_set<std::string> &available_symbols,
    std::string_view context) {
  const auto dependencies = ast::CollectExpressionDependencies(expression);
  for (const std::string &dependency : dependencies) {
    CHECK(available_symbols.contains(dependency), common::InvalidArgumentError,
          std::string(context) + " dependency is not in scope: " + dependency);
  }
}

std::unique_ptr<LogicalPlan> CloneLogicalPlan(const LogicalPlan &plan) {
  switch (plan.node_type) {
    case LogicalPlanNodeType::kArgument: {
      const auto &typed = static_cast<const Argument &>(plan);
      return std::make_unique<Argument>(typed.argument_ids);
    }
    case LogicalPlanNodeType::kAllNodesScan: {
      const auto &typed = static_cast<const AllNodesScan &>(plan);
      return std::make_unique<AllNodesScan>(typed.id_name, typed.argument_ids);
    }
    case LogicalPlanNodeType::kNodeByLabelScan: {
      const auto &typed = static_cast<const NodeByLabelScan &>(plan);
      return std::make_unique<NodeByLabelScan>(typed.id_name, typed.label,
                                               typed.argument_ids);
    }
    case LogicalPlanNodeType::kExpand: {
      const auto &typed = static_cast<const Expand &>(plan);
      return std::make_unique<Expand>(CloneLogicalPlan(*typed.source),
                                      typed.from, typed.relationship, typed.to,
                                      typed.direction, typed.types);
    }
    case LogicalPlanNodeType::kSelection: {
      const auto &typed = static_cast<const Selection &>(plan);
      return std::make_unique<Selection>(CloneLogicalPlan(*typed.source),
                                         typed.predicates);
    }
    case LogicalPlanNodeType::kUnwind: {
      const auto &typed = static_cast<const Unwind &>(plan);
      return std::make_unique<Unwind>(CloneLogicalPlan(*typed.source),
                                      typed.expression, typed.alias);
    }
    case LogicalPlanNodeType::kProject: {
      const auto &typed = static_cast<const Project &>(plan);
      return std::make_unique<Project>(CloneLogicalPlan(*typed.source),
                                       typed.items, typed.distinct);
    }
    case LogicalPlanNodeType::kSort: {
      const auto &typed = static_cast<const Sort &>(plan);
      return std::make_unique<Sort>(CloneLogicalPlan(*typed.source),
                                    typed.items);
    }
    case LogicalPlanNodeType::kSkip: {
      const auto &typed = static_cast<const Skip &>(plan);
      return std::make_unique<Skip>(CloneLogicalPlan(*typed.source),
                                    typed.count);
    }
    case LogicalPlanNodeType::kLimit: {
      const auto &typed = static_cast<const Limit &>(plan);
      return std::make_unique<Limit>(CloneLogicalPlan(*typed.source),
                                     typed.count);
    }
    case LogicalPlanNodeType::kProduceResult: {
      const auto &typed = static_cast<const ProduceResult &>(plan);
      return std::make_unique<ProduceResult>(CloneLogicalPlan(*typed.source),
                                             typed.columns);
    }
    case LogicalPlanNodeType::kCartesianProduct: {
      const auto &typed = static_cast<const CartesianProduct &>(plan);
      return std::make_unique<CartesianProduct>(CloneLogicalPlan(*typed.left),
                                                CloneLogicalPlan(*typed.right));
    }
    case LogicalPlanNodeType::kNodeHashJoin: {
      const auto &typed = static_cast<const NodeHashJoin &>(plan);
      return std::make_unique<NodeHashJoin>(CloneLogicalPlan(*typed.left),
                                            CloneLogicalPlan(*typed.right),
                                            typed.join_symbols);
    }
    case LogicalPlanNodeType::kUnion: {
      const auto &typed = static_cast<const Union &>(plan);
      return std::make_unique<Union>(CloneLogicalPlan(*typed.left),
                                     CloneLogicalPlan(*typed.right), typed.all);
    }
    case LogicalPlanNodeType::kUnknown:
      break;
  }
  THROW(common::InternalError, "unsupported logical plan clone node type");
}

std::unique_ptr<LogicalPlan> BindArgumentInput(
    std::unique_ptr<LogicalPlan> plan, const LogicalPlan &input_plan) {
  CHECK(plan != nullptr, common::InternalError,
        "logical plan for argument binding is null");

  switch (plan->node_type) {
    case LogicalPlanNodeType::kArgument:
      return CloneLogicalPlan(input_plan);
    case LogicalPlanNodeType::kAllNodesScan:
    case LogicalPlanNodeType::kNodeByLabelScan:
      return plan;
    case LogicalPlanNodeType::kExpand: {
      auto *typed = static_cast<Expand *>(plan.get());
      return std::make_unique<Expand>(
          BindArgumentInput(std::move(typed->source), input_plan), typed->from,
          typed->relationship, typed->to, typed->direction, typed->types);
    }
    case LogicalPlanNodeType::kSelection: {
      auto *typed = static_cast<Selection *>(plan.get());
      return std::make_unique<Selection>(
          BindArgumentInput(std::move(typed->source), input_plan),
          typed->predicates);
    }
    case LogicalPlanNodeType::kUnwind: {
      auto *typed = static_cast<Unwind *>(plan.get());
      return std::make_unique<Unwind>(
          BindArgumentInput(std::move(typed->source), input_plan),
          typed->expression, typed->alias);
    }
    case LogicalPlanNodeType::kProject: {
      auto *typed = static_cast<Project *>(plan.get());
      return std::make_unique<Project>(
          BindArgumentInput(std::move(typed->source), input_plan), typed->items,
          typed->distinct);
    }
    case LogicalPlanNodeType::kSort: {
      auto *typed = static_cast<Sort *>(plan.get());
      return std::make_unique<Sort>(
          BindArgumentInput(std::move(typed->source), input_plan),
          typed->items);
    }
    case LogicalPlanNodeType::kSkip: {
      auto *typed = static_cast<Skip *>(plan.get());
      return std::make_unique<Skip>(
          BindArgumentInput(std::move(typed->source), input_plan),
          typed->count);
    }
    case LogicalPlanNodeType::kLimit: {
      auto *typed = static_cast<Limit *>(plan.get());
      return std::make_unique<Limit>(
          BindArgumentInput(std::move(typed->source), input_plan),
          typed->count);
    }
    case LogicalPlanNodeType::kProduceResult: {
      auto *typed = static_cast<ProduceResult *>(plan.get());
      return std::make_unique<ProduceResult>(
          BindArgumentInput(std::move(typed->source), input_plan),
          typed->columns);
    }
    case LogicalPlanNodeType::kCartesianProduct: {
      auto *typed = static_cast<CartesianProduct *>(plan.get());
      return std::make_unique<CartesianProduct>(
          BindArgumentInput(std::move(typed->left), input_plan),
          BindArgumentInput(std::move(typed->right), input_plan));
    }
    case LogicalPlanNodeType::kNodeHashJoin: {
      auto *typed = static_cast<NodeHashJoin *>(plan.get());
      return std::make_unique<NodeHashJoin>(
          BindArgumentInput(std::move(typed->left), input_plan),
          BindArgumentInput(std::move(typed->right), input_plan),
          typed->join_symbols);
    }
    case LogicalPlanNodeType::kUnion: {
      auto *typed = static_cast<Union *>(plan.get());
      return std::make_unique<Union>(
          BindArgumentInput(std::move(typed->left), input_plan),
          BindArgumentInput(std::move(typed->right), input_plan), typed->all);
    }
    case LogicalPlanNodeType::kUnknown:
      break;
  }
  THROW(common::InternalError, "unsupported logical plan bind node type");
}

std::unique_ptr<LogicalPlan> BuildProjectionHorizon(
    std::unique_ptr<LogicalPlan> plan, const Projection &projection) {
  CHECK(plan != nullptr, common::InternalError,
        "projection horizon input plan is null");

  const auto input_symbols = plan->AvailableSymbols();
  for (const auto &item : projection.items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "projection item expression is null");
    CheckExpressionDependencies(*item.expression, input_symbols, "projection");
  }

  plan = std::make_unique<Project>(
      std::move(plan), BuildProjectItems(projection), projection.distinct);
  const auto projection_symbols = plan->AvailableSymbols();

  if (projection.where != nullptr) {
    CheckExpressionDependencies(*projection.where, projection_symbols,
                                "WITH/RETURN WHERE");
    plan = std::make_unique<Selection>(
        std::move(plan),
        std::vector<const ast::Expression *>{projection.where});
  }

  if (!projection.order_by.empty()) {
    for (const auto &item : projection.order_by) {
      CHECK(item.expression != nullptr, common::InvalidArgumentError,
            "order by expression is null");
      CheckExpressionDependencies(*item.expression, projection_symbols,
                                  "ORDER BY");
    }
    plan = std::make_unique<Sort>(std::move(plan), BuildOrderItems(projection));
  }

  if (projection.skip != nullptr) {
    CheckExpressionDependencies(*projection.skip, projection_symbols, "SKIP");
    plan = std::make_unique<Skip>(std::move(plan), projection.skip);
  }

  if (projection.limit != nullptr) {
    CheckExpressionDependencies(*projection.limit, projection_symbols, "LIMIT");
    plan = std::make_unique<Limit>(std::move(plan), projection.limit);
  }
  return plan;
}

std::unique_ptr<LogicalPlan> BuildUnwindHorizon(
    std::unique_ptr<LogicalPlan> plan, const UnwindHorizon &unwind) {
  CHECK(plan != nullptr, common::InternalError,
        "UNWIND horizon input plan is null");
  CHECK(unwind.expression != nullptr, common::InvalidArgumentError,
        "UNWIND expression is null");
  CHECK(!unwind.alias.empty(), common::InvalidArgumentError,
        "UNWIND alias is empty");

  CheckExpressionDependencies(*unwind.expression, plan->AvailableSymbols(),
                              "UNWIND");
  return std::make_unique<Unwind>(std::move(plan), unwind.expression,
                                  unwind.alias);
}

std::unique_ptr<LogicalPlan> BuildQueryHorizon(
    std::unique_ptr<LogicalPlan> plan, const QueryHorizon &horizon) {
  switch (horizon.kind) {
    case QueryHorizonKind::kProjection:
      return BuildProjectionHorizon(std::move(plan),
                                    horizon.RequireProjection());
    case QueryHorizonKind::kUnwind:
      return BuildUnwindHorizon(std::move(plan), horizon.RequireUnwind());
  }
  THROW(common::InternalError, "unknown query horizon kind");
}

std::unique_ptr<LogicalPlan> BuildQueryPartPipeline(
    const SinglePlannerQuery &query_part,
    const std::unordered_set<std::string> &argument_symbols,
    const LogicalPlannerConfig &config) {
  std::unique_ptr<LogicalPlan> plan =
      BuildIDPLogicalPlan(query_part.query_graph, argument_symbols, config.idp);
  CHECK(plan != nullptr, common::InternalError, "IDP output plan is null");

  return BuildQueryHorizon(std::move(plan), query_part.horizon);
}

struct PlannedPlannerQuery {
  std::unique_ptr<LogicalPlan> plan;
  std::vector<std::string> columns;
};

PlannedPlannerQuery PlanSingleQuery(const SinglePlannerQuery &single_query,
                                    const LogicalPlannerConfig &config) {
  const SinglePlannerQuery *current = &single_query;
  std::unordered_set<std::string> argument_symbols;
  std::unique_ptr<LogicalPlan> previous_output;
  std::unique_ptr<LogicalPlan> plan;

  while (current != nullptr) {
    plan = BuildQueryPartPipeline(*current, argument_symbols, config);
    CHECK(plan != nullptr, common::InternalError, "query part plan is null");
    if (previous_output != nullptr) {
      plan = BindArgumentInput(std::move(plan), *previous_output);
      CHECK(plan != nullptr, common::InternalError,
            "bound query part plan is null");
    }
    argument_symbols = plan->AvailableSymbols();
    previous_output = CloneLogicalPlan(*plan);
    current = current->tail.get();
  }
  CHECK(plan != nullptr, common::InternalError, "single query plan is null");
  return PlannedPlannerQuery{
      .plan = std::move(plan),
      .columns = BuildProduceColumns(
          single_query.Last()->horizon.RequireProjection())};
}

PlannedPlannerQuery PlanPlannerQuery(const PlannerQuery &planner_query,
                                     const LogicalPlannerConfig &config) {
  switch (planner_query.Kind()) {
    case PlannerQueryKind::kSingle:
      return PlanSingleQuery(planner_query.RequireSingle(), config);
    case PlannerQueryKind::kUnion: {
      const UnionPlannerQuery &union_query = planner_query.RequireUnion();
      CHECK(union_query.lhs != nullptr, common::InvalidArgumentError,
            "UNION lhs planner query is null");

      PlannedPlannerQuery planned_lhs =
          PlanPlannerQuery(*union_query.lhs, config);
      CHECK(planned_lhs.plan != nullptr, common::InternalError,
            "UNION lhs logical plan is null");

      PlannedPlannerQuery planned_rhs =
          PlanSingleQuery(union_query.rhs, config);
      CHECK(planned_rhs.plan != nullptr, common::InternalError,
            "UNION rhs logical plan is null");

      CHECK(planned_rhs.columns == planned_lhs.columns,
            common::InvalidArgumentError,
            Unsupported("UNION with different projection columns"));
      return PlannedPlannerQuery{
          .plan = std::make_unique<Union>(std::move(planned_lhs.plan),
                                          std::move(planned_rhs.plan),
                                          union_query.all),
          .columns = std::move(planned_lhs.columns)};
    }
  }
  THROW(common::InternalError, "unknown planner query kind");
}

}  // namespace

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const ast::Statement &statement, const LogicalPlannerConfig &config) {
  std::unique_ptr<PlannerQuery> planner_query = CreatePlannerQuery(statement);
  CHECK(planner_query != nullptr, common::InternalError,
        "planner query is null");
  return BuildLogicalPlan(*planner_query, config);
}

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const PlannerQuery &planner_query, const LogicalPlannerConfig &config) {
  PlannedPlannerQuery planned_query = PlanPlannerQuery(planner_query, config);
  CHECK(planned_query.plan != nullptr, common::InternalError,
        "planner query logical plan is null");
  return std::make_unique<ProduceResult>(std::move(planned_query.plan),
                                         planned_query.columns);
}

}  // namespace ir
