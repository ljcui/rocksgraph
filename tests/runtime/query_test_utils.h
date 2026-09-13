#pragma once

#include <memory>
#include <string_view>
#include <utility>

#include "runtime/query_executor.h"

namespace rg::test {

class ReadOnlyTransaction final : public GraphTransaction {
 public:
  explicit ReadOnlyTransaction(std::unique_ptr<GraphTransaction> delegate)
      : reader_(delegate.get()), delegate_(std::move(delegate)) {}
  ReadOnlyTransaction(const GraphReader &reader,
                      std::unique_ptr<GraphTransaction> delegate)
      : reader_(&reader), delegate_(std::move(delegate)) {}

  [[nodiscard]] bool IsWritable() const noexcept override { return false; }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIds() const override {
    return reader_->ScanNodeIds();
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIds()
      const override {
    return reader_->ScanRelationshipIds();
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIdsByLabels(
      const std::vector<std::string> &labels) const override {
    return reader_->ScanNodeIdsByLabels(labels);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIdsByTypes(
      const std::vector<std::string> &types) const override {
    return reader_->ScanRelationshipIdsByTypes(types);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> RelationshipIdsConnectedTo(
      std::int64_t node_id) const override {
    return reader_->RelationshipIdsConnectedTo(node_id);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OutgoingRelationshipIds(
      std::int64_t node_id) const override {
    return reader_->OutgoingRelationshipIds(node_id);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> IncomingRelationshipIds(
      std::int64_t node_id) const override {
    return reader_->IncomingRelationshipIds(node_id);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindNodeIdsByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const override {
    return reader_->FindNodeIdsByIndex(labels, property_key, value);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindNodeIdsByIndexRange(
      const std::vector<std::string> &labels, std::string_view property_key,
      const IndexRange &range) const override {
    return reader_->FindNodeIdsByIndexRange(labels, property_key, range);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindRelationshipIdsByIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const Value &value) const override {
    return reader_->FindRelationshipIdsByIndex(relationship_types, property_key,
                                               value);
  }
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindRelationshipIdsByIndexRange(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const IndexRange &range) const override {
    return reader_->FindRelationshipIdsByIndexRange(relationship_types,
                                                    property_key, range);
  }
  [[nodiscard]] std::size_t RelationshipCount() const override {
    return reader_->RelationshipCount();
  }
  [[nodiscard]] NodePtr NodeById(std::int64_t id) const override {
    return reader_->NodeById(id);
  }
  [[nodiscard]] RelationshipPtr RelationshipById(
      std::int64_t id) const override {
    return reader_->RelationshipById(id);
  }
  [[nodiscard]] Value NodeProperty(
      std::int64_t node_id, std::string_view property_key) const override {
    return reader_->NodeProperty(node_id, property_key);
  }
  [[nodiscard]] Value RelationshipProperty(
      std::int64_t relationship_id,
      std::string_view property_key) const override {
    return reader_->RelationshipProperty(relationship_id, property_key);
  }
  [[nodiscard]] State GetState() const noexcept override {
    return delegate_->GetState();
  }
  void Commit() override { delegate_->Commit(); }
  void Rollback() override { delegate_->Rollback(); }

 private:
  const GraphReader *reader_ = nullptr;
  std::unique_ptr<GraphTransaction> delegate_;
};

template <typename TransactionProvider, typename Execute>
QueryResult ExecuteAndCommit(TransactionProvider &provider, Execute execute) {
  auto transaction = provider.BeginTransaction();
  try {
    QueryResult result = execute(*transaction);
    transaction->Commit();
    return result;
  } catch (...) {
    if (transaction->GetState() == GraphTransaction::State::kActive) {
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
    return ExecuteAndCommit(*provider_, [&](GraphTransaction &transaction) {
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
  return ExecuteAndCommit(provider, [&](GraphTransaction &transaction) {
    return ExecuteQuery(transaction, cypher, std::move(options));
  });
}

}  // namespace rg::test
