#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "runtime/execution_context.h"
#include "runtime/physical_plan.h"
#include "storage/storage.h"
#include "value/value.h"

namespace rg {

class PhysicalResultCursor {
 public:
  PhysicalResultCursor() = default;
  PhysicalResultCursor(const PhysicalResultCursor &) = delete;
  PhysicalResultCursor &operator=(const PhysicalResultCursor &) = delete;
  virtual ~PhysicalResultCursor() = default;

  [[nodiscard]] virtual bool Next(std::vector<Value> *row) = 0;
  virtual void Cancel() noexcept = 0;
  virtual void Close() noexcept = 0;
  [[nodiscard]] virtual std::size_t PeakMemoryBytes() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<PhysicalResultCursor> StartPhysicalPlan(
    const PhysicalPlan &plan, const GraphReader &graph_reader, Storage *storage,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns,
    QueryExecutionOptions options = {});

}  // namespace rg
