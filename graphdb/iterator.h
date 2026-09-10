//
// Created by botu.wzy
//

#pragma once

namespace txn {
class Transaction;
}
namespace graphdb {
class Iterator {
 public:
  explicit Iterator(txn::Transaction* txn) : txn_(txn) {}
  // No copying allowed
  Iterator(const Iterator&) = delete;
  void operator=(const Iterator&) = delete;

  txn::Transaction* GetTxn() { return txn_; }
  virtual bool Valid() { return valid_; };
  virtual void Next() = 0;
  virtual ~Iterator() = default;

 protected:
  txn::Transaction* txn_ = nullptr;
  bool valid_ = false;
};
}  // namespace graphdb