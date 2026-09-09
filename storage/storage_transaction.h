#pragma once

namespace rg {

class GraphReader;
class Storage;

class StorageTransaction {
 public:
  enum class State { kActive, kCommitted, kRolledBack };

  StorageTransaction() = default;
  StorageTransaction(const StorageTransaction &) = delete;
  StorageTransaction &operator=(const StorageTransaction &) = delete;
  virtual ~StorageTransaction() = default;

  // The reader and optional writer expose the transaction's data view. A
  // read-only transaction may return nullptr from Writer().
  [[nodiscard]] virtual const GraphReader &Reader() const = 0;
  [[nodiscard]] virtual Storage *Writer() = 0;
  [[nodiscard]] virtual State GetState() const noexcept = 0;
  virtual void Commit() = 0;
  virtual void Rollback() = 0;
};

}  // namespace rg
