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

#include "graphdb/assistant_pool.h"
#include "graphdb/graph_db.h"
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
};

inline QueryResult ExecuteQueryAndCommit(GraphDBTestDatabase& database,
                                         std::string_view cypher,
                                         QueryOptions options = {}) {
  return database.ExecuteQueryAndCommit(cypher, std::move(options));
}

}  // namespace rg::test
