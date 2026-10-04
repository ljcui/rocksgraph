#include "runtime/physical_executor.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "runtime/physical_executor_internal.h"

namespace runtime {
namespace {

void ValidateTransactionProcedures(const PhysicalPlanNode &node) {
  if (node.kind == PhysicalOperatorKind::kProcedureCall) {
    const auto &procedure = std::get<ProcedureCallOp>(node.data);
    RG_CHECK(procedure.read_only, common::ErrorCode::Unimplemented,
             "procedure [{}] requires auto-commit mode",
             procedure.procedure_name);
  }
  for (const auto &child : node.children) {
    ValidateTransactionProcedures(*child);
  }
}

class PhysicalResultCursorImpl final : public PhysicalResultCursor {
 public:
  PhysicalResultCursorImpl(const PhysicalPlan &plan,
                           graphdb::Transaction *transaction,
                           const QueryParameters &parameters,
                           std::vector<std::string> result_columns,
                           QueryExecutionOptions options)
      : state_(transaction, parameters, std::move(options)),
        factory_(state_),
        root_(factory_.Build(plan.Root())),
        output_layout_(plan.Root().output_layout),
        row_(output_layout_) {
    result_offsets_.reserve(result_columns.size());
    for (const auto &column : result_columns) {
      result_offsets_.push_back(output_layout_->At(column));
    }
  }

  ~PhysicalResultCursorImpl() override { Close(); }

  [[nodiscard]] bool Next(std::vector<rg::Value> *row) override {
    RG_CHECK(row != nullptr, common::ErrorCode::InvalidParameter,
             "result row is null");
    if (closed_) {
      return false;
    }
    try {
      state_.CheckCancelled();
      row_.Reset();
      if (!root_->Next(&row_)) {
        Close();
        return false;
      }
      row->clear();
      row->reserve(result_offsets_.size());
      for (const std::size_t offset : result_offsets_) {
        row->push_back(row_.IsInitialized(offset)
                           ? execution::ReadRowValue(row_, offset)
                           : rg::Value::Null());
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
  execution::RuntimeState state_;
  execution::OperatorFactory factory_;
  std::unique_ptr<execution::PullOperator> root_;
  RowLayoutPtr output_layout_;
  ExecutionRow row_;
  std::vector<std::size_t> result_offsets_;
  std::size_t peak_memory_bytes_ = 0;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<PhysicalResultCursor> StartPhysicalPlan(
    const PhysicalPlan &plan, graphdb::Transaction *transaction,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns,
    QueryExecutionOptions options) {
  RG_CHECK(!options.read_only || !plan.Effects().writes,
           common::ErrorCode::WriteInReadOnlyTransaction,
           "write query is not allowed in a read-only transaction");
  if (options.explicit_transaction) {
    ValidateTransactionProcedures(plan.Root());
  }
  return std::make_unique<PhysicalResultCursorImpl>(
      plan, transaction, parameters, result_columns, std::move(options));
}

}  // namespace runtime
