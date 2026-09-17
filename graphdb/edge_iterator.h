#pragma once

#include <rocksdb/utilities/transaction.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "edge_direction.h"
#include "graph_entity.h"
#include "graph_iterator.h"

namespace graphdb {

struct EdgePropertyIndex;

class EdgeIterator : public GraphIterator {
 public:
  explicit EdgeIterator(Transaction* txn) : GraphIterator(txn) {}
  virtual Edge& GetEdge() = 0;
};

class EmptyEdgeIterator final : public EdgeIterator {
 public:
  explicit EmptyEdgeIterator(Transaction* txn) : EdgeIterator(txn) {}
  void Next() override {}
  Edge& GetEdge() override;
};

class EdgeTypeScanIterator final : public EdgeIterator {
 public:
  EdgeTypeScanIterator(Transaction* txn, std::unordered_set<uint32_t> type_ids);
  void Next() override;
  Edge& GetEdge() override;

 private:
  void LoadCurrent();
  void SeekToNextPrefix();
  void CheckIteratorStatus() const;

  bool scan_all_ = false;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::queue<std::string> prefixes_;
  std::string prefix_;
  std::unique_ptr<Edge> edge_;
};

class IncidentEdgeIterator final : public EdgeIterator {
 public:
  IncidentEdgeIterator(Transaction* txn, int64_t vertex_id,
                       EdgeDirection direction,
                       std::unordered_set<uint32_t> type_ids);
  void Next() override;
  Edge& GetEdge() override;

 private:
  bool LoadCurrent();
  void SeekToNextPrefix();

  int64_t vertex_id_;
  EdgeDirection direction_;
  std::unordered_set<uint32_t> type_ids_;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::string prefix_;
  std::queue<std::string> prefixes_;
  std::unique_ptr<Edge> edge_;
};

class EdgePropertyFilterIterator final : public EdgeIterator {
 public:
  EdgePropertyFilterIterator(
      std::unique_ptr<EdgeIterator> source,
      std::unordered_map<uint32_t, rg::Value> properties);
  void Next() override;
  Edge& GetEdge() override;

 private:
  void SeekToNextMatch();
  bool Matches(Edge& edge) const;

  std::unique_ptr<EdgeIterator> source_;
  std::unordered_map<uint32_t, rg::Value> properties_;
};

class EdgeOtherVertexFilterIterator final : public EdgeIterator {
 public:
  EdgeOtherVertexFilterIterator(
      std::unique_ptr<EdgeIterator> source, int64_t origin_vertex_id,
      std::optional<int64_t> other_vertex_id,
      std::unordered_set<uint32_t> other_vertex_labels,
      std::unordered_map<uint32_t, rg::Value> other_vertex_properties);
  void Next() override;
  Edge& GetEdge() override;

 private:
  void SeekToNextMatch();
  bool Matches(Edge& edge) const;

  std::unique_ptr<EdgeIterator> source_;
  int64_t origin_vertex_id_;
  std::optional<int64_t> other_vertex_id_;
  std::unordered_set<uint32_t> other_vertex_labels_;
  std::unordered_map<uint32_t, rg::Value> other_vertex_properties_;
};

class EdgeIndexSeekIterator final : public EdgeIterator {
 public:
  EdgeIndexSeekIterator(Transaction* txn,
                        std::shared_ptr<EdgePropertyIndex> index,
                        std::string prefix);
  void Next() override;
  Edge& GetEdge() override;

 private:
  void SeekToNextValid();

  std::shared_ptr<EdgePropertyIndex> index_;
  std::string prefix_;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::unique_ptr<Edge> edge_;
};

class EdgeIndexRangeIterator final : public EdgeIterator {
 public:
  EdgeIndexRangeIterator(Transaction* txn,
                         std::shared_ptr<EdgePropertyIndex> index,
                         std::optional<std::string> lower_key,
                         std::optional<std::string> upper_key, bool left_closed,
                         bool right_closed);
  void Next() override;
  Edge& GetEdge() override;

 private:
  void SeekToNextValid();

  std::shared_ptr<EdgePropertyIndex> index_;
  std::string index_prefix_;
  std::optional<std::string> lower_key_;
  std::optional<std::string> upper_key_;
  bool left_closed_ = true;
  bool right_closed_ = true;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::unique_ptr<Edge> edge_;
};

}  // namespace graphdb
