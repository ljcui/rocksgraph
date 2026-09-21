#include "runtime/slotted_executor.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "runtime/slotted_executor_internal.h"

namespace rg {
namespace {

class PhysicalResultCursorImpl final : public PhysicalResultCursor {
 public:
  PhysicalResultCursorImpl(const PhysicalPlan &plan,
                           graphdb::Transaction &transaction,
                           const QueryParameters &parameters,
                           std::vector<std::string> result_columns,
                           QueryExecutionOptions options)
      : state_(transaction, parameters, std::move(options)),
        factory_(state_),
        root_(factory_.Build(plan.Root())),
        output_slots_(plan.Root().output_slots),
        slotted_(output_slots_) {
    result_slots_.reserve(result_columns.size());
    for (const auto &column : result_columns) {
      result_slots_.push_back(output_slots_->At(column));
    }
  }

  ~PhysicalResultCursorImpl() override { Close(); }

  [[nodiscard]] bool Next(std::vector<Value> *row) override {
    RG_CHECK(row != nullptr, common::ErrorCode::InvalidParameter,
             "result row is null");
    if (closed_) {
      return false;
    }
    try {
      state_.CheckCancelled();
      slotted_.Reset();
      if (!root_->Next(&slotted_)) {
        Close();
        return false;
      }
      row->clear();
      row->reserve(result_slots_.size());
      for (const std::size_t slot : result_slots_) {
        row->push_back(slotted_.IsInitialized(slot)
                           ? slotted::ReadRowValue(slotted_, slot)
                           : Value::Null());
      }
      return true;
    } catch (...) {
      Close();
      throw;
    }
  }

  void Cancel() noexcept override { state_.cancellation->Cancel(); }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (root_ != nullptr) {
      root_->Close();
    }
    peak_memory_bytes_ = state_.memory_tracker.PeakBytes();
    closed_ = true;
  }

  [[nodiscard]] std::size_t PeakMemoryBytes() const noexcept override {
    return closed_ ? peak_memory_bytes_ : state_.memory_tracker.PeakBytes();
  }

 private:
  slotted::RuntimeState state_;
  slotted::OperatorFactory factory_;
  std::unique_ptr<slotted::PullOperator> root_;
  SlotConfigurationPtr output_slots_;
  SlottedRow slotted_;
  std::vector<std::size_t> result_slots_;
  std::size_t peak_memory_bytes_ = 0;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<PhysicalResultCursor> StartPhysicalPlan(
    const PhysicalPlan &plan, graphdb::Transaction &transaction,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns,
    QueryExecutionOptions options) {
  return std::make_unique<PhysicalResultCursorImpl>(
      plan, transaction, parameters, result_columns, std::move(options));
}

}  // namespace rg
