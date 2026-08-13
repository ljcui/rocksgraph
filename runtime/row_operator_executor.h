#pragma once

#include "ir/logical_plan.h"
#include "runtime/query_row.h"

namespace rg {

class RowOperatorExecutor final {
 public:
  [[nodiscard]] QueryRows Execute(const ir::ArgumentPlan &plan,
                                  const QueryRows &input) const;
  [[nodiscard]] QueryRows Execute(const ir::FilterPlan &plan,
                                  QueryRows rows) const;
  [[nodiscard]] QueryRows Execute(const ir::ProjectionPlan &plan,
                                  const QueryRows &rows) const;
  [[nodiscard]] QueryRows Execute(const ir::DistinctPlan &plan,
                                  const QueryRows &rows) const;
  [[nodiscard]] QueryRows Execute(const ir::AggregationPlan &plan,
                                  const QueryRows &rows) const;
  [[nodiscard]] QueryRows Execute(const ir::UnwindPlan &plan,
                                  const QueryRows &rows) const;
  [[nodiscard]] QueryRows Execute(const ir::UnionPlan &plan,
                                  const QueryRows &left_rows,
                                  const QueryRows &right_rows) const;
};

}  // namespace rg
