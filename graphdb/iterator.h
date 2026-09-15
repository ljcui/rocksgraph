//
// Created by botu.wzy
//

#pragma once

namespace graphdb {
class Transaction;

class Iterator {
 public:
  explicit Iterator(Transaction* txn) : txn_(txn) {}
  // No copying allowed
  Iterator(const Iterator&) = delete;
  void operator=(const Iterator&) = delete;

  Transaction* GetTxn() { return txn_; }
  virtual bool Valid() { return valid_; };
  virtual void Next() = 0;
  virtual ~Iterator() = default;

 protected:
  Transaction* txn_ = nullptr;
  bool valid_ = false;
};
}  // namespace graphdb
