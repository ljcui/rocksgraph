#pragma once

#include <functional>

#include "ir/logical_plan.h"
#include "runtime/query_row.h"

namespace rg {

using ApplyPlanExecutor =
    std::function<QueryRows(const ir::LogicalPlan &, const QueryRows &)>;

class ApplyExecutor final {
 public:
  [[nodiscard]] QueryRows Execute(const ir::ApplyPlan &plan,
                                  const QueryRows &input,
                                  const ApplyPlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::SemiApplyPlan &plan,
                                  const QueryRows &input,
                                  const ApplyPlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::AntiSemiApplyPlan &plan,
                                  const QueryRows &input,
                                  const ApplyPlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::LetSemiApplyPlan &plan,
                                  const QueryRows &input,
                                  const ApplyPlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::RollUpApplyPlan &plan,
                                  const QueryRows &input,
                                  const ApplyPlanExecutor &execute_plan) const;
  [[nodiscard]] QueryRows Execute(const ir::OptionalApplyPlan &plan,
                                  const QueryRows &input,
                                  const ApplyPlanExecutor &execute_plan) const;
};

}  // namespace rg
