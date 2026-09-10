//
// Created by botu.wzy
//

#pragma once
#include <boost/endian/conversion.hpp>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>

#include "common/byte_utils.h"
#include "edge_direction.h"
#include "value/value.h"

namespace rocksdb {
class PinnableSlice;
}
namespace txn {
class Transaction;
}
namespace graphdb {
class Property {
 public:
  virtual rg::Value GetProperty(const std::string&) = 0;
  virtual rg::Value GetProperty(uint32_t) = 0;
  virtual std::unordered_map<std::string, rg::Value> GetAllProperty() = 0;

  virtual void SetProperties(
      const std::unordered_map<std::string, rg::Value>&) = 0;

  virtual void RemoveProperty(const std::string&) = 0;
  virtual void RemoveAllProperty() = 0;
};

class EdgeIterator;

class Vertex : Property {
 public:
  Vertex(txn::Transaction* txn, int64_t id) : txn_(txn), id_(id) {}
  [[nodiscard]] int64_t GetId() const { return id_; };
  [[nodiscard]] int64_t GetNativeId() const {
    return boost::endian::big_to_native(id_);
  };
  [[nodiscard]] std::string_view GetIdView() const {
    return common::AsStringView(id_);
  };

  std::unordered_set<uint32_t> GetLabelIds();
  std::unordered_set<std::string> GetLabels();
  void AddLabels(const std::unordered_set<std::string>& labels);
  void DeleteLabels(const std::unordered_set<std::string>& labels);
  int Delete();
  std::unique_ptr<EdgeIterator> NewEdgeIterator(
      EdgeDirection direction, const std::unordered_set<std::string>& types,
      const std::unordered_map<std::string, rg::Value>& props);
  std::unique_ptr<EdgeIterator> NewEdgeIterator(
      EdgeDirection direction, const std::unordered_set<std::string>& types,
      const std::unordered_map<std::string, rg::Value>& props,
      const std::unordered_set<std::string>& other_node_labels,
      const std::unordered_map<std::string, rg::Value>& other_node_props);
  std::unique_ptr<EdgeIterator> NewEdgeIterator(
      EdgeDirection direction, const std::string& type,
      const std::unordered_map<std::string, rg::Value>& props,
      const Vertex& other_node);
  std::unique_ptr<EdgeIterator> NewEdgeIterator(
      EdgeDirection direction, const std::unordered_set<std::string>& types,
      const std::unordered_map<std::string, rg::Value>& props,
      const Vertex& other_node);
  bool operator==(const Vertex& v) const { return id_ == v.id_; }

  rg::Value GetProperty(const std::string&) override;
  rg::Value GetProperty(uint32_t) override;
  bool TryGetVectorPropertyRaw(uint32_t pid, rocksdb::PinnableSlice* out,
                               size_t* dimensions);
  std::unordered_map<std::string, rg::Value> GetAllProperty() override;
  int GetDegree(EdgeDirection direction);
  void SetProperties(
      const std::unordered_map<std::string, rg::Value>& properties) override;
  void RemoveProperty(const std::string&) override;
  void RemoveAllProperty() override;
  virtual ~Vertex() = default;

 private:
  void Lock();
  txn::Transaction* txn_ = nullptr;
  int64_t id_;
};

class Edge : public Property {
 public:
  Edge(txn::Transaction* txn, int64_t id, int64_t startId, int64_t endId,
       uint32_t typeId)
      : txn_(txn), id_(id), startId_(startId), endId_(endId), typeId_(typeId) {}

  [[nodiscard]] int64_t GetId() const { return id_; };
  [[nodiscard]] int64_t GetNativeId() const {
    return boost::endian::big_to_native(id_);
  };
  [[nodiscard]] Vertex GetStart() const { return {txn_, startId_}; };
  [[nodiscard]] int64_t GetStartId() const { return startId_; };
  [[nodiscard]] int64_t GetNativeStartId() const {
    return boost::endian::big_to_native(startId_);
  };
  [[nodiscard]] Vertex GetEnd() const { return {txn_, endId_}; };
  [[nodiscard]] int64_t GetEndId() const { return endId_; };
  [[nodiscard]] int64_t GetNativeEndId() const {
    return boost::endian::big_to_native(endId_);
  };
  [[nodiscard]] uint32_t GetTypeId() const { return typeId_; };
  [[nodiscard]] Vertex GetOtherEnd(int64_t vid) const;
  std::string GetType();
  void Delete();

  bool operator==(const Edge& e) const {
    return (id_ == e.id_) && (startId_ == e.startId_) && (endId_ == e.endId_) &&
           (typeId_ == e.typeId_);
  }

  rg::Value GetProperty(const std::string&) override;
  rg::Value GetProperty(uint32_t) override;
  std::unordered_map<std::string, rg::Value> GetAllProperty() override;
  void SetProperties(
      const std::unordered_map<std::string, rg::Value>& properties) override;
  void RemoveProperty(const std::string&) override;
  void RemoveAllProperty() override;
  virtual ~Edge() = default;

 private:
  void Lock();
  txn::Transaction* txn_ = nullptr;
  int64_t id_;
  int64_t startId_;
  int64_t endId_;
  uint32_t typeId_;
};
}  // namespace graphdb
