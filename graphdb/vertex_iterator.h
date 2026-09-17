#pragma once

#include <rocksdb/utilities/transaction.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ftindex/include/lib.rs.h"
#include "graph_entity.h"
#include "graph_iterator.h"

namespace graphdb {

struct VertexPropertyIndex;

class VertexIterator : public GraphIterator {
 public:
  explicit VertexIterator(Transaction* txn) : GraphIterator(txn) {}
  virtual Vertex& GetVertex() = 0;
};

class EmptyVertexIterator final : public VertexIterator {
 public:
  explicit EmptyVertexIterator(Transaction* txn) : VertexIterator(txn) {}
  void Next() override {}
  Vertex& GetVertex() override;
};

class VertexScanIterator final : public VertexIterator {
 public:
  explicit VertexScanIterator(Transaction* txn);
  void Next() override;
  Vertex& GetVertex() override;

 private:
  void SeekToNextVertex();

  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::unique_ptr<Vertex> vertex_;
};

class VertexLabelScanIterator final : public VertexIterator {
 public:
  VertexLabelScanIterator(Transaction* txn, uint32_t label_id);
  void Next() override;
  Vertex& GetVertex() override;

 private:
  void LoadCurrent();

  uint32_t label_id_ = 0;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::unique_ptr<Vertex> vertex_;
};

class VertexPropertyFilterIterator final : public VertexIterator {
 public:
  VertexPropertyFilterIterator(
      std::unique_ptr<VertexIterator> source,
      std::unordered_map<uint32_t, rg::Value> properties);
  void Next() override;
  Vertex& GetVertex() override;

 private:
  void SeekToNextMatch();
  bool Matches(Vertex& vertex) const;

  std::unique_ptr<VertexIterator> source_;
  std::unordered_map<uint32_t, rg::Value> properties_;
};

class VertexUniqueIndexIterator final : public VertexIterator {
 public:
  VertexUniqueIndexIterator(Transaction* txn,
                            std::shared_ptr<VertexPropertyIndex> index,
                            std::vector<rg::Value> values);
  void Next() override { valid_ = false; }
  Vertex& GetVertex() override;

 private:
  std::shared_ptr<VertexPropertyIndex> index_;
  std::unique_ptr<Vertex> vertex_;
};

class VertexIndexSeekIterator final : public VertexIterator {
 public:
  VertexIndexSeekIterator(Transaction* txn,
                          std::shared_ptr<VertexPropertyIndex> index,
                          std::string prefix);
  void Next() override;
  Vertex& GetVertex() override;

 private:
  void SeekToNextValid();

  std::shared_ptr<VertexPropertyIndex> index_;
  std::string prefix_;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::unique_ptr<Vertex> vertex_;
};

class VertexIndexRangeIterator final : public VertexIterator {
 public:
  VertexIndexRangeIterator(Transaction* txn,
                           std::shared_ptr<VertexPropertyIndex> index,
                           std::optional<std::string> lower_key,
                           std::optional<std::string> upper_key,
                           bool left_closed, bool right_closed);
  void Next() override;
  Vertex& GetVertex() override;

 private:
  void SeekToNextValid();

  std::shared_ptr<VertexPropertyIndex> index_;
  std::string index_prefix_;
  std::optional<std::string> lower_key_;
  std::optional<std::string> upper_key_;
  bool left_closed_ = true;
  bool right_closed_ = true;
  std::unique_ptr<rocksdb::Iterator> iterator_;
  std::unique_ptr<Vertex> vertex_;
};

struct VertexScore {
  Vertex vertex;
  float score;
  VertexScore(Vertex vertex, float score)
      : vertex(std::move(vertex)), score(score) {}
};

class VertexScoreIterator : public GraphIterator {
 public:
  explicit VertexScoreIterator(Transaction* txn) : GraphIterator(txn) {}
  virtual VertexScore& GetVertexScore() = 0;
};

class VertexFullTextSearchIterator final : public VertexScoreIterator {
 public:
  VertexFullTextSearchIterator(Transaction* txn, const std::string& index_name,
                               const std::string& query, size_t top_n);
  void Next() override;
  VertexScore& GetVertexScore() override;

 private:
  ::rust::Vec<::IdScore> result_;
  size_t result_index_ = 0;
  std::unique_ptr<VertexScore> vertex_score_;
};

class VertexKnnSearchIterator final : public VertexScoreIterator {
 public:
  VertexKnnSearchIterator(Transaction* txn, const std::string& index_name,
                          const std::vector<float>& query, int top_k,
                          int ef_search);
  void Next() override;
  VertexScore& GetVertexScore() override;

 private:
  std::vector<std::pair<int64_t, float>> result_;
  size_t result_index_ = 0;
  std::unique_ptr<VertexScore> vertex_score_;
};

}  // namespace graphdb
