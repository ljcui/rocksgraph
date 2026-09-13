//
// Created by botu.wzy
//

#pragma once
#include <rocksdb/utilities/transaction_db.h>

#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
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
}
namespace bolt {
class BoltConnection;
}
namespace txn {
class Transaction {
 public:
  // No copying allowed
  Transaction(const Transaction&) = delete;
  void operator=(const Transaction&) = delete;

  Transaction(rocksdb::Transaction* txn, graphdb::GraphDB* graph_db)
      : txn_(txn), db_(graph_db) {}
  ~Transaction() { delete txn_; }
  graphdb::Vertex CreateVertex(
      const std::unordered_set<std::string>& labels,
      const std::unordered_map<std::string, rg::Value>& values);
  graphdb::Edge CreateEdge(
      const graphdb::Vertex& start, const graphdb::Vertex& end,
      const std::string& type,
      const std::unordered_map<std::string, rg::Value>& values);
  graphdb::Vertex GetVertexById(int64_t vid);
  graphdb::Edge GetEdgeById(uint32_t etid, int64_t eid);
  std::unique_ptr<graphdb::VertexIterator> NewVertexIterator();
  std::unique_ptr<graphdb::VertexIterator> NewVertexIterator(
      const std::string& label);
  std::unique_ptr<graphdb::VertexIterator> NewVertexIterator(
      const std::optional<std::string>& label,
      const std::optional<std::unordered_map<std::string, rg::Value>>& props);
  std::unique_ptr<graphdb::EdgeIterator> NewEdgeIterator();
  std::unique_ptr<graphdb::EdgeIterator> NewEdgeIterator(
      const std::unordered_set<std::string>& types);
  // for debug
  std::string GetVertexIteratorInfo(
      const std::optional<std::string>& label,
      const std::optional<std::unordered_set<std::string>>& props);
  std::unique_ptr<graphdb::VertexIterator> QueryVertexByPropertyIndex(
      const std::string& index_name, const rg::Value& query);
  std::unique_ptr<graphdb::VertexIterator> QueryVertexByPropertyRange(
      const std::string& index_name, const std::optional<rg::Value>& lower,
      const std::optional<rg::Value>& upper, bool left_closed,
      bool right_closed);
  std::unique_ptr<graphdb::EdgeIterator> QueryEdgeByPropertyIndex(
      const std::string& index_name, const rg::Value& query);
  std::unique_ptr<graphdb::EdgeIterator> QueryEdgeByPropertyRange(
      const std::string& index_name, const std::optional<rg::Value>& lower,
      const std::optional<rg::Value>& upper, bool left_closed,
      bool right_closed);
  std::unique_ptr<graphdb::VertexScoreIterator> QueryVertexByFTIndex(
      const std::string& index_name, const std::string& query, size_t top_n);
  std::unique_ptr<graphdb::VertexScoreIterator> QueryVertexByKnnSearch(
      const std::string& index_name, const std::vector<float>& query, int top_k,
      int ef_search);
  void AppendPropertyIndexWAL(
      std::shared_ptr<graphdb::VertexPropertyIndex> index,
      const meta::PropertyIndexUpdate& update);
  void AppendEdgePropertyIndexWAL(
      std::shared_ptr<graphdb::EdgePropertyIndex> index,
      const meta::PropertyIndexUpdate& update);
  void AppendFullTextIndexWAL(
      std::shared_ptr<graphdb::VertexFullTextIndex> index,
      const meta::FullTextIndexUpdate& update);
  void AppendVectorIndexWAL(std::shared_ptr<graphdb::VertexVectorIndex> index,
                            const meta::VectorIndexUpdate& update) {
    pending_vector_wals_.push_back({std::move(index), update});
  }
  void Commit();
  void Rollback();
  graphdb::GraphDB* db() { return db_; }
  rocksdb::Transaction* dbtxn() { return txn_; };
  void SetConn(const std::shared_ptr<bolt::BoltConnection>& conn) {
    conn_ = conn;
  }
  std::shared_ptr<bolt::BoltConnection>& conn() { return conn_; }

 private:
  struct PendingPropertyWAL {
    std::shared_ptr<graphdb::VertexPropertyIndex> index;
    meta::PropertyIndexUpdate update;
  };

  struct PendingEdgePropertyWAL {
    std::shared_ptr<graphdb::EdgePropertyIndex> index;
    meta::PropertyIndexUpdate update;
  };

  struct PendingFullTextWAL {
    std::shared_ptr<graphdb::VertexFullTextIndex> index;
    meta::FullTextIndexUpdate update;
  };

  struct PendingVectorWAL {
    std::shared_ptr<graphdb::VertexVectorIndex> index;
    meta::VectorIndexUpdate update;
  };

  rocksdb::Transaction* txn_;
  graphdb::GraphDB* db_;
  std::shared_ptr<bolt::BoltConnection> conn_;
  std::vector<PendingPropertyWAL> pending_property_wals_;
  std::vector<PendingEdgePropertyWAL> pending_edge_property_wals_;
  std::vector<PendingFullTextWAL> pending_fulltext_wals_;
  std::vector<PendingVectorWAL> pending_vector_wals_;
};

}  // namespace txn
