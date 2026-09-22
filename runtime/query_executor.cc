#include "runtime/query_executor.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
#include "ast/ast_node.h"
#include "common/exception.h"
#include "graphdb/graph_db.h"
#include "graphdb/transaction.h"
#include "planner/planned_query.h"
#include "runtime/graphdb_planner_catalog.h"
#include "runtime/physical_executor.h"
#include "runtime/physical_plan.h"
#include "runtime/plan_cache.h"

namespace rg {
namespace {

planner::LogicalPlanBuilderOptions PlannerOptionsFor(
    const QueryOptions &options) {
  return planner::LogicalPlanBuilderOptions{
      .max_idp_candidates_per_relationship_count =
          options.max_idp_candidates_per_relationship_count,
      .planner_statistics = options.planner_statistics,
      .planner_catalog = options.planner_catalog};
}

void ValidateQueryParameters(const ast::Statement &statement,
                             const QueryParameters &parameters) {
  class ParameterValidator final : public ast::ASTConstWalker {
   public:
    explicit ParameterValidator(const QueryParameters &parameters)
        : parameters_(&parameters) {}

    void Visit(const ast::Parameter &parameter) override {
      if (!parameters_->contains(parameter.name)) {
        RG_THROW(common::ErrorCode::InvalidParameter,
                 "missing query parameter: " + parameter.name);
      }
    }

   private:
    const QueryParameters *parameters_ = nullptr;
  } validator(parameters);
  validator.Walk(statement);
}

std::vector<std::string> CollectQueryParameters(
    const ast::Statement &statement) {
  class ParameterCollector final : public ast::ASTConstWalker {
   public:
    void Visit(const ast::Parameter &parameter) override {
      if (seen_.emplace(parameter.name).second) {
        parameters_.push_back(parameter.name);
      }
    }

    [[nodiscard]] std::vector<std::string> TakeParameters() {
      return std::move(parameters_);
    }

   private:
    std::unordered_set<std::string> seen_;
    std::vector<std::string> parameters_;
  } collector;
  collector.Walk(statement);
  return collector.TakeParameters();
}

void ValidateQueryParameters(const std::vector<std::string> &required,
                             const QueryParameters &parameters) {
  for (const auto &name : required) {
    if (!parameters.contains(name)) {
      RG_THROW(common::ErrorCode::InvalidParameter,
               "missing query parameter: " + name);
    }
  }
}

std::shared_ptr<const CachedPlan> CompileCachedPlan(
    std::string_view cypher, const QueryOptions &options) {
  planner::PlannedQuery planned_query =
      planner::PlanCypher(cypher, PlannerOptionsFor(options));
  auto required_parameters = CollectQueryParameters(planned_query.Ast());
  auto physical_plan = std::make_shared<PhysicalPlan>(
      CreatePhysicalPlan(planned_query.LogicalPlan()));
  return std::make_shared<CachedPlan>(std::move(physical_plan),
                                      std::move(required_parameters));
}

class QueryResultCursorImpl final : public QueryResultCursor {
 public:
  [[nodiscard]] static std::unique_ptr<QueryResultCursorImpl> Create(
      const ir::LogicalPlan &logical_plan, graphdb::Transaction &transaction,
      const QueryParameters &parameters, QueryExecutionOptions options) {
    RG_CHECK(transaction.GetState() == graphdb::Transaction::State::kActive,
             common::ErrorCode::InvalidParameter,
             "query execution requires an active transaction");
    auto physical_plan =
        std::make_shared<PhysicalPlan>(CreatePhysicalPlan(logical_plan));
    return Create(std::move(physical_plan), transaction, parameters,
                  std::move(options));
  }

  [[nodiscard]] static std::unique_ptr<QueryResultCursorImpl> Create(
      std::shared_ptr<const PhysicalPlan> physical_plan,
      graphdb::Transaction &transaction, const QueryParameters &parameters,
      QueryExecutionOptions options) {
    RG_CHECK(transaction.GetState() == graphdb::Transaction::State::kActive,
             common::ErrorCode::InvalidParameter,
             "query execution requires an active transaction");
    RG_CHECK(physical_plan != nullptr, common::ErrorCode::InternalError,
             "physical plan is null");
    auto cursor = std::unique_ptr<QueryResultCursorImpl>(
        new QueryResultCursorImpl(std::move(physical_plan), transaction));
    cursor->physical_cursor_ = StartPhysicalPlan(
        *cursor->physical_plan_, transaction, parameters,
        cursor->physical_plan_->ResultColumns(), std::move(options));
    return cursor;
  }

  ~QueryResultCursorImpl() override { Close(); }

  [[nodiscard]] const std::vector<std::string> &Columns()
      const noexcept override {
    return physical_plan_->ResultColumns();
  }

  [[nodiscard]] bool Next(std::vector<Value> *row) override {
    RG_CHECK(row != nullptr, common::ErrorCode::InvalidParameter,
             "result row is null");
    if (closed_) {
      return false;
    }
    RG_CHECK(IsActive(), common::ErrorCode::InvalidParameter,
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
  QueryResultCursorImpl(std::shared_ptr<const PhysicalPlan> physical_plan,
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

  std::shared_ptr<const PhysicalPlan> physical_plan_;
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
  RG_CHECK(transaction_ != nullptr, common::ErrorCode::InternalError,
           "transaction is null");
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
  RG_CHECK(transaction.GetState() == graphdb::Transaction::State::kActive,
           common::ErrorCode::InvalidParameter,
           "query execution requires an active transaction");
  GraphDBPlannerCatalog graphdb_catalog(*transaction.db());
  const bool uses_graphdb_catalog = options.planner_catalog == nullptr;
  if (uses_graphdb_catalog) {
    options.planner_catalog = &graphdb_catalog;
  }

  if (options.plan_cache != nullptr) {
    const PlanCacheKey key{
        .cypher = std::string(cypher),
        .max_idp_candidates_per_relationship_count =
            options.max_idp_candidates_per_relationship_count,
        .graph_identity = transaction.db()->PlanCacheIdentity(),
        .planner_catalog_identity =
            uses_graphdb_catalog
                ? static_cast<const void *>(transaction.db())
                : static_cast<const void *>(options.planner_catalog),
        .planner_statistics_identity = options.planner_statistics,
        .planner_catalog_version =
            uses_graphdb_catalog
                ? transaction.db()->meta_info().PlannerCatalogVersion()
                : 0,
        .planning_context_version = options.plan_cache_generation};
    auto cached_plan = options.plan_cache->LookupOrCompile(
        key, [&] { return CompileCachedPlan(cypher, options); });
    ValidateQueryParameters(cached_plan->RequiredParameters(),
                            options.parameters);
    return QueryResultCursorImpl::Create(cached_plan->PhysicalPlanPtr(),
                                         transaction, options.parameters,
                                         std::move(options.execution));
  }

  planner::PlannedQuery planned_query =
      planner::PlanCypher(cypher, PlannerOptionsFor(options));
  ValidateQueryParameters(planned_query.Ast(), options.parameters);
  return QueryResultCursorImpl::Create(planned_query.LogicalPlan(), transaction,
                                       options.parameters,
                                       std::move(options.execution));
}

}  // namespace rg
