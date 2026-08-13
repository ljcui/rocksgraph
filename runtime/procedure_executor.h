#pragma once

#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "storage/access_path.h"

namespace rg {

class ProcedureExecutor final {
 public:
  explicit ProcedureExecutor(const AccessPath &access_path)
      : access_path_(&access_path) {}

  [[nodiscard]] QueryRows Execute(const ir::ProcedureCallPlan &plan,
                                  const QueryRows &input) const;

 private:
  const AccessPath *access_path_ = nullptr;
};

}  // namespace rg
