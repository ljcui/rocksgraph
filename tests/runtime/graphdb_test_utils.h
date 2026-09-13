#pragma once

#include <boost/endian/conversion.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "graphdb/assistant_pool.h"
#include "graphdb/edge_iterator.h"
#include "graphdb/graph_db.h"
#include "runtime/graphdb_access.h"
#include "runtime/query_executor.h"
#include "transaction/transaction.h"

namespace rg::test {

inline std::unordered_map<std::string, Value> ToGraphDBProperties(
    const Value::Map& properties) {
  return {properties.begin(), properties.end()};
}

inline void RollbackGraphDBTransaction(txn::Transaction* transaction) noexcept {
  if (transaction == nullptr ||
      transaction->GetState() != txn::Transaction::State::kActive) {
    return;
  }
  try {
    transaction->Rollback();
  } catch (...) {
  }
}

inline QueryResult ExecuteGraphDBQueryAndCommit(graphdb::GraphDB& graph,
                                                std::string_view cypher,
                                                QueryOptions options = {}) {
  auto transaction = graph.BeginTransaction();
  try {
    QueryResult result = ExecuteQuery(*transaction, cypher, std::move(options));
    transaction->Commit();
    return result;
  } catch (...) {
    RollbackGraphDBTransaction(transaction.get());
    throw;
  }
}

inline std::size_t CountVertices(txn::Transaction& transaction) {
  auto vertices = transaction.NewVertexIterator();
  std::size_t count = 0;
  while (vertices->Valid()) {
    ++count;
    vertices->Next();
  }
  return count;
}

inline std::size_t CountEdges(txn::Transaction& transaction) {
  auto edges = transaction.NewEdgeIterator();
  std::size_t count = 0;
  while (edges->Valid()) {
    ++count;
    edges->Next();
  }
  return count;
}

class GraphDBTestDatabase final {
 public:
  GraphDBTestDatabase()
      : path_(NewPath()),
        assistant_pool_(std::make_shared<graphdb::AssistantPool>(1)) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    graphdb::GraphDBOptions options;
    options.assistant_pool = assistant_pool_;
    graph_ = graphdb::GraphDB::Open(path_.string(), options);
    graph_->drop_on_close() = true;
  }

  [[nodiscard]] graphdb::GraphDB& Graph() noexcept { return *graph_; }

  [[nodiscard]] std::unique_ptr<txn::Transaction> BeginTransaction() {
    return graph_->BeginTransaction();
  }

  Value::NodePtr CreateNode(std::vector<std::string> labels = {},
                            Value::Map properties = {}) {
    auto transaction = BeginTransaction();
    try {
      auto node = CreateGraphDBVertex(*transaction, std::move(labels),
                                      std::move(properties));
      transaction->Commit();
      return node;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

  Value::NodePtr CreateNode(std::initializer_list<std::string> labels,
                            Value::Map properties = {}) {
    return CreateNode(std::vector<std::string>(labels), std::move(properties));
  }

  Value::RelationshipPtr CreateRelationship(const Value::NodePtr& start,
                                            const Value::NodePtr& end,
                                            std::string type,
                                            Value::Map properties = {}) {
    auto transaction = BeginTransaction();
    try {
      auto relationship =
          CreateGraphDBEdge(*transaction, start->id, end->id, std::move(type),
                            std::move(properties));
      transaction->Commit();
      return relationship;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

  void AddNodeIndex(const std::vector<std::string>& labels,
                    std::string_view property) {
    CHECK(!labels.empty(), common::InvalidArgumentError,
          "GraphDB node index requires a label");
    graph_->AddVertexPropertyIndex(
        "runtime_node_index_" + std::to_string(index_sequence_++), false,
        labels.front(), {std::string(property)});
  }

  void AddRelationshipIndex(const std::vector<std::string>& types,
                            std::string_view property) {
    CHECK(!types.empty(), common::InvalidArgumentError,
          "GraphDB relationship index requires a type");
    graph_->AddEdgePropertyIndex(
        "runtime_edge_index_" + std::to_string(index_sequence_++), false,
        types.front(), {std::string(property)});
  }

  [[nodiscard]] std::vector<Value::NodePtr> Nodes() {
    auto transaction = BeginTransaction();
    try {
      std::vector<Value::NodePtr> nodes;
      auto iterator = transaction->NewVertexIterator();
      while (iterator->Valid()) {
        nodes.push_back(MaterializeGraphDBVertex(
            *transaction, iterator->GetVertex().GetNativeId()));
        iterator->Next();
      }
      transaction->Commit();
      return nodes;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

  [[nodiscard]] std::vector<Value::RelationshipPtr> Relationships() {
    auto transaction = BeginTransaction();
    try {
      std::vector<Value::RelationshipPtr> relationships;
      auto iterator = transaction->NewEdgeIterator();
      while (iterator->Valid()) {
        const auto& edge = iterator->GetEdge();
        relationships.push_back(MaterializeGraphDBEdge(
            *transaction, {edge.GetNativeId(), edge.GetTypeId()}));
        iterator->Next();
      }
      transaction->Commit();
      return relationships;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

  [[nodiscard]] QueryResult ExecuteQueryAndCommit(std::string_view cypher,
                                                  QueryOptions options = {}) {
    return rg::test::ExecuteGraphDBQueryAndCommit(*graph_, cypher,
                                                  std::move(options));
  }

  std::int64_t CreateVertex(std::vector<std::string> labels = {},
                            Value::Map properties = {}) {
    std::unordered_set<std::string> label_set(labels.begin(), labels.end());
    auto transaction = BeginTransaction();
    try {
      const std::int64_t id =
          transaction->CreateVertex(label_set, ToGraphDBProperties(properties))
              .GetNativeId();
      transaction->Commit();
      return id;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

  std::int64_t CreateVertex(std::initializer_list<std::string> labels,
                            Value::Map properties = {}) {
    return CreateVertex(std::vector<std::string>(labels),
                        std::move(properties));
  }

  std::int64_t CreateEdge(std::int64_t start_node_id, std::int64_t end_node_id,
                          std::string type, Value::Map properties = {}) {
    auto transaction = BeginTransaction();
    try {
      const auto start = transaction->GetVertexById(
          boost::endian::native_to_big(start_node_id));
      const auto end =
          transaction->GetVertexById(boost::endian::native_to_big(end_node_id));
      const std::int64_t id =
          transaction
              ->CreateEdge(start, end, type, ToGraphDBProperties(properties))
              .GetNativeId();
      transaction->Commit();
      return id;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

  [[nodiscard]] std::size_t VertexCount() const {
    auto transaction = graph_->BeginTransaction();
    try {
      const std::size_t count = CountVertices(*transaction);
      transaction->Commit();
      return count;
    } catch (...) {
      RollbackGraphDBTransaction(transaction.get());
      throw;
    }
  }

 private:
  static std::filesystem::path NewPath() {
    static std::size_t sequence = 0;
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("rocksgraph_runtime_" + std::to_string(timestamp) + "_" +
            std::to_string(sequence++));
  }

  std::filesystem::path path_;
  std::shared_ptr<graphdb::AssistantPool> assistant_pool_;
  std::unique_ptr<graphdb::GraphDB> graph_;
  inline static std::size_t index_sequence_ = 0;
};

inline QueryResult ExecuteQueryAndCommit(GraphDBTestDatabase& database,
                                         std::string_view cypher,
                                         QueryOptions options = {}) {
  return database.ExecuteQueryAndCommit(cypher, std::move(options));
}

inline QueryResult ExecutePlanAndCommit(GraphDBTestDatabase& database,
                                        const ir::LogicalPlan& plan,
                                        const QueryParameters& parameters = {},
                                        QueryExecutionOptions options = {}) {
  auto transaction = database.BeginTransaction();
  try {
    QueryResult result = QueryExecutor(*transaction)
                             .Execute(plan, parameters, std::move(options));
    transaction->Commit();
    return result;
  } catch (...) {
    RollbackGraphDBTransaction(transaction.get());
    throw;
  }
}

}  // namespace rg::test
