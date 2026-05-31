#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ir/logical_plan.h"
#include "storage/in_memory_graph.h"
#include "value/value.h"

namespace rg {

using QueryRow = std::map<std::string, Value>;

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

class QueryExecutor final {
 public:
  explicit QueryExecutor(InMemoryGraph &graph) : graph_(&graph) {}

  [[nodiscard]] QueryResult Execute(const ir::LogicalPlan &plan) const;
  void ExecuteWrite(const ir::LogicalPlan &plan);

 private:
  InMemoryGraph *graph_ = nullptr;
};

[[nodiscard]] QueryResult ExecuteReadQuery(const InMemoryGraph &graph,
                                           std::string_view cypher);
[[nodiscard]] QueryResult ExecuteQuery(InMemoryGraph &graph,
                                       std::string_view cypher);
void ExecuteWriteQuery(InMemoryGraph &graph, std::string_view cypher);

}  // namespace rg
