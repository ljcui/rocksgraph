#pragma once

#include "ir/logical_plan.h"
#include "runtime/query_row.h"

namespace rg {

class JoinExecutor final {
 public:
  [[nodiscard]] QueryRows Execute(const ir::CartesianProductPlan &plan,
                                  const QueryRows &left,
                                  const QueryRows &right) const;
  [[nodiscard]] QueryRows Execute(const ir::NodeHashJoinPlan &plan,
                                  const QueryRows &left,
                                  const QueryRows &right) const;
  [[nodiscard]] QueryRows Execute(const ir::ValueHashJoinPlan &plan,
                                  const QueryRows &left,
                                  const QueryRows &right) const;
  [[nodiscard]] QueryRows Execute(const ir::PredicateJoinPlan &plan,
                                  const QueryRows &left,
                                  const QueryRows &right) const;
};

}  // namespace rg
