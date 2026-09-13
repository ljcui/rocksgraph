#include "runtime/query_executor.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "planner/planned_query.h"
#include "runtime/graphdb_planner_catalog.h"
#include "runtime/physical_plan.h"
#include "runtime/slotted_executor.h"
#include "transaction/transaction.h"

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
  QueryResultCursorImpl(const ir::LogicalPlan &logical_plan,
                        GraphTransaction &transaction,
                        const QueryParameters &parameters,
                        QueryExecutionOptions options)
      : physical_plan_(CreatePhysicalPlan(logical_plan)),
        transaction_(&transaction) {
    CHECK(transaction.GetState() == GraphTransaction::State::kActive,
          common::InvalidArgumentError,
          "query execution requires an active transaction");
    const bool writes = physical_plan_.Effects().writes;
    try {
      if (writes) {
        CHECK(transaction_->IsWritable(), common::InvalidArgumentError,
              "write execution requires a writable transaction");
      }
      if (logical_plan.Type() == ir::LogicalPlanNodeType::kProduceResults ||
          !writes) {
        columns_ = logical_plan.OutputColumns();
      }
      physical_cursor_ =
          StartPhysicalPlan(physical_plan_, *transaction_, parameters, columns_,
                            std::move(options));
    } catch (...) {
      Rollback();
      throw;
    }
  }

  QueryResultCursorImpl(const ir::LogicalPlan &logical_plan,
                        txn::Transaction &transaction,
                        const QueryParameters &parameters,
                        QueryExecutionOptions options)
      : physical_plan_(CreatePhysicalPlan(logical_plan)),
        graphdb_transaction_(&transaction) {
    CHECK(transaction.GetState() == txn::Transaction::State::kActive,
          common::InvalidArgumentError,
          "query execution requires an active transaction");
    CHECK(!physical_plan_.Effects().writes, common::InvalidArgumentError,
          "GraphDB query execution currently supports read plans only");
    try {
      columns_ = logical_plan.OutputColumns();
      physical_cursor_ =
          StartPhysicalPlan(physical_plan_, transaction, parameters, columns_,
                            std::move(options));
    } catch (...) {
      Rollback();
      throw;
    }
  }

  ~QueryResultCursorImpl() override { Close(); }

  [[nodiscard]] const std::vector<std::string> &Columns()
      const noexcept override {
    return columns_;
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
      Rollback();
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
    if (!exhausted_) {
      Rollback();
    }
    closed_ = true;
  }

  [[nodiscard]] std::size_t PeakMemoryBytes() const noexcept override {
    return physical_cursor_ != nullptr ? physical_cursor_->PeakMemoryBytes()
                                       : peak_memory_bytes_;
  }

 private:
  [[nodiscard]] bool IsActive() const noexcept {
    return (transaction_ != nullptr &&
            transaction_->GetState() == GraphTransaction::State::kActive) ||
           (graphdb_transaction_ != nullptr &&
            graphdb_transaction_->GetState() ==
                txn::Transaction::State::kActive);
  }

  void Finish() {
    ClosePhysicalCursor();
    exhausted_ = true;
    closed_ = true;
  }

  void Rollback() noexcept {
    try {
      if (transaction_ != nullptr &&
          transaction_->GetState() == GraphTransaction::State::kActive) {
        transaction_->Rollback();
      } else if (graphdb_transaction_ != nullptr &&
                 graphdb_transaction_->GetState() ==
                     txn::Transaction::State::kActive) {
        graphdb_transaction_->Rollback();
      }
    } catch (...) {
    }
  }

  void ClosePhysicalCursor() noexcept {
    if (physical_cursor_ != nullptr) {
      physical_cursor_->Close();
      peak_memory_bytes_ = physical_cursor_->PeakMemoryBytes();
    }
  }

  PhysicalPlan physical_plan_;
  std::vector<std::string> columns_;
  GraphTransaction *transaction_ = nullptr;
  txn::Transaction *graphdb_transaction_ = nullptr;
  std::unique_ptr<PhysicalResultCursor> physical_cursor_;
  std::size_t peak_memory_bytes_ = 0;
  bool exhausted_ = false;
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
  CHECK(transaction_ != nullptr || graphdb_transaction_ != nullptr,
        common::InternalError, "transaction is null");
  if (graphdb_transaction_ != nullptr) {
    return std::make_unique<QueryResultCursorImpl>(
        plan, *graphdb_transaction_, parameters, std::move(options));
  }
  return std::make_unique<QueryResultCursorImpl>(
      plan, *transaction_, parameters, std::move(options));
}

QueryResult ExecuteQuery(GraphTransaction &transaction, std::string_view cypher,
                         QueryOptions options) {
  return ConsumeCursor(
      ExecuteQueryCursor(transaction, cypher, std::move(options)));
}

std::unique_ptr<QueryResultCursor> ExecuteQueryCursor(
    GraphTransaction &transaction, std::string_view cypher,
    QueryOptions options) {
  CHECK(transaction.GetState() == GraphTransaction::State::kActive,
        common::InvalidArgumentError,
        "query execution requires an active transaction");
  try {
    ir::PlannedQuery planned_query =
        ir::PlanCypher(cypher, PlannerOptionsFor(options));
    return std::make_unique<QueryResultCursorImpl>(
        planned_query.Plan(), transaction, options.parameters,
        std::move(options.execution));
  } catch (...) {
    if (transaction.GetState() == GraphTransaction::State::kActive) {
      try {
        transaction.Rollback();
      } catch (...) {
      }
    }
    throw;
  }
}

QueryResult ExecuteQuery(txn::Transaction &transaction, std::string_view cypher,
                         QueryOptions options) {
  return ConsumeCursor(
      ExecuteQueryCursor(transaction, cypher, std::move(options)));
}

std::unique_ptr<QueryResultCursor> ExecuteQueryCursor(
    txn::Transaction &transaction, std::string_view cypher,
    QueryOptions options) {
  CHECK(transaction.GetState() == txn::Transaction::State::kActive,
        common::InvalidArgumentError,
        "query execution requires an active transaction");
  try {
    GraphDBPlannerCatalog graphdb_catalog(*transaction.db());
    if (options.planner_catalog == nullptr) {
      options.planner_catalog = &graphdb_catalog;
    }
    ir::PlannedQuery planned_query =
        ir::PlanCypher(cypher, PlannerOptionsFor(options));
    return std::make_unique<QueryResultCursorImpl>(
        planned_query.Plan(), transaction, options.parameters,
        std::move(options.execution));
  } catch (...) {
    if (transaction.GetState() == txn::Transaction::State::kActive) {
      try {
        transaction.Rollback();
      } catch (...) {
      }
    }
    throw;
  }
}

}  // namespace rg
