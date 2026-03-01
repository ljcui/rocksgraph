#include "ir/logical_planner.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
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

class VariableDependencyCollector : public ast::ASTConstWalker {
 public:
  explicit VariableDependencyCollector(
      std::unordered_set<std::string> *dependencies)
      : dependencies_(dependencies) {
    CHECK(dependencies_ != nullptr, common::InternalError,
          "dependencies output is null");
  }

  void Collect(const ast::Expression &expression) { expression.Accept(*this); }

 protected:
  void Visit(const ast::Variable &node) override {
    CHECK(!node.name.empty(), common::InvalidArgumentError,
          "variable dependency name is empty");
    dependencies_->emplace(node.name);
  }

 private:
  std::unordered_set<std::string> *dependencies_;
};

std::unordered_set<std::string> CollectDependencies(
    const ast::Expression &expression) {
  std::unordered_set<std::string> dependencies;
  VariableDependencyCollector collector(&dependencies);
  collector.Collect(expression);
  return dependencies;
}

void CheckExpressionDependencies(
    const ast::Expression &expression,
    const std::unordered_set<std::string> &available_symbols,
    std::string_view context) {
  const auto dependencies = CollectDependencies(expression);
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

std::unique_ptr<LogicalPlan> BuildQueryPartPipeline(
    const SingleQueryIR &query_part,
    const std::unordered_set<std::string> &argument_symbols,
    const LogicalPlannerConfig &config) {
  std::unique_ptr<LogicalPlan> plan =
      BuildIDPLogicalPlan(query_part.query_graph, argument_symbols, config.idp);
  CHECK(plan != nullptr, common::InternalError, "IDP output plan is null");

  plan = std::make_unique<Project>(std::move(plan),
                                   BuildProjectItems(query_part.projection),
                                   query_part.projection.distinct);
  const auto projection_symbols = plan->AvailableSymbols();

  if (query_part.projection.where != nullptr) {
    CheckExpressionDependencies(*query_part.projection.where,
                                projection_symbols, "WITH/RETURN WHERE");
    plan = std::make_unique<Selection>(
        std::move(plan),
        std::vector<const ast::Expression *>{query_part.projection.where});
  }

  if (!query_part.projection.order_by.empty()) {
    for (const auto &item : query_part.projection.order_by) {
      CHECK(item.expression != nullptr, common::InvalidArgumentError,
            "order by expression is null");
      CheckExpressionDependencies(*item.expression, projection_symbols,
                                  "ORDER BY");
    }
    plan = std::make_unique<Sort>(std::move(plan),
                                  BuildOrderItems(query_part.projection));
  }

  if (query_part.projection.skip != nullptr) {
    CheckExpressionDependencies(*query_part.projection.skip, projection_symbols,
                                "SKIP");
    plan = std::make_unique<Skip>(std::move(plan), query_part.projection.skip);
  }

  if (query_part.projection.limit != nullptr) {
    CheckExpressionDependencies(*query_part.projection.limit,
                                projection_symbols, "LIMIT");
    plan =
        std::make_unique<Limit>(std::move(plan), query_part.projection.limit);
  }
  return plan;
}

struct PlannedSingleQuery {
  std::unique_ptr<LogicalPlan> plan;
  std::vector<std::string> columns;
};

PlannedSingleQuery PlanSingleQuery(const SingleQueryIR &single_query,
                                   const LogicalPlannerConfig &config) {
  const SingleQueryIR *current = &single_query;
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
  return PlannedSingleQuery{
      .plan = std::move(plan),
      .columns = BuildProduceColumns(single_query.Last()->projection)};
}

}  // namespace

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const ast::Statement &statement, const LogicalPlannerConfig &config) {
  QueryIR query_ir = BuildStatement(statement);
  return BuildLogicalPlan(query_ir, config);
}

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const QueryIR &query_ir, const LogicalPlannerConfig &config) {
  PlannedSingleQuery planned_main =
      PlanSingleQuery(query_ir.regular.main, config);
  CHECK(planned_main.plan != nullptr, common::InternalError,
        "main query logical plan is null");

  std::unique_ptr<LogicalPlan> combined = std::move(planned_main.plan);
  std::vector<std::string> output_columns = planned_main.columns;

  for (const auto &branch : query_ir.regular.unions) {
    PlannedSingleQuery planned_branch = PlanSingleQuery(branch.query, config);
    CHECK(planned_branch.plan != nullptr, common::InternalError,
          "union branch logical plan is null");
    CHECK(planned_branch.columns == output_columns,
          common::InvalidArgumentError,
          Unsupported("UNION with different projection columns"));
    combined = std::make_unique<Union>(
        std::move(combined), std::move(planned_branch.plan), branch.all);
  }

  return std::make_unique<ProduceResult>(std::move(combined), output_columns);
}

}  // namespace ir
