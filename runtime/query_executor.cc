#include "runtime/query_executor.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "graphdb/transaction.h"
#include "planner/planned_query.h"
#include "runtime/graphdb_planner_catalog.h"
#include "runtime/physical_plan.h"
#include "runtime/slotted_executor.h"

namespace rg {
namespace {

ir::LogicalPlanBuilderOptions PlannerOptionsFor(const QueryOptions &options) {
  return ir::LogicalPlanBuilderOptions{
      .max_idp_candidates_per_relationship_count =
          options.max_idp_candidates_per_relationship_count,
      .planner_statistics = options.planner_statistics,
      .planner_catalog = options.planner_catalog};
}

class QueryResultCursorImpl final : public QueryResultCursor {
 public:
  [[nodiscard]] static std::unique_ptr<QueryResultCursorImpl> Create(
      const ir::LogicalPlan &logical_plan, graphdb::Transaction &transaction,
      const QueryParameters &parameters, QueryExecutionOptions options) {
    CHECK(transaction.GetState() == graphdb::Transaction::State::kActive,
          common::InvalidArgumentError,
          "query execution requires an active transaction");
    auto cursor =
        std::unique_ptr<QueryResultCursorImpl>(new QueryResultCursorImpl(
            CreatePhysicalPlan(logical_plan), transaction));
    cursor->physical_cursor_ = StartPhysicalPlan(
        cursor->physical_plan_, transaction, parameters,
        cursor->physical_plan_.ResultColumns(), std::move(options));
    return cursor;
  }

  ~QueryResultCursorImpl() override { Close(); }

  [[nodiscard]] const std::vector<std::string> &Columns()
      const noexcept override {
    return physical_plan_.ResultColumns();
  }

  [[nodiscard]] bool Next(std::vector<Value> *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "result row is null");
    if (closed_) {
      return false;
    }
    CHECK(IsActive(), common::InvalidArgumentError,
          "transaction is no longer active");
    try {
      if (physical_cursor_->Next(row)) {
        return true;
      }
      Finish();
      return false;
    } catch (...) {
      ClosePhysicalCursor();
      closed_ = true;
      throw;
    }
  }

  void Cancel() noexcept override {
    if (physical_cursor_ != nullptr) {
      physical_cursor_->Cancel();
    }
    Close();
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    ClosePhysicalCursor();
    closed_ = true;
  }

  [[nodiscard]] std::size_t PeakMemoryBytes() const noexcept override {
    return physical_cursor_ != nullptr ? physical_cursor_->PeakMemoryBytes()
                                       : peak_memory_bytes_;
  }

 private:
  QueryResultCursorImpl(PhysicalPlan physical_plan,
                        graphdb::Transaction &transaction)
      : physical_plan_(std::move(physical_plan)), transaction_(&transaction) {}

  [[nodiscard]] bool IsActive() const noexcept {
    return transaction_ != nullptr &&
           transaction_->GetState() == graphdb::Transaction::State::kActive;
  }

  void Finish() {
    ClosePhysicalCursor();
    closed_ = true;
  }

  void ClosePhysicalCursor() noexcept {
    if (physical_cursor_ != nullptr) {
      physical_cursor_->Close();
      peak_memory_bytes_ = physical_cursor_->PeakMemoryBytes();
    }
  }

  PhysicalPlan physical_plan_;
  graphdb::Transaction *transaction_ = nullptr;
  std::unique_ptr<PhysicalResultCursor> physical_cursor_;
  std::size_t peak_memory_bytes_ = 0;
  bool closed_ = false;
};

QueryResult ConsumeCursor(std::unique_ptr<QueryResultCursor> cursor) {
  QueryResult result;
  result.columns = cursor->Columns();
  std::vector<Value> row;
  while (cursor->Next(&row)) {
    if (!result.columns.empty()) {
      result.rows.push_back(std::move(row));
    }
  }
  result.peak_memory_bytes = cursor->PeakMemoryBytes();
  cursor->Close();
  return result;
}

}  // namespace

QueryResult QueryExecutor::Execute(const ir::LogicalPlan &plan,
                                   const QueryParameters &parameters,
                                   QueryExecutionOptions options) const {
  return ConsumeCursor(ExecuteCursor(plan, parameters, std::move(options)));
}

std::unique_ptr<QueryResultCursor> QueryExecutor::ExecuteCursor(
    const ir::LogicalPlan &plan, const QueryParameters &parameters,
    QueryExecutionOptions options) const {
  CHECK(transaction_ != nullptr, common::InternalError, "transaction is null");
  return QueryResultCursorImpl::Create(plan, *transaction_, parameters,
                                       std::move(options));
}

QueryResult ExecuteQuery(graphdb::Transaction &transaction,
                         std::string_view cypher, QueryOptions options) {
  return ConsumeCursor(
      ExecuteQueryCursor(transaction, cypher, std::move(options)));
}

std::unique_ptr<QueryResultCursor> ExecuteQueryCursor(
    graphdb::Transaction &transaction, std::string_view cypher,
    QueryOptions options) {
  CHECK(transaction.GetState() == graphdb::Transaction::State::kActive,
        common::InvalidArgumentError,
        "query execution requires an active transaction");
  GraphDBPlannerCatalog graphdb_catalog(*transaction.db());
  if (options.planner_catalog == nullptr) {
    options.planner_catalog = &graphdb_catalog;
  }
  ir::PlannedQuery planned_query =
      ir::PlanCypher(cypher, PlannerOptionsFor(options));
  return QueryResultCursorImpl::Create(planned_query.LogicalPlan(), transaction,
                                       options.parameters,
                                       std::move(options.execution));
}

}  // namespace rg
