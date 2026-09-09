#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include "runtime/query_executor.h"

namespace rg::test {

class ReadOnlyTransaction final : public StorageTransaction {
 public:
  explicit ReadOnlyTransaction(std::unique_ptr<StorageTransaction> delegate)
      : delegate_(std::move(delegate)) {}

  [[nodiscard]] const GraphReader &Reader() const override {
    return delegate_->Reader();
  }
  [[nodiscard]] Storage *Writer() override { return nullptr; }
  [[nodiscard]] State GetState() const noexcept override {
    return delegate_->GetState();
  }
  void Commit() override { delegate_->Commit(); }
  void Rollback() override { delegate_->Rollback(); }

 private:
  std::unique_ptr<StorageTransaction> delegate_;
};

template <typename TransactionProvider, typename Execute>
QueryResult ExecuteAndCommit(TransactionProvider &provider, Execute execute) {
  auto transaction = provider.BeginTransaction();
  try {
    QueryResult result = execute(*transaction);
    transaction->Commit();
    return result;
  } catch (...) {
    if (transaction->GetState() == StorageTransaction::State::kActive) {
      transaction->Rollback();
    }
    throw;
  }
}

template <typename TransactionProvider>
class CommittingQueryExecutor {
 public:
  explicit CommittingQueryExecutor(TransactionProvider &provider)
      : provider_(&provider) {}

  QueryResult Execute(const ir::LogicalPlan &plan,
                      const QueryParameters &parameters = {},
                      QueryExecutionOptions options = {}) {
    return ExecuteAndCommit(*provider_, [&](StorageTransaction &transaction) {
      return rg::QueryExecutor(transaction)
          .Execute(plan, parameters, std::move(options));
    });
  }

 private:
  TransactionProvider *provider_;
};

template <typename TransactionProvider>
QueryResult ExecuteQueryAndCommit(TransactionProvider &provider,
                                  std::string_view cypher,
                                  QueryOptions options = {}) {
  return ExecuteAndCommit(provider, [&](StorageTransaction &transaction) {
    return ExecuteQuery(transaction, cypher, std::move(options));
  });
}

}  // namespace rg::test
