//
// Created by botu.wzy
//

#pragma once
#include <rocksdb/utilities/transaction_db.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graphdb/edge_direction.h"
#include "graphdb/edge_iterator.h"
#include "graphdb/graph_cf.h"
#include "graphdb/id_generator.h"
#include "graphdb/meta_info.h"
#include "graphdb/vertex_iterator.h"
#include "value/value.h"
namespace graphdb {
class GraphDB;

class Transaction {
 public:
  enum class State { kActive, kCommitted, kRolledBack };

  // No copying allowed
  Transaction(const Transaction&) = delete;
  void operator=(const Transaction&) = delete;

  Transaction(rocksdb::Transaction* txn, GraphDB* graph_db)
      : txn_(txn), db_(graph_db) {}
  ~Transaction() { delete txn_; }
  Vertex CreateVertex(const std::unordered_set<std::string>& labels,
                      const std::unordered_map<std::string, rg::Value>& values);
  Edge CreateEdge(const Vertex& start, const Vertex& end,
                  const std::string& type,
                  const std::unordered_map<std::string, rg::Value>& values);
  Vertex GetVertexById(int64_t vid);
  Edge GetEdgeById(uint32_t etid, int64_t eid);
  std::unique_ptr<VertexIterator> NewVertexIterator();
  std::unique_ptr<VertexIterator> NewVertexIterator(const std::string& label);
  std::unique_ptr<VertexIterator> NewVertexIterator(
      const std::optional<std::string>& label,
      const std::optional<std::unordered_map<std::string, rg::Value>>& props);
  std::unique_ptr<EdgeIterator> NewEdgeIterator();
  std::unique_ptr<EdgeIterator> NewEdgeIterator(
      const std::unordered_set<std::string>& types);
  // for debug
  std::string GetVertexIteratorInfo(
      const std::optional<std::string>& label,
      const std::optional<std::unordered_set<std::string>>& props);
  std::unique_ptr<VertexIterator> QueryVertexByPropertyIndex(
      const std::string& index_name, const rg::Value& query);
  std::unique_ptr<VertexIterator> QueryVertexByPropertyIndex(
      const std::vector<std::string>& labels, const std::string& property_key,
      const rg::Value& query);
  std::unique_ptr<VertexIterator> QueryVertexByPropertyRange(
      const std::string& index_name, const std::optional<rg::Value>& lower,
      const std::optional<rg::Value>& upper, bool left_closed,
      bool right_closed);
  std::unique_ptr<VertexIterator> QueryVertexByPropertyRange(
      const std::vector<std::string>& labels, const std::string& property_key,
      const std::optional<rg::Value>& lower,
      const std::optional<rg::Value>& upper, bool left_closed,
      bool right_closed);
  std::unique_ptr<EdgeIterator> QueryEdgeByPropertyIndex(
      const std::string& index_name, const rg::Value& query);
  std::unique_ptr<EdgeIterator> QueryEdgeByPropertyIndex(
      const std::vector<std::string>& types, const std::string& property_key,
      const rg::Value& query);
  std::unique_ptr<EdgeIterator> QueryEdgeByPropertyRange(
      const std::string& index_name, const std::optional<rg::Value>& lower,
      const std::optional<rg::Value>& upper, bool left_closed,
      bool right_closed);
  std::unique_ptr<EdgeIterator> QueryEdgeByPropertyRange(
      const std::vector<std::string>& types, const std::string& property_key,
      const std::optional<rg::Value>& lower,
      const std::optional<rg::Value>& upper, bool left_closed,
      bool right_closed);
  std::unique_ptr<VertexScoreIterator> QueryVertexByFTIndex(
      const std::string& index_name, const std::string& query, size_t top_n);
  std::unique_ptr<VertexScoreIterator> QueryVertexByKnnSearch(
      const std::string& index_name, const std::vector<float>& query, int top_k,
      int ef_search);
  void AppendPropertyIndexWAL(std::shared_ptr<VertexPropertyIndex> index,
                              const meta::PropertyIndexUpdate& update);
  void AppendEdgePropertyIndexWAL(std::shared_ptr<EdgePropertyIndex> index,
                                  const meta::PropertyIndexUpdate& update);
  void AppendFullTextIndexWAL(std::shared_ptr<VertexFullTextIndex> index,
                              const meta::FullTextIndexUpdate& update);
  void AppendVectorIndexWAL(std::shared_ptr<VertexVectorIndex> index,
                            const meta::VectorIndexUpdate& update) {
    pending_vector_wals_.push_back({std::move(index), update});
  }
  void Commit();
  void Rollback();
  [[nodiscard]] State GetState() const noexcept { return state_; }
  GraphDB* db() { return db_; }
  rocksdb::Transaction* dbtxn() { return txn_; };

 private:
  struct PendingPropertyWAL {
    std::shared_ptr<VertexPropertyIndex> index;
    meta::PropertyIndexUpdate update;
  };

  struct PendingEdgePropertyWAL {
    std::shared_ptr<EdgePropertyIndex> index;
    meta::PropertyIndexUpdate update;
  };

  struct PendingFullTextWAL {
    std::shared_ptr<VertexFullTextIndex> index;
    meta::FullTextIndexUpdate update;
  };

  struct PendingVectorWAL {
    std::shared_ptr<VertexVectorIndex> index;
    meta::VectorIndexUpdate update;
  };

  rocksdb::Transaction* txn_;
  GraphDB* db_;
  std::vector<PendingPropertyWAL> pending_property_wals_;
  std::vector<PendingEdgePropertyWAL> pending_edge_property_wals_;
  std::vector<PendingFullTextWAL> pending_fulltext_wals_;
  std::vector<PendingVectorWAL> pending_vector_wals_;
  State state_ = State::kActive;
};

}  // namespace graphdb
