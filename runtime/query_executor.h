#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/execution_context.h"
#include "storage/graph_transaction.h"
#include "value/value.h"

namespace ir {

class PlannerCatalog;
class PlannerStatistics;

}  // namespace ir

namespace rg {

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  std::size_t peak_memory_bytes = 0;
};

class QueryResultCursor {
 public:
  QueryResultCursor() = default;
  QueryResultCursor(const QueryResultCursor &) = delete;
  QueryResultCursor &operator=(const QueryResultCursor &) = delete;
  virtual ~QueryResultCursor() = default;

  [[nodiscard]] virtual const std::vector<std::string> &Columns()
      const noexcept = 0;
  [[nodiscard]] virtual bool Next(std::vector<Value> *row) = 0;
  virtual void Cancel() noexcept = 0;
  virtual void Close() noexcept = 0;
  [[nodiscard]] virtual std::size_t PeakMemoryBytes() const noexcept = 0;
};

struct QueryOptions {
  std::size_t max_idp_candidates_per_relationship_count = 128;
  const ir::PlannerStatistics *planner_statistics = nullptr;
  const ir::PlannerCatalog *planner_catalog = nullptr;
  QueryParameters parameters;
  QueryExecutionOptions execution;
};

class QueryExecutor final {
 public:
  explicit QueryExecutor(GraphTransaction &transaction)
      : transaction_(&transaction) {}

  // Execution requires an active transaction and never commits it.
  [[nodiscard]] QueryResult Execute(const ir::LogicalPlan &plan,
                                    const QueryParameters &parameters = {},
                                    QueryExecutionOptions options = {}) const;
  // The plan and its referenced AST expressions only need to remain valid
  // until this call returns. The cursor borrows the caller-owned transaction.
  [[nodiscard]] std::unique_ptr<QueryResultCursor> ExecuteCursor(
      const ir::LogicalPlan &plan, const QueryParameters &parameters = {},
      QueryExecutionOptions options = {}) const;

 private:
  GraphTransaction *transaction_ = nullptr;
};

// Executes one Cypher statement in an active transaction. The caller owns the
// transaction boundary and must commit it explicitly.
[[nodiscard]] QueryResult ExecuteQuery(GraphTransaction &transaction,
                                       std::string_view cypher,
                                       QueryOptions options = {});
[[nodiscard]] std::unique_ptr<QueryResultCursor> ExecuteQueryCursor(
    GraphTransaction &transaction, std::string_view cypher,
    QueryOptions options = {});

}  // namespace rg
