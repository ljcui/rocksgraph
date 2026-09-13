#pragma once

#include <memory>

#include "storage/graph_transaction.h"

namespace rg {

class Storage {
 public:
  Storage() = default;
  Storage(const Storage &) = delete;
  Storage &operator=(const Storage &) = delete;
  virtual ~Storage() = default;

  // The caller owns the returned transaction and must commit it explicitly.
  [[nodiscard]] virtual std::unique_ptr<GraphTransaction>
  BeginTransaction() = 0;
};

}  // namespace rg
