#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/exception.h"
#include "storage/graph_reader.h"
#include "storage/graph_writer.h"

namespace rg {

// A transaction is the single read/write execution context for a query.
class GraphTransaction : public GraphReader, public GraphWriter {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;
  using GraphWriter::CreateNode;
  using GraphWriter::CreateRelationship;
  enum class State { kActive, kCommitted, kRolledBack };

  GraphTransaction() = default;
  GraphTransaction(const GraphTransaction &) = delete;
  GraphTransaction &operator=(const GraphTransaction &) = delete;
  ~GraphTransaction() override = default;

  [[nodiscard]] virtual bool IsWritable() const noexcept = 0;
  [[nodiscard]] virtual State GetState() const noexcept = 0;
  virtual void Commit() = 0;
  virtual void Rollback() = 0;

  // Read-only transactions can use these defaults. Query execution rejects a
  // write plan before it starts, and direct physical execution fails here.
  NodePtr CreateNode(std::vector<std::string>, Value::Map) override {
    RejectWrite();
  }
  RelationshipPtr CreateRelationship(std::int64_t, std::int64_t, std::string,
                                     Value::Map) override {
    RejectWrite();
  }
  void SetNodeProperty(std::int64_t, std::string, Value) override {
    RejectWrite();
  }
  void SetRelationshipProperty(std::int64_t, std::string, Value) override {
    RejectWrite();
  }
  void SetNodeProperties(std::int64_t, Value::Map, bool) override {
    RejectWrite();
  }
  void SetRelationshipProperties(std::int64_t, Value::Map, bool) override {
    RejectWrite();
  }
  void SetLabels(std::int64_t, std::vector<std::string>) override {
    RejectWrite();
  }
  void RemoveNodeProperty(std::int64_t, std::string_view) override {
    RejectWrite();
  }
  void RemoveRelationshipProperty(std::int64_t, std::string_view) override {
    RejectWrite();
  }
  void RemoveLabels(std::int64_t, const std::vector<std::string> &) override {
    RejectWrite();
  }
  void DeleteNode(std::int64_t) override { RejectWrite(); }
  void DeleteRelationship(std::int64_t) override { RejectWrite(); }

 private:
  [[noreturn]] static void RejectWrite() {
    THROW(common::InvalidArgumentError,
          "write execution requires a writable transaction");
  }
};

}  // namespace rg
