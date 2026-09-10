#include "storage/graph_db_storage.h"

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "graphdb/assistant_pool.h"
#include "graphdb/edge_direction.h"
#include "graphdb/edge_iterator.h"
#include "graphdb/graph_db.h"
#include "graphdb/graph_entity.h"
#include "graphdb/index.h"
#include "graphdb/vertex_iterator.h"
#include "transaction/transaction.h"

namespace rg {
namespace {

class VectorEntityIdCursor final : public EntityIdCursor {
 public:
  explicit VectorEntityIdCursor(std::vector<std::int64_t> ids)
      : ids_(std::move(ids)) {}
  ~VectorEntityIdCursor() override { Close(); }

  [[nodiscard]] bool Next() override {
    if (closed_ || next_ >= ids_.size()) {
      Close();
      return false;
    }
    current_ = ids_[next_++];
    positioned_ = true;
    return true;
  }

  [[nodiscard]] std::int64_t Id() const override {
    CHECK(positioned_ && !closed_, common::InvalidArgumentError,
          "entity cursor is not positioned");
    return current_;
  }

  void Close() noexcept override {
    ids_.clear();
    closed_ = true;
  }

 private:
  std::vector<std::int64_t> ids_;
  std::size_t next_ = 0;
  std::int64_t current_ = -1;
  bool positioned_ = false;
  bool closed_ = false;
};

std::unique_ptr<EntityIdCursor> MakeCursor(std::vector<std::int64_t> ids) {
  return std::make_unique<VectorEntityIdCursor>(std::move(ids));
}

std::vector<std::int64_t> DrainCursor(std::unique_ptr<EntityIdCursor> cursor) {
  std::vector<std::int64_t> ids;
  while (cursor->Next()) {
    ids.push_back(cursor->Id());
  }
  cursor->Close();
  return ids;
}

std::unordered_map<std::string, Value> ToGraphDBProperties(
    const Value::Map &properties) {
  std::unordered_map<std::string, Value> result;
  result.reserve(properties.size());
  for (const auto &[key, value] : properties) {
    if (!value.IsNull()) {
      result.emplace(key, value);
    }
  }
  return result;
}

Value::Map ToQueryProperties(
    const std::unordered_map<std::string, Value> &properties) {
  Value::Map result;
  for (const auto &[key, value] : properties) {
    if (!value.IsNull()) {
      result.emplace(key, value);
    }
  }
  return result;
}

std::vector<std::string> SortedLabels(
    const std::unordered_set<std::string> &labels) {
  std::vector<std::string> result(labels.begin(), labels.end());
  std::sort(result.begin(), result.end());
  return result;
}

bool ContainsAllLabels(const std::unordered_set<std::string> &actual,
                       const std::vector<std::string> &required) {
  return std::all_of(
      required.begin(), required.end(),
      [&actual](const std::string &label) { return actual.contains(label); });
}

ValueType IndexValueGroup(const Value &value) {
  return value.IsDouble() ? ValueType::kInteger : value.Type();
}

bool IsInvalidBound(const Value &value) {
  return value.IsNull() || (value.IsDouble() && std::isnan(value.AsDouble()));
}

bool SupportsGraphDBRange(const IndexRange &range) {
  const auto supported = [](const IndexRangeBound &bound) {
    return !bound.value.IsList() && !bound.value.IsMap();
  };
  return std::all_of(range.lower_bounds.begin(), range.lower_bounds.end(),
                     supported) &&
         std::all_of(range.upper_bounds.begin(), range.upper_bounds.end(),
                     supported);
}

bool MatchesRange(const Value &value, const IndexRange &range) {
  if (IsInvalidBound(value)) {
    return false;
  }
  if (range.prefix.has_value() &&
      (!value.IsString() || !range.prefix->IsString() ||
       !value.AsString().starts_with(range.prefix->AsString()))) {
    return false;
  }
  for (const auto &bound : range.lower_bounds) {
    if (IsInvalidBound(bound.value) ||
        IndexValueGroup(value) != IndexValueGroup(bound.value) ||
        !(ValueLess(bound.value, value) ||
          (bound.inclusive && ValuesEqual(value, bound.value)))) {
      return false;
    }
  }
  for (const auto &bound : range.upper_bounds) {
    if (IsInvalidBound(bound.value) ||
        IndexValueGroup(value) != IndexValueGroup(bound.value) ||
        !(ValueLess(value, bound.value) ||
          (bound.inclusive && ValuesEqual(value, bound.value)))) {
      return false;
    }
  }
  return true;
}

std::unordered_set<std::string> ToStringSet(
    const std::vector<std::string> &values) {
  return {values.begin(), values.end()};
}

}  // namespace

class GraphDBStorage::Transaction final : public StorageTransaction,
                                          public Storage {
 public:
  explicit Transaction(std::unique_ptr<txn::Transaction> transaction)
      : transaction_(std::move(transaction)) {
    CHECK(transaction_ != nullptr, common::InternalError,
          "graphdb transaction is null");
  }

  ~Transaction() override {
    if (state_ == State::kActive) {
      try {
        transaction_->Rollback();
      } catch (...) {
      }
    }
  }

  [[nodiscard]] const GraphReader &Reader() const override { return *this; }
  [[nodiscard]] Storage *Writer() override {
    EnsureActive();
    return this;
  }
  [[nodiscard]] State GetState() const noexcept override { return state_; }

  void Commit() override {
    EnsureActive();
    transaction_->Commit();
    state_ = State::kCommitted;
  }

  void Rollback() override {
    EnsureActive();
    transaction_->Rollback();
    state_ = State::kRolledBack;
  }

  [[nodiscard]] std::unique_ptr<StorageTransaction> BeginTransaction()
      override {
    THROW(common::InvalidArgumentError,
          "cannot begin a transaction from a transaction view");
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIds() const override {
    return CollectNodeIds(transaction_->NewVertexIterator(), {});
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIds()
      const override {
    return CollectRelationshipIds(std::nullopt,
                                  graphdb::EdgeDirection::OUTGOING, {});
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIdsByLabels(
      const std::vector<std::string> &labels) const override {
    if (labels.empty()) {
      return ScanNodeIds();
    }
    return CollectNodeIds(transaction_->NewVertexIterator(labels.front()),
                          labels);
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIdsByTypes(
      const std::vector<std::string> &types) const override {
    return CollectRelationshipIds(std::nullopt,
                                  graphdb::EdgeDirection::OUTGOING, types);
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> RelationshipIdsConnectedTo(
      std::int64_t node_id) const override {
    return CollectRelationshipIds(node_id, graphdb::EdgeDirection::BOTH, {});
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> OutgoingRelationshipIds(
      std::int64_t node_id) const override {
    return CollectRelationshipIds(node_id, graphdb::EdgeDirection::OUTGOING,
                                  {});
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> IncomingRelationshipIds(
      std::int64_t node_id) const override {
    return CollectRelationshipIds(node_id, graphdb::EdgeDirection::INCOMING,
                                  {});
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindNodeIdsByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const override {
    if (value.IsNull()) {
      return MakeCursor({});
    }
    std::unique_ptr<graphdb::VertexIterator> iterator;
    if (auto index = FindReadyNodeIndex(labels, property_key)) {
      iterator = transaction_->QueryVertexByPropertyIndex(index->Name(), value);
    } else if (labels.empty()) {
      iterator = transaction_->NewVertexIterator();
    } else {
      iterator = transaction_->NewVertexIterator(labels.front());
    }

    std::vector<std::int64_t> ids;
    while (iterator->Valid()) {
      graphdb::Vertex &vertex = iterator->GetVertex();
      const Value property = vertex.GetProperty(std::string(property_key));
      if (!property.IsNull() && ContainsAllLabels(vertex.GetLabels(), labels) &&
          ValuesEqual(property, value)) {
        ids.push_back(vertex.GetNativeId());
      }
      iterator->Next();
    }
    return MakeCursor(std::move(ids));
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindNodeIdsByIndexRange(
      const std::vector<std::string> &labels, std::string_view property_key,
      const IndexRange &range) const override {
    CHECK(range.prefix.has_value() || !range.lower_bounds.empty() ||
              !range.upper_bounds.empty(),
          common::InvalidArgumentError, "index range has no bounds");
    const Value &sample =
        range.prefix.has_value()
            ? *range.prefix
            : (!range.lower_bounds.empty() ? range.lower_bounds.front().value
                                           : range.upper_bounds.front().value);
    if (IsInvalidBound(sample) ||
        (range.prefix.has_value() && !range.prefix->IsString())) {
      return MakeCursor({});
    }

    std::unique_ptr<graphdb::VertexIterator> iterator;
    auto index = FindReadyNodeIndex(labels, property_key);
    if (index != nullptr && !range.prefix.has_value() &&
        SupportsGraphDBRange(range)) {
      const std::optional<Value> lower =
          range.lower_bounds.empty()
              ? std::nullopt
              : std::optional<Value>(range.lower_bounds.front().value);
      const std::optional<Value> upper =
          range.upper_bounds.empty()
              ? std::nullopt
              : std::optional<Value>(range.upper_bounds.front().value);
      iterator = transaction_->QueryVertexByPropertyRange(
          index->Name(), lower, upper,
          range.lower_bounds.empty() || range.lower_bounds.front().inclusive,
          range.upper_bounds.empty() || range.upper_bounds.front().inclusive);
    } else if (labels.empty()) {
      iterator = transaction_->NewVertexIterator();
    } else {
      iterator = transaction_->NewVertexIterator(labels.front());
    }

    std::vector<std::int64_t> ids;
    while (iterator->Valid()) {
      graphdb::Vertex &vertex = iterator->GetVertex();
      const Value property = vertex.GetProperty(std::string(property_key));
      if (ContainsAllLabels(vertex.GetLabels(), labels) &&
          MatchesRange(property, range)) {
        ids.push_back(vertex.GetNativeId());
      }
      iterator->Next();
    }
    return MakeCursor(std::move(ids));
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindRelationshipIdsByIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const Value &value) const override {
    if (value.IsNull()) {
      return MakeCursor({});
    }
    return FilterRelationships(
        relationship_types, [property_key, &value](graphdb::Edge &edge) {
          const Value property = edge.GetProperty(std::string(property_key));
          return !property.IsNull() && ValuesEqual(property, value);
        });
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindRelationshipIdsByIndexRange(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const IndexRange &range) const override {
    CHECK(range.prefix.has_value() || !range.lower_bounds.empty() ||
              !range.upper_bounds.empty(),
          common::InvalidArgumentError, "index range has no bounds");
    const Value &sample =
        range.prefix.has_value()
            ? *range.prefix
            : (!range.lower_bounds.empty() ? range.lower_bounds.front().value
                                           : range.upper_bounds.front().value);
    if (IsInvalidBound(sample) ||
        (range.prefix.has_value() && !range.prefix->IsString())) {
      return MakeCursor({});
    }
    return FilterRelationships(relationship_types, [property_key, &range](
                                                       graphdb::Edge &edge) {
      return MatchesRange(edge.GetProperty(std::string(property_key)), range);
    });
  }

  [[nodiscard]] std::size_t RelationshipCount() const override {
    auto cursor = ScanRelationshipIds();
    std::size_t count = 0;
    while (cursor->Next()) {
      ++count;
    }
    return count;
  }

  [[nodiscard]] NodePtr NodeById(std::int64_t id) const override {
    EnsureActive();
    return SnapshotVertex(
        transaction_->GetVertexById(boost::endian::native_to_big(id)));
  }

  [[nodiscard]] RelationshipPtr RelationshipById(
      std::int64_t id) const override {
    EnsureActive();
    const auto cached = relationship_cache_.find(id);
    if (cached != relationship_cache_.end()) {
      return cached->second;
    }
    graphdb::Edge edge = ResolveEdge(id);
    return CacheEdge(edge);
  }

  [[nodiscard]] Value NodeProperty(
      std::int64_t node_id, std::string_view property_key) const override {
    EnsureActive();
    return transaction_->GetVertexById(boost::endian::native_to_big(node_id))
        .GetProperty(std::string(property_key));
  }

  [[nodiscard]] Value RelationshipProperty(
      std::int64_t relationship_id,
      std::string_view property_key) const override {
    EnsureActive();
    return ResolveEdge(relationship_id).GetProperty(std::string(property_key));
  }

  NodePtr CreateNode(std::vector<std::string> labels,
                     Value::Map properties) override {
    EnsureActive();
    graphdb::Vertex vertex = transaction_->CreateVertex(
        ToStringSet(labels), ToGraphDBProperties(properties));
    return SnapshotVertex(vertex);
  }

  RelationshipPtr CreateRelationship(std::int64_t start_node_id,
                                     std::int64_t end_node_id, std::string type,
                                     Value::Map properties) override {
    EnsureActive();
    graphdb::Vertex start = transaction_->GetVertexById(
        boost::endian::native_to_big(start_node_id));
    graphdb::Vertex end =
        transaction_->GetVertexById(boost::endian::native_to_big(end_node_id));
    graphdb::Edge edge = transaction_->CreateEdge(
        start, end, type, ToGraphDBProperties(properties));
    return CacheEdge(edge);
  }

  void SetNodeProperty(std::int64_t node_id, std::string property_key,
                       Value value) override {
    EnsureActive();
    graphdb::Vertex vertex =
        transaction_->GetVertexById(boost::endian::native_to_big(node_id));
    if (value.IsNull()) {
      vertex.RemoveProperty(property_key);
    } else {
      vertex.SetProperties({{std::move(property_key), std::move(value)}});
    }
  }

  void SetRelationshipProperty(std::int64_t relationship_id,
                               std::string property_key, Value value) override {
    EnsureActive();
    graphdb::Edge edge = ResolveEdge(relationship_id);
    if (value.IsNull()) {
      edge.RemoveProperty(property_key);
    } else {
      edge.SetProperties({{std::move(property_key), std::move(value)}});
    }
    relationship_cache_.erase(relationship_id);
  }

  void SetNodeProperties(std::int64_t node_id, Value::Map properties,
                         bool include_existing) override {
    EnsureActive();
    graphdb::Vertex vertex =
        transaction_->GetVertexById(boost::endian::native_to_big(node_id));
    if (!include_existing) {
      vertex.RemoveAllProperty();
    }
    for (const auto &[key, value] : properties) {
      if (value.IsNull()) {
        vertex.RemoveProperty(key);
      }
    }
    vertex.SetProperties(ToGraphDBProperties(properties));
  }

  void SetRelationshipProperties(std::int64_t relationship_id,
                                 Value::Map properties,
                                 bool include_existing) override {
    EnsureActive();
    graphdb::Edge edge = ResolveEdge(relationship_id);
    if (!include_existing) {
      edge.RemoveAllProperty();
    }
    for (const auto &[key, value] : properties) {
      if (value.IsNull()) {
        edge.RemoveProperty(key);
      }
    }
    edge.SetProperties(ToGraphDBProperties(properties));
    relationship_cache_.erase(relationship_id);
  }

  void SetLabels(std::int64_t node_id,
                 std::vector<std::string> labels) override {
    EnsureActive();
    transaction_->GetVertexById(boost::endian::native_to_big(node_id))
        .AddLabels(ToStringSet(labels));
  }

  void RemoveNodeProperty(std::int64_t node_id,
                          std::string_view property_key) override {
    EnsureActive();
    transaction_->GetVertexById(boost::endian::native_to_big(node_id))
        .RemoveProperty(std::string(property_key));
  }

  void RemoveRelationshipProperty(std::int64_t relationship_id,
                                  std::string_view property_key) override {
    EnsureActive();
    ResolveEdge(relationship_id).RemoveProperty(std::string(property_key));
    relationship_cache_.erase(relationship_id);
  }

  void RemoveLabels(std::int64_t node_id,
                    const std::vector<std::string> &labels) override {
    EnsureActive();
    transaction_->GetVertexById(boost::endian::native_to_big(node_id))
        .DeleteLabels(ToStringSet(labels));
  }

  void DeleteNode(std::int64_t node_id) override {
    EnsureActive();
    auto relationships = RelationshipIdsConnectedTo(node_id);
    CHECK(!relationships->Next(), common::InvalidArgumentError,
          "node still has relationships");
    relationships->Close();
    transaction_->GetVertexById(boost::endian::native_to_big(node_id)).Delete();
  }

  void DeleteRelationship(std::int64_t relationship_id) override {
    EnsureActive();
    ResolveEdge(relationship_id).Delete();
    relationship_cache_.erase(relationship_id);
    relationship_type_ids_.erase(relationship_id);
  }

 private:
  using RelationshipPredicate =
      std::function<bool(graphdb::Edge &relationship)>;

  void EnsureActive() const {
    CHECK(state_ == State::kActive, common::InvalidArgumentError,
          "transaction is no longer active");
  }

  [[nodiscard]] NodePtr SnapshotVertex(graphdb::Vertex vertex) const {
    auto node = std::make_shared<Node>();
    node->id = vertex.GetNativeId();
    node->labels = SortedLabels(vertex.GetLabels());
    auto properties = vertex.GetAllProperty();
    for (const auto &field :
         transaction_->db()->meta_info().GetVertexVectorFields(
             vertex.GetLabelIds())) {
      const Value value = vertex.GetProperty(
          boost::endian::native_to_big(field->property_id()));
      if (!value.IsNull()) {
        properties.insert_or_assign(field->property(), value);
      }
    }
    node->properties = ToQueryProperties(properties);
    return node;
  }

  [[nodiscard]] RelationshipPtr SnapshotEdge(graphdb::Edge edge) const {
    auto relationship = std::make_shared<Relationship>();
    relationship->id = edge.GetNativeId();
    relationship->start_node_id = edge.GetNativeStartId();
    relationship->end_node_id = edge.GetNativeEndId();
    relationship->type = edge.GetType();
    relationship->properties = ToQueryProperties(edge.GetAllProperty());
    return relationship;
  }

  [[nodiscard]] RelationshipPtr CacheEdge(graphdb::Edge edge) const {
    const std::int64_t id = edge.GetNativeId();
    relationship_type_ids_.insert_or_assign(id, edge.GetTypeId());
    auto snapshot = SnapshotEdge(edge);
    relationship_cache_.insert_or_assign(id, snapshot);
    return snapshot;
  }

  [[nodiscard]] graphdb::Edge ResolveEdge(std::int64_t id) const {
    EnsureActive();
    const auto type = relationship_type_ids_.find(id);
    if (type != relationship_type_ids_.end()) {
      return transaction_->GetEdgeById(type->second,
                                       boost::endian::native_to_big(id));
    }

    auto vertices = transaction_->NewVertexIterator();
    while (vertices->Valid()) {
      auto edges = vertices->GetVertex().NewEdgeIterator(
          graphdb::EdgeDirection::OUTGOING, {}, {});
      while (edges->Valid()) {
        graphdb::Edge edge = edges->GetEdge();
        if (edge.GetNativeId() == id) {
          relationship_type_ids_.emplace(id, edge.GetTypeId());
          return edge;
        }
        edges->Next();
      }
      vertices->Next();
    }
    THROW(common::NotFoundError, "relationship does not exist");
  }

  [[nodiscard]] std::shared_ptr<graphdb::VertexPropertyIndex>
  FindReadyNodeIndex(const std::vector<std::string> &labels,
                     std::string_view property_key) const {
    auto property_id =
        transaction_->db()->id_generator().GetPid(std::string(property_key));
    if (!property_id.has_value()) {
      return nullptr;
    }
    for (const auto &label : labels) {
      auto label_id = transaction_->db()->id_generator().GetLid(label);
      if (!label_id.has_value()) {
        continue;
      }
      auto index = transaction_->db()->meta_info().GetReadyVertexPropertyIndex(
          *label_id, *property_id);
      if (index != nullptr && index->PropertyCount() == 1) {
        return index;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> CollectNodeIds(
      std::unique_ptr<graphdb::VertexIterator> iterator,
      const std::vector<std::string> &labels) const {
    EnsureActive();
    std::vector<std::int64_t> ids;
    while (iterator->Valid()) {
      graphdb::Vertex &vertex = iterator->GetVertex();
      if (labels.empty() || ContainsAllLabels(vertex.GetLabels(), labels)) {
        ids.push_back(vertex.GetNativeId());
      }
      iterator->Next();
    }
    return MakeCursor(std::move(ids));
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> CollectRelationshipIds(
      std::optional<std::int64_t> node_id, graphdb::EdgeDirection direction,
      const std::vector<std::string> &types) const {
    return FilterRelationships(node_id, direction, types, {});
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> FilterRelationships(
      const std::vector<std::string> &types,
      RelationshipPredicate predicate) const {
    return FilterRelationships(std::nullopt, graphdb::EdgeDirection::OUTGOING,
                               types, std::move(predicate));
  }

  [[nodiscard]] std::unique_ptr<EntityIdCursor> FilterRelationships(
      std::optional<std::int64_t> node_id, graphdb::EdgeDirection direction,
      const std::vector<std::string> &types,
      RelationshipPredicate predicate) const {
    EnsureActive();
    std::vector<std::int64_t> ids;
    std::unordered_set<std::int64_t> seen;
    const auto type_set = ToStringSet(types);

    auto append_edges = [&](graphdb::Vertex &vertex) {
      auto edges = vertex.NewEdgeIterator(direction, type_set, {});
      while (edges->Valid()) {
        graphdb::Edge edge = edges->GetEdge();
        const std::int64_t id = edge.GetNativeId();
        if (seen.insert(id).second && (!predicate || predicate(edge))) {
          (void)CacheEdge(edge);
          ids.push_back(id);
        }
        edges->Next();
      }
    };

    if (node_id.has_value()) {
      graphdb::Vertex vertex =
          transaction_->GetVertexById(boost::endian::native_to_big(*node_id));
      append_edges(vertex);
    } else {
      auto vertices = transaction_->NewVertexIterator();
      while (vertices->Valid()) {
        append_edges(vertices->GetVertex());
        vertices->Next();
      }
    }
    return MakeCursor(std::move(ids));
  }

  std::unique_ptr<txn::Transaction> transaction_;
  mutable std::unordered_map<std::int64_t, RelationshipPtr> relationship_cache_;
  mutable std::unordered_map<std::int64_t, std::uint32_t>
      relationship_type_ids_;
  State state_ = State::kActive;
};

std::unique_ptr<GraphDBStorage> GraphDBStorage::Open(const std::string &path) {
  graphdb::GraphDBOptions options;
  options.assistant_pool = std::make_shared<graphdb::AssistantPool>(1);
  return Open(path, options);
}

std::unique_ptr<GraphDBStorage> GraphDBStorage::Open(
    const std::string &path, const graphdb::GraphDBOptions &options) {
  return std::make_unique<GraphDBStorage>(
      graphdb::GraphDB::Open(path, options));
}

GraphDBStorage::GraphDBStorage(std::unique_ptr<graphdb::GraphDB> graph_db)
    : graph_db_(std::move(graph_db)) {
  CHECK(graph_db_ != nullptr, common::InvalidArgumentError,
        "graphdb instance is null");
}

GraphDBStorage::~GraphDBStorage() = default;

std::unique_ptr<StorageTransaction> GraphDBStorage::BeginTransaction() {
  return std::make_unique<Transaction>(graph_db_->BeginTransaction());
}

std::unique_ptr<StorageTransaction> GraphDBStorage::BeginReadTransaction()
    const {
  return std::make_unique<Transaction>(graph_db_->BeginTransaction());
}

#define GRAPHDB_READ_CURSOR(method, ...)                             \
  auto transaction = BeginReadTransaction();                         \
  auto ids = DrainCursor(transaction->Reader().method(__VA_ARGS__)); \
  transaction->Commit();                                             \
  return MakeCursor(std::move(ids))

std::unique_ptr<EntityIdCursor> GraphDBStorage::ScanNodeIds() const {
  GRAPHDB_READ_CURSOR(ScanNodeIds);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::ScanRelationshipIds() const {
  GRAPHDB_READ_CURSOR(ScanRelationshipIds);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::ScanNodeIdsByLabels(
    const std::vector<std::string> &labels) const {
  GRAPHDB_READ_CURSOR(ScanNodeIdsByLabels, labels);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::ScanRelationshipIdsByTypes(
    const std::vector<std::string> &types) const {
  GRAPHDB_READ_CURSOR(ScanRelationshipIdsByTypes, types);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::RelationshipIdsConnectedTo(
    std::int64_t node_id) const {
  GRAPHDB_READ_CURSOR(RelationshipIdsConnectedTo, node_id);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::OutgoingRelationshipIds(
    std::int64_t node_id) const {
  GRAPHDB_READ_CURSOR(OutgoingRelationshipIds, node_id);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::IncomingRelationshipIds(
    std::int64_t node_id) const {
  GRAPHDB_READ_CURSOR(IncomingRelationshipIds, node_id);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::FindNodeIdsByIndex(
    const std::vector<std::string> &labels, std::string_view property_key,
    const Value &value) const {
  GRAPHDB_READ_CURSOR(FindNodeIdsByIndex, labels, property_key, value);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::FindNodeIdsByIndexRange(
    const std::vector<std::string> &labels, std::string_view property_key,
    const IndexRange &range) const {
  GRAPHDB_READ_CURSOR(FindNodeIdsByIndexRange, labels, property_key, range);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::FindRelationshipIdsByIndex(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, const Value &value) const {
  GRAPHDB_READ_CURSOR(FindRelationshipIdsByIndex, relationship_types,
                      property_key, value);
}

std::unique_ptr<EntityIdCursor> GraphDBStorage::FindRelationshipIdsByIndexRange(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, const IndexRange &range) const {
  GRAPHDB_READ_CURSOR(FindRelationshipIdsByIndexRange, relationship_types,
                      property_key, range);
}

#undef GRAPHDB_READ_CURSOR

std::size_t GraphDBStorage::RelationshipCount() const {
  auto cursor = ScanRelationshipIds();
  std::size_t count = 0;
  while (cursor->Next()) {
    ++count;
  }
  return count;
}

GraphDBStorage::NodePtr GraphDBStorage::NodeById(std::int64_t id) const {
  auto transaction = BeginReadTransaction();
  NodePtr node = transaction->Reader().NodeById(id);
  transaction->Commit();
  return node;
}

GraphDBStorage::RelationshipPtr GraphDBStorage::RelationshipById(
    std::int64_t id) const {
  auto transaction = BeginReadTransaction();
  RelationshipPtr relationship = transaction->Reader().RelationshipById(id);
  transaction->Commit();
  return relationship;
}

Value GraphDBStorage::NodeProperty(std::int64_t node_id,
                                   std::string_view property_key) const {
  auto transaction = BeginReadTransaction();
  Value value = transaction->Reader().NodeProperty(node_id, property_key);
  transaction->Commit();
  return value;
}

Value GraphDBStorage::RelationshipProperty(
    std::int64_t relationship_id, std::string_view property_key) const {
  auto transaction = BeginReadTransaction();
  Value value =
      transaction->Reader().RelationshipProperty(relationship_id, property_key);
  transaction->Commit();
  return value;
}

GraphDBStorage::NodePtr GraphDBStorage::CreateNode(
    std::vector<std::string> labels, Value::Map properties) {
  auto transaction = BeginTransaction();
  NodePtr node = transaction->Writer()->CreateNode(std::move(labels),
                                                   std::move(properties));
  transaction->Commit();
  return node;
}

GraphDBStorage::RelationshipPtr GraphDBStorage::CreateRelationship(
    std::int64_t start_node_id, std::int64_t end_node_id, std::string type,
    Value::Map properties) {
  auto transaction = BeginTransaction();
  RelationshipPtr relationship = transaction->Writer()->CreateRelationship(
      start_node_id, end_node_id, std::move(type), std::move(properties));
  transaction->Commit();
  return relationship;
}

#define GRAPHDB_WRITE(method, ...)            \
  auto transaction = BeginTransaction();      \
  transaction->Writer()->method(__VA_ARGS__); \
  transaction->Commit()

void GraphDBStorage::SetNodeProperty(std::int64_t node_id,
                                     std::string property_key, Value value) {
  GRAPHDB_WRITE(SetNodeProperty, node_id, std::move(property_key),
                std::move(value));
}

void GraphDBStorage::SetRelationshipProperty(std::int64_t relationship_id,
                                             std::string property_key,
                                             Value value) {
  GRAPHDB_WRITE(SetRelationshipProperty, relationship_id,
                std::move(property_key), std::move(value));
}

void GraphDBStorage::SetNodeProperties(std::int64_t node_id,
                                       Value::Map properties,
                                       bool include_existing) {
  GRAPHDB_WRITE(SetNodeProperties, node_id, std::move(properties),
                include_existing);
}

void GraphDBStorage::SetRelationshipProperties(std::int64_t relationship_id,
                                               Value::Map properties,
                                               bool include_existing) {
  GRAPHDB_WRITE(SetRelationshipProperties, relationship_id,
                std::move(properties), include_existing);
}

void GraphDBStorage::SetLabels(std::int64_t node_id,
                               std::vector<std::string> labels) {
  GRAPHDB_WRITE(SetLabels, node_id, std::move(labels));
}

void GraphDBStorage::RemoveNodeProperty(std::int64_t node_id,
                                        std::string_view property_key) {
  GRAPHDB_WRITE(RemoveNodeProperty, node_id, property_key);
}

void GraphDBStorage::RemoveRelationshipProperty(std::int64_t relationship_id,
                                                std::string_view property_key) {
  GRAPHDB_WRITE(RemoveRelationshipProperty, relationship_id, property_key);
}

void GraphDBStorage::RemoveLabels(std::int64_t node_id,
                                  const std::vector<std::string> &labels) {
  GRAPHDB_WRITE(RemoveLabels, node_id, labels);
}

void GraphDBStorage::DeleteNode(std::int64_t node_id) {
  GRAPHDB_WRITE(DeleteNode, node_id);
}

void GraphDBStorage::DeleteRelationship(std::int64_t relationship_id) {
  GRAPHDB_WRITE(DeleteRelationship, relationship_id);
}

#undef GRAPHDB_WRITE

std::optional<ir::NodeIndexDescriptor> GraphDBStorage::FindNodeIndex(
    const std::vector<std::string> &labels,
    std::string_view property_key) const {
  auto property_id =
      graph_db_->id_generator().GetPid(std::string(property_key));
  if (!property_id.has_value()) {
    return std::nullopt;
  }
  for (const auto &label : labels) {
    auto label_id = graph_db_->id_generator().GetLid(label);
    if (!label_id.has_value()) {
      continue;
    }
    auto index = graph_db_->meta_info().GetReadyVertexPropertyIndex(
        *label_id, *property_id);
    if (index != nullptr && index->PropertyCount() == 1) {
      return ir::NodeIndexDescriptor{.property_key = std::string(property_key),
                                     .unique = index->is_unique()};
    }
  }
  return std::nullopt;
}

std::optional<ir::RelationshipIndexDescriptor>
GraphDBStorage::FindRelationshipIndex(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  (void)relationship_types;
  (void)property_key;
  return std::nullopt;
}

double GraphDBStorage::EstimateNodeCount(
    const std::unordered_set<std::string> &labels) const {
  auto cursor = ScanNodeIdsByLabels({labels.begin(), labels.end()});
  double count = 0.0;
  while (cursor->Next()) {
    count += 1.0;
  }
  return count;
}

double GraphDBStorage::EstimateRelationshipCount(
    const std::vector<std::string> &relationship_types) const {
  auto cursor = ScanRelationshipIdsByTypes(relationship_types);
  double count = 0.0;
  while (cursor->Next()) {
    count += 1.0;
  }
  return count;
}

double GraphDBStorage::EstimateExpandFanout(
    const std::vector<std::string> &relationship_types) const {
  const double nodes = EstimateNodeCount({});
  return nodes == 0.0 ? 0.0
                      : EstimateRelationshipCount(relationship_types) / nodes;
}

double GraphDBStorage::EstimateExpandIntoSelectivity(
    const std::vector<std::string> &relationship_types) const {
  const double nodes = EstimateNodeCount({});
  if (nodes == 0.0) {
    return 0.0;
  }
  return std::clamp(
      EstimateRelationshipCount(relationship_types) / (nodes * nodes), 0.0,
      1.0);
}

double GraphDBStorage::EstimateFilterSelectivity() const { return 0.1; }

double GraphDBStorage::EstimateNodeHashJoinSelectivity(
    std::size_t key_count) const {
  return key_count == 0 ? 1.0 : 1.0 / static_cast<double>(key_count);
}

}  // namespace rg
