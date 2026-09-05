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

QueryResult ExecuteWithTransaction(const ir::LogicalPlan &plan,
                                   const GraphReader &graph_reader,
                                   Storage *storage,
                                   const QueryParameters &parameters,
                                   bool discard_results) {
  const bool write = PlanContainsWrites(plan);
  if (write) {
    CHECK(storage != nullptr, common::InvalidArgumentError,
          "write execution requires storage");
  }

  std::unique_ptr<StorageTransaction> transaction;
  if (write) {
    transaction = storage->BeginTransaction();
  }
  try {
    PhysicalPlan physical_plan = CreatePhysicalPlan(plan);
    const bool collect_results =
        !discard_results &&
        (plan.Type() == ir::LogicalPlanNodeType::kProduceResults || !write);
    PhysicalExecutionResult execution =
        ExecutePhysicalPlan(physical_plan, graph_reader, storage, parameters,
                            plan.OutputColumns(), collect_results);
    if (transaction != nullptr) {
      transaction->Commit();
    }
    QueryResult result;
    if (collect_results) {
      result.columns = plan.OutputColumns();
      result.rows = std::move(execution.rows);
    }
    return result;
  } catch (...) {
    if (transaction != nullptr) {
      transaction->Rollback();
    }
    throw;
  }
}

}  // namespace

QueryResult QueryExecutor::Execute(const ir::LogicalPlan &plan,
                                   const QueryParameters &parameters) const {
  CHECK(graph_reader_ != nullptr, common::InternalError,
        "graph reader is null");
  return ExecuteWithTransaction(plan, *graph_reader_, storage_, parameters,
                                false);
}

void QueryExecutor::ExecuteWrite(const ir::LogicalPlan &plan,
                                 const QueryParameters &parameters) {
  CHECK(storage_ != nullptr, common::InternalError,
        "write execution requires storage");
  (void)ExecuteWithTransaction(plan, *storage_, storage_, parameters, true);
}

QueryResult ExecuteReadQuery(const GraphReader &graph_reader,
                             std::string_view cypher, QueryOptions options) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::QueryIR> query_ir = ir::CreateQueryIR(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*query_ir, PlannerOptionsFor(options));
  return QueryExecutor(graph_reader).Execute(*logical_plan, options.parameters);
}

QueryResult ExecuteQuery(Storage &storage, std::string_view cypher,
                         QueryOptions options) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::QueryIR> query_ir = ir::CreateQueryIR(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*query_ir, PlannerOptionsFor(options));
  return QueryExecutor(storage).Execute(*logical_plan, options.parameters);
}

void ExecuteWriteQuery(Storage &storage, std::string_view cypher,
                       QueryOptions options) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::QueryIR> query_ir = ir::CreateQueryIR(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan =
      ir::CreateLogicalPlan(*query_ir, PlannerOptionsFor(options));
  QueryExecutor(storage).ExecuteWrite(*logical_plan, options.parameters);
}

}  // namespace rg
