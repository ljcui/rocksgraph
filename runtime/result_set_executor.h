#pragma once

#include "ir/logical_plan.h"
#include "runtime/execution_context.h"
#include "runtime/query_row.h"

namespace rg {

class ResultSetExecutor final {
 public:
  explicit ResultSetExecutor(ExecutionContext context = {})
      : context_(context) {}
  [[nodiscard]] QueryRows Execute(const ir::SortPlan &plan,
                                  QueryRows rows) const;
  [[nodiscard]] QueryRows Execute(const ir::SkipPlan &plan,
                                  QueryRows rows) const;
  [[nodiscard]] QueryRows Execute(const ir::LimitPlan &plan,
                                  QueryRows rows) const;

 private:
  ExecutionContext context_;
};

}  // namespace rg
