#include "runtime/query_executor.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "common/exception.h"
#include "ir/query_ir.h"
#include "planner/catalog.h"
#include "planner/logical_plan_builder.h"
#include "runtime/physical_plan.h"
#include "runtime/slotted_executor.h"

namespace rg {
namespace {

class NoIndexPlannerCatalog final : public ir::PlannerCatalog {
 public:
  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    (void)property_key;
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override {
    (void)relationship_types;
    (void)property_key;
    return std::nullopt;
  }
};

const ir::PlannerCatalog &DefaultRuntimePlannerCatalog() {
  static const NoIndexPlannerCatalog kCatalog;
  return kCatalog;
}

ir::LogicalPlanBuilderOptions PlannerOptionsFor(const QueryOptions &options) {
  return ir::LogicalPlanBuilderOptions{
      .max_idp_candidates_per_relationship_count =
          options.max_idp_candidates_per_relationship_count,
      .planner_statistics = options.planner_statistics,
      .planner_catalog = options.planner_catalog != nullptr
                             ? options.planner_catalog
                             : &DefaultRuntimePlannerCatalog()};
}

struct ParsedQueryOwner {
  std::unique_ptr<ast::Statement> statement;
  std::unique_ptr<ir::QueryIR> query_ir;
  std::unique_ptr<ir::LogicalPlan> logical_plan;
};

class QueryResultCursorImpl final : public QueryResultCursor {
 public:
  QueryResultCursorImpl(const ir::LogicalPlan &logical_plan,
                        const GraphReader &graph_reader, Storage *storage,
                        const QueryParameters &parameters,
                        QueryExecutionOptions options,
                        std::shared_ptr<void> owner = {})
      : owner_(std::move(owner)),
        write_(PlanContainsWrites(logical_plan)),
        physical_plan_(CreatePhysicalPlan(logical_plan)) {
    if (write_) {
      CHECK(storage != nullptr, common::InvalidArgumentError,
            "write execution requires storage");
      transaction_ = storage->BeginTransaction();
    }
    if (logical_plan.Type() == ir::LogicalPlanNodeType::kProduceResults ||
        !write_) {
      columns_ = logical_plan.OutputColumns();
    }
    try {
      physical_cursor_ =
          StartPhysicalPlan(physical_plan_, graph_reader, storage, parameters,
                            columns_, std::move(options));
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
    if (!completed_) {
      Rollback();
    }
    closed_ = true;
  }

  [[nodiscard]] std::size_t PeakMemoryBytes() const noexcept override {
    return physical_cursor_ != nullptr ? physical_cursor_->PeakMemoryBytes()
                                       : peak_memory_bytes_;
  }

 private:
  void Finish() {
    ClosePhysicalCursor();
    if (transaction_ != nullptr) {
      transaction_->Commit();
    }
    completed_ = true;
    closed_ = true;
  }

  void Rollback() noexcept {
    if (transaction_ == nullptr || completed_) {
      return;
    }
    try {
      transaction_->Rollback();
    } catch (...) {
    }
    completed_ = true;
  }

  void ClosePhysicalCursor() noexcept {
    if (physical_cursor_ != nullptr) {
      physical_cursor_->Close();
      peak_memory_bytes_ = physical_cursor_->PeakMemoryBytes();
    }
  }

  std::shared_ptr<void> owner_;
  bool write_ = false;
  std::vector<std::string> columns_;
  PhysicalPlan physical_plan_;
  std::unique_ptr<PhysicalResultCursor> physical_cursor_;
  std::unique_ptr<StorageTransaction> transaction_;
  std::size_t peak_memory_bytes_ = 0;
  bool completed_ = false;
  bool closed_ = false;
};

std::shared_ptr<ParsedQueryOwner> BuildParsedQuery(
    std::string_view cypher, const QueryOptions &options) {
  auto owner = std::make_shared<ParsedQueryOwner>();
  owner->statement = ast::ParseCypherAndRewrite(std::string(cypher));
  owner->query_ir = ir::CreateQueryIR(*owner->statement);
  owner->logical_plan =
      ir::CreateLogicalPlan(*owner->query_ir, PlannerOptionsFor(options));
  return owner;
}

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
  CHECK(graph_reader_ != nullptr, common::InternalError,
        "graph reader is null");
  return std::make_unique<QueryResultCursorImpl>(
      plan, *graph_reader_, storage_, parameters, std::move(options));
}

QueryResult ExecuteReadQuery(const GraphReader &graph_reader,
                             std::string_view cypher, QueryOptions options) {
  return ConsumeCursor(
      ExecuteReadQueryCursor(graph_reader, cypher, std::move(options)));
}

QueryResult ExecuteQuery(Storage &storage, std::string_view cypher,
                         QueryOptions options) {
  return ConsumeCursor(ExecuteQueryCursor(storage, cypher, std::move(options)));
}

void ExecuteWriteQuery(Storage &storage, std::string_view cypher,
                       QueryOptions options) {
  (void)ConsumeCursor(ExecuteQueryCursor(storage, cypher, std::move(options)));
}

std::unique_ptr<QueryResultCursor> ExecuteReadQueryCursor(
    const GraphReader &graph_reader, std::string_view cypher,
    QueryOptions options) {
  std::shared_ptr<ParsedQueryOwner> owner = BuildParsedQuery(cypher, options);
  return std::make_unique<QueryResultCursorImpl>(
      *owner->logical_plan, graph_reader, nullptr, options.parameters,
      std::move(options.execution), owner);
}

std::unique_ptr<QueryResultCursor> ExecuteQueryCursor(Storage &storage,
                                                      std::string_view cypher,
                                                      QueryOptions options) {
  std::shared_ptr<ParsedQueryOwner> owner = BuildParsedQuery(cypher, options);
  return std::make_unique<QueryResultCursorImpl>(
      *owner->logical_plan, storage, &storage, options.parameters,
      std::move(options.execution), owner);
}

}  // namespace rg
