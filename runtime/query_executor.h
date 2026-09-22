#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/execution_context.h"
#include "value/value.h"

namespace graphdb {
class Transaction;
}

namespace planner {

class PlannerCatalog;
class PlannerStatistics;

}  // namespace planner

namespace rg {

class PlanCache;

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
  // Stops execution and releases cursor resources. These methods never
  // commit or roll back the caller-owned transaction.
  virtual void Cancel() noexcept = 0;
  virtual void Close() noexcept = 0;
  [[nodiscard]] virtual std::size_t PeakMemoryBytes() const noexcept = 0;
};

struct QueryOptions {
  std::size_t max_idp_candidates_per_relationship_count = 128;
  const planner::PlannerStatistics *planner_statistics = nullptr;
  const planner::PlannerCatalog *planner_catalog = nullptr;
  // A cache is scoped by the graph, planner inputs, and catalog version. When
  // supplying a custom catalog or mutable statistics, increment
  // plan_cache_generation whenever its planning metadata changes (or clear
  // the cache explicitly).
  PlanCache *plan_cache = nullptr;
  std::uint64_t plan_cache_generation = 0;
  QueryParameters parameters;
  QueryExecutionOptions execution;
};

class QueryExecutor final {
 public:
  explicit QueryExecutor(graphdb::Transaction &transaction)
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
  graphdb::Transaction *transaction_ = nullptr;
};

// Executes one Cypher statement in an active transaction. The caller owns the
// transaction boundary and must commit it explicitly.
[[nodiscard]] QueryResult ExecuteQuery(graphdb::Transaction &transaction,
                                       std::string_view cypher,
                                       QueryOptions options = {});
[[nodiscard]] std::unique_ptr<QueryResultCursor> ExecuteQueryCursor(
    graphdb::Transaction &transaction, std::string_view cypher,
    QueryOptions options = {});

}  // namespace rg
