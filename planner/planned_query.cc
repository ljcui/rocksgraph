#include "planner/planned_query.h"

#include <string>
#include <utility>

#include "ast/ast_builder.h"
#include "common/exception.h"
#include "ir/query_ir.h"

namespace ir {

PlannedQuery::PlannedQuery(std::unique_ptr<ast::Statement> statement,
                           std::unique_ptr<QueryIR> query_ir,
                           std::unique_ptr<LogicalPlan> logical_plan)
    : statement_(std::move(statement)),
      query_ir_(std::move(query_ir)),
      logical_plan_(std::move(logical_plan)) {
  CHECK(statement_ != nullptr, common::InternalError,
        "planned query statement is null");
  CHECK(query_ir_ != nullptr, common::InternalError,
        "planned query IR is null");
  CHECK(logical_plan_ != nullptr, common::InternalError,
        "planned query logical plan is null");
}

PlannedQuery::PlannedQuery(PlannedQuery &&) noexcept = default;

PlannedQuery &PlannedQuery::operator=(PlannedQuery &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  logical_plan_.reset();
  query_ir_.reset();
  statement_.reset();
  statement_ = std::move(other.statement_);
  query_ir_ = std::move(other.query_ir_);
  logical_plan_ = std::move(other.logical_plan_);
  return *this;
}

PlannedQuery::~PlannedQuery() = default;

const ast::Statement &PlannedQuery::Ast() const noexcept { return *statement_; }

const QueryIR &PlannedQuery::Ir() const noexcept { return *query_ir_; }

const LogicalPlan &PlannedQuery::Plan() const noexcept {
  return *logical_plan_;
}

PlannedQuery PlanCypher(std::string_view cypher,
                        const LogicalPlanBuilderOptions &options) {
  auto statement = ast::ParseCypherAndRewrite(std::string(cypher));
  auto query_ir = CreateQueryIR(*statement);
  auto logical_plan = CreateLogicalPlan(*query_ir, options);
  return PlannedQuery(std::move(statement), std::move(query_ir),
                      std::move(logical_plan));
}

}  // namespace ir
