#pragma once

namespace graphdb {

class Transaction;

class GraphIterator {
 public:
  explicit GraphIterator(Transaction* transaction) : txn_(transaction) {}
  GraphIterator(const GraphIterator&) = delete;
  GraphIterator& operator=(const GraphIterator&) = delete;
  virtual ~GraphIterator() = default;

  [[nodiscard]] Transaction* transaction() const noexcept { return txn_; }
  [[nodiscard]] bool Valid() const noexcept { return valid_; }
  virtual void Next() = 0;

 protected:
  Transaction* txn_ = nullptr;
  bool valid_ = false;
};

}  // namespace graphdb
