//
// Created by botu.wzy
//

#pragma once
#include <rocksdb/utilities/transaction.h>

#include <memory>
#include <optional>
#include <queue>
#include <unordered_set>

#include "edge_direction.h"
#include "graph_entity.h"
#include "iterator.h"
namespace graphdb {
struct EdgePropertyIndex;
class EdgeIterator : public Iterator {
 public:
  explicit EdgeIterator(txn::Transaction* txn) : Iterator(txn) {}
  virtual Edge& GetEdge() = 0;
};

class NoEdgeFound : public EdgeIterator {
 public:
  explicit NoEdgeFound(txn::Transaction* txn) : EdgeIterator(txn) {
    valid_ = false;  // always false
  }
  void Next() override {}
  Edge& GetEdge() override {
    assert(valid_);
    return *ee_;
  };

 private:
  std::unique_ptr<Edge> ee_;
};

class ScanEdgeByTypes : public EdgeIterator {
 public:
  ScanEdgeByTypes(txn::Transaction* txn, std::unordered_set<uint32_t> types);
  void Next() override;
  Edge& GetEdge() override {
    assert(valid_);
    return *ee_;
  }

 private:
  void Load();
  void SeekToNextPrefix();
  void CheckIteratorStatus() const;

  bool scan_all_ = false;
  std::unique_ptr<rocksdb::Iterator> iter_;
  std::queue<std::string> prefixes_;
  std::string prefix_;
  std::unique_ptr<Edge> ee_;
};

class ScanEdgeByVidDirectionTypes : public EdgeIterator {
 public:
  ScanEdgeByVidDirectionTypes(txn::Transaction* txn, int64_t vid,
                              EdgeDirection direction,
                              std::unordered_set<uint32_t> types);
  void Next() override;
  Edge& GetEdge() override {
    assert(valid_);
    return *ee_;
  };

 private:
  void Load();
  void SeekToNextPrefix();
  int64_t vid_;
  EdgeDirection direction_;
  std::unordered_set<uint32_t> types_;
  std::unique_ptr<rocksdb::Iterator> iter_;
  std::string prefix_;
  std::queue<std::string> prefixes_;
  std::unique_ptr<Edge> ee_;
};

class ScanEdgeByVidDirectionTypesProperties : public EdgeIterator {
 public:
  ScanEdgeByVidDirectionTypesProperties(
      txn::Transaction* txn, int64_t vid, EdgeDirection direction,
      std::unordered_set<uint32_t> types,
      std::unordered_map<uint32_t, rg::Value> properties);
  void Next() override;
  Edge& GetEdge() override { return iter_->GetEdge(); };

 private:
  bool MatchProperties();
  std::unique_ptr<ScanEdgeByVidDirectionTypes> iter_;
  std::unordered_map<uint32_t, rg::Value> properties_;
};

class ScanEdgeByVidDirectionTypesPropertiesOtherNode : public EdgeIterator {
 public:
  ScanEdgeByVidDirectionTypesPropertiesOtherNode(
      txn::Transaction* txn, int64_t vid, EdgeDirection direction,
      std::unordered_set<uint32_t> types,
      std::unordered_map<uint32_t, rg::Value> properties,
      std::unordered_set<uint32_t> other_node_labels,
      std::unordered_map<uint32_t, rg::Value> other_node_properties);
  void Next() override;
  Edge& GetEdge() override { return iter_->GetEdge(); };

 private:
  bool MatchOtherEnd(Vertex& other_end);
  int64_t vid_;
  std::unique_ptr<ScanEdgeByVidDirectionTypesProperties> iter_;
  std::unordered_set<uint32_t> other_node_labels_;
  std::unordered_map<uint32_t, rg::Value> other_node_properties_;
};

class ScanEdgeByVidDirectionTypesPropertiesOtherVid : public EdgeIterator {
 public:
  ScanEdgeByVidDirectionTypesPropertiesOtherVid(
      txn::Transaction* txn, int64_t vid, EdgeDirection direction,
      std::unordered_set<uint32_t> types,
      std::unordered_map<uint32_t, rg::Value> properties, const Vertex& other);
  void Next() override;
  Edge& GetEdge() override { return iter_->GetEdge(); };

 private:
  int64_t vid_;
  std::unique_ptr<ScanEdgeByVidDirectionTypesProperties> iter_;
  const Vertex& other_node_;
};

class ScanEdgeByVidDirectionTypePropertiesOtherNode : public EdgeIterator {
 public:
  ScanEdgeByVidDirectionTypePropertiesOtherNode(
      txn::Transaction* txn, int64_t vid, EdgeDirection direction,
      uint32_t type, std::unordered_map<uint32_t, rg::Value> properties,
      const Vertex& other_node);
  void Next() override;
  Edge& GetEdge() override { return iter_->GetEdge(); };

 private:
  bool MatchProperties();
  int64_t vid_;
  std::unordered_map<uint32_t, rg::Value> properties_;
  const Vertex& other_node_;
  std::unique_ptr<ScanEdgeByVidDirectionTypes> iter_;
};

class GetEdgeByPropertyIndex : public EdgeIterator {
 public:
  GetEdgeByPropertyIndex(txn::Transaction* txn,
                         std::shared_ptr<EdgePropertyIndex> index,
                         std::string prefix);
  void Next() override;
  Edge& GetEdge() override {
    assert(valid_);
    return *ee_;
  }

 private:
  void SeekToNextValid();
  std::shared_ptr<EdgePropertyIndex> index_;
  std::string prefix_;
  std::unique_ptr<rocksdb::Iterator> iter_;
  std::unique_ptr<Edge> ee_;
};

class GetEdgeByPropertyRange : public EdgeIterator {
 public:
  GetEdgeByPropertyRange(txn::Transaction* txn,
                         std::shared_ptr<EdgePropertyIndex> index,
                         std::optional<std::string> lower_key,
                         std::optional<std::string> upper_key, bool left_closed,
                         bool right_closed);
  void Next() override;
  Edge& GetEdge() override {
    assert(valid_);
    return *ee_;
  }

 private:
  void SeekToNextValid();
  std::shared_ptr<EdgePropertyIndex> index_;
  std::string index_prefix_;
  std::optional<std::string> lower_key_;
  std::optional<std::string> upper_key_;
  bool left_closed_ = true;
  bool right_closed_ = true;
  std::unique_ptr<rocksdb::Iterator> iter_;
  std::unique_ptr<Edge> ee_;
};
}  // namespace graphdb
