#pragma once

#include <memory>
#include <string_view>

#include "planner/logical_plan_builder.h"

namespace ast {
class Statement;
}  // namespace ast

namespace ir {

// Owns every compilation artifact whose expressions are referenced by the
// query IR and logical plan.
class PlannedQuery final {
 public:
  PlannedQuery(const PlannedQuery &) = delete;
  PlannedQuery &operator=(const PlannedQuery &) = delete;
  PlannedQuery(PlannedQuery &&) noexcept;
  PlannedQuery &operator=(PlannedQuery &&) noexcept;
  ~PlannedQuery();

  [[nodiscard]] const ast::Statement &Ast() const noexcept;
  [[nodiscard]] const QueryIR &Ir() const noexcept;
  [[nodiscard]] const LogicalPlan &Plan() const noexcept;

 private:
  PlannedQuery(std::unique_ptr<ast::Statement> statement,
               std::unique_ptr<QueryIR> query_ir,
               std::unique_ptr<LogicalPlan> logical_plan);

  std::unique_ptr<ast::Statement> statement_;
  std::unique_ptr<QueryIR> query_ir_;
  std::unique_ptr<LogicalPlan> logical_plan_;

  friend PlannedQuery PlanCypher(std::string_view cypher,
                                 const LogicalPlanBuilderOptions &options);
};

[[nodiscard]] PlannedQuery PlanCypher(
    std::string_view cypher, const LogicalPlanBuilderOptions &options = {});

}  // namespace ir
