#pragma once

#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "storage/graph_reader.h"

namespace rg {

class ProcedureExecutor final {
 public:
  explicit ProcedureExecutor(const GraphReader &graph_reader)
      : graph_reader_(&graph_reader) {}

  [[nodiscard]] QueryRows Execute(const ir::ProcedureCallPlan &plan,
                                  const QueryRows &input) const;

 private:
  const GraphReader *graph_reader_ = nullptr;
};

}  // namespace rg
