#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ir/logical_plan.h"
#include "storage/storage.h"
#include "value/value.h"

namespace ir {

class PlannerCatalog;
class PlannerStatistics;

}  // namespace ir

namespace rg {

using QueryRow = std::map<std::string, Value>;

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

struct QueryOptions {
  std::size_t max_idp_candidates_per_relationship_count = 128;
  const ir::PlannerStatistics *planner_statistics = nullptr;
  const ir::PlannerCatalog *planner_catalog = nullptr;
};

class QueryExecutor final {
 public:
  explicit QueryExecutor(Storage &storage)
      : access_path_(&storage), storage_(&storage) {}
  explicit QueryExecutor(const AccessPath &access_path)
      : access_path_(&access_path) {}

  [[nodiscard]] QueryResult Execute(const ir::LogicalPlan &plan) const;
  void ExecuteWrite(const ir::LogicalPlan &plan);

 private:
  const AccessPath *access_path_ = nullptr;
  Storage *storage_ = nullptr;
};

[[nodiscard]] QueryResult ExecuteReadQuery(const AccessPath &access_path,
                                           std::string_view cypher,
                                           QueryOptions options = {});
[[nodiscard]] QueryResult ExecuteQuery(Storage &storage,
                                       std::string_view cypher,
                                       QueryOptions options = {});
void ExecuteWriteQuery(Storage &storage, std::string_view cypher,
                       QueryOptions options = {});

}  // namespace rg
