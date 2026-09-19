#include "runtime/slotted_row.h"

#include <utility>

#include "common/exception.h"
#include "runtime/graphdb_access.h"

namespace rg {
std::size_t EstimatedValueHeapUsage(const Value &value) {
  if (value.IsString()) {
    return sizeof(Value) + value.AsString().capacity();
  }
  if (value.IsList()) {
    std::size_t bytes =
        sizeof(Value) + value.AsList().capacity() * sizeof(Value);
    for (const auto &item : value.AsList()) {
      bytes += EstimatedValueHeapUsage(item);
    }
    return bytes;
  }
  if (value.IsMap()) {
    std::size_t bytes = sizeof(Value);
    for (const auto &[key, item] : value.AsMap()) {
      bytes += key.capacity() + EstimatedValueHeapUsage(item);
    }
    return bytes;
  }
  return sizeof(Value);
}

SlotConfiguration::SlotConfiguration(std::vector<std::string> columns) {
  columns_.reserve(columns.size());
  for (auto &column : columns) {
    if (column.empty() || offsets_.contains(column)) {
      continue;
    }
    const std::size_t offset = columns_.size();
    columns_.push_back(column);
    offsets_.emplace(std::move(column), offset);
  }
}

std::optional<std::size_t> SlotConfiguration::Find(
    std::string_view name) const {
  const auto found = offsets_.find(name);
  if (found == offsets_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::size_t SlotConfiguration::At(std::string_view name) const {
  const std::optional<std::size_t> offset = Find(name);
  RG_CHECK(offset.has_value(), common::InvalidArgumentError,
           "slot is not allocated: " + std::string(name));
  return *offset;
}

bool SlotConfiguration::Contains(std::string_view name) const {
  return Find(name).has_value();
}

SlottedRow::SlottedRow(SlotConfigurationPtr slots) : slots_(std::move(slots)) {
  RG_CHECK(slots_ != nullptr, common::InvalidArgumentError,
           "slot configuration is null");
  values_.resize(slots_->SlotCount());
}

void SlottedRow::Reset() {
  for (auto &value : values_) {
    value = UninitializedSlot{};
  }
}

void SlottedRow::Reset(SlotConfigurationPtr slots) {
  RG_CHECK(slots != nullptr, common::InvalidArgumentError,
           "slot configuration is null");
  if (slots_ != slots) {
    slots_ = std::move(slots);
    values_.clear();
    values_.resize(slots_->SlotCount());
    return;
  }
  Reset();
}

bool SlottedRow::IsInitialized(std::size_t offset) const {
  RG_CHECK(offset < values_.size(), common::InternalError,
           "slot offset is out of range");
  return !std::holds_alternative<UninitializedSlot>(values_[offset]);
}

std::int64_t SlottedRow::EntityIdAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "entity slot is not initialized");
  const SlotValue &stored = values_[offset];
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    return vertex->GetId();
  }
  if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
    return edge->GetId();
  }
  const auto *value = std::get_if<Value>(&stored);
  RG_CHECK(value != nullptr, common::InternalError,
           "initialized slot contains no value");
  if (value->IsNull()) {
    return -1;
  }
  if (value->IsNode()) {
    return value->AsNode().id;
  }
  if (value->IsRelationship()) {
    return value->AsRelationship().id;
  }
  RG_THROW(common::InvalidArgumentError,
           "slot does not contain a graph entity");
}

const graphdb::Vertex &SlottedRow::VertexAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "node slot is not initialized");
  const auto *vertex = std::get_if<graphdb::Vertex>(&values_[offset]);
  RG_CHECK(vertex != nullptr, common::InvalidArgumentError,
           "node slot is null");
  return *vertex;
}

const graphdb::Edge &SlottedRow::EdgeAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "relationship slot is not initialized");
  const auto *edge = std::get_if<graphdb::Edge>(&values_[offset]);
  RG_CHECK(edge != nullptr, common::InvalidArgumentError,
           "relationship slot is null");
  return *edge;
}

const Value &SlottedRow::ValueAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "value slot is not initialized");
  const auto *value = std::get_if<Value>(&values_[offset]);
  RG_CHECK(value != nullptr, common::InternalError,
           "slot does not contain a Value");
  return *value;
}

bool SlottedRow::SlotEquals(std::size_t offset, const SlottedRow &other,
                            std::size_t other_offset) const {
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "left slot is not initialized");
  RG_CHECK(other.IsInitialized(other_offset), common::InvalidArgumentError,
           "right slot is not initialized");
  const SlotValue &left = values_[offset];
  const SlotValue &right = other.values_[other_offset];

  auto node_id = [](const SlotValue &stored) -> std::optional<std::int64_t> {
    if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
      return vertex->GetId();
    }
    const auto *value = std::get_if<Value>(&stored);
    if (value != nullptr && value->IsNode()) {
      return value->AsNode().id;
    }
    return std::nullopt;
  };
  if (std::holds_alternative<graphdb::Vertex>(left) ||
      std::holds_alternative<graphdb::Vertex>(right)) {
    const auto left_id = node_id(left);
    const auto right_id = node_id(right);
    return left_id.has_value() && right_id.has_value() && *left_id == *right_id;
  }

  auto relationship_id =
      [](const SlotValue &stored) -> std::optional<std::int64_t> {
    if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
      return edge->GetId();
    }
    const auto *value = std::get_if<Value>(&stored);
    if (value != nullptr && value->IsRelationship()) {
      return value->AsRelationship().id;
    }
    return std::nullopt;
  };
  if (std::holds_alternative<graphdb::Edge>(left) ||
      std::holds_alternative<graphdb::Edge>(right)) {
    const auto left_id = relationship_id(left);
    const auto right_id = relationship_id(right);
    return left_id.has_value() && right_id.has_value() && *left_id == *right_id;
  }

  const auto *left_value = std::get_if<Value>(&left);
  const auto *right_value = std::get_if<Value>(&right);
  RG_CHECK(left_value != nullptr && right_value != nullptr,
           common::InternalError,
           "initialized slots contain no comparable values");
  return ValuesEqual(*left_value, *right_value);
}

bool SlottedRow::ReadProperty(std::size_t offset, std::string_view property_key,
                              Value *value) const {
  RG_CHECK(value != nullptr, common::InternalError, "property output is null");
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "slot is not initialized");
  const SlotValue &stored = values_[offset];
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    auto copy = *vertex;
    *value = copy.GetProperty(std::string(property_key));
    return true;
  }
  if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
    auto copy = *edge;
    *value = copy.GetProperty(std::string(property_key));
    return true;
  }
  return false;
}

void SlottedRow::Set(std::size_t offset, graphdb::Vertex vertex) {
  SetSlotValue(offset, std::move(vertex));
}

void SlottedRow::Set(std::size_t offset, graphdb::Edge edge) {
  SetSlotValue(offset, std::move(edge));
}

void SlottedRow::Set(std::size_t offset, Value value) {
  SetSlotValue(offset, std::move(value));
}

void SlottedRow::SetSlotValue(std::size_t offset, SlotValue value) {
  RG_CHECK(offset < values_.size(), common::InternalError,
           "slot offset is out of range");
  values_[offset] = std::move(value);
}

void SlottedRow::SetNull(std::size_t offset) { Set(offset, Value::Null()); }

void SlottedRow::CopySlotFrom(const SlottedRow &source,
                              std::size_t source_offset,
                              std::size_t target_offset) {
  RG_CHECK(source.IsInitialized(source_offset), common::InvalidArgumentError,
           "source slot is not initialized");
  values_[target_offset] = source.values_[source_offset];
}

void SlottedRow::MaterializeGraphEntities() {
  for (SlotValue &stored : values_) {
    if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
      stored = Value(MaterializeGraphDBVertex(*vertex));
    } else if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
      stored = Value(MaterializeGraphDBEdge(*edge));
    }
  }
}

Value SlottedRow::Get(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::InvalidArgumentError,
           "slot is not initialized");
  const SlotValue &stored = values_[offset];
  if (const auto *value = std::get_if<Value>(&stored)) {
    return *value;
  }
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    return Value(MaterializeGraphDBVertex(*vertex));
  }
  const auto *edge = std::get_if<graphdb::Edge>(&stored);
  RG_CHECK(edge != nullptr, common::InternalError,
           "initialized slot contains no value");
  return Value(MaterializeGraphDBEdge(*edge));
}

std::size_t SlottedRow::EstimatedHeapUsage() const {
  std::size_t bytes =
      sizeof(SlottedRow) + values_.capacity() * sizeof(SlotValue);
  for (const auto &slot : values_) {
    if (const auto *value = std::get_if<Value>(&slot)) {
      bytes += EstimatedValueHeapUsage(*value);
    }
  }
  return bytes;
}

SlottedRow SlottedRow::CopyTo(SlotConfigurationPtr target,
                              const std::vector<SlotMapping> &mappings) const {
  SlottedRow out(std::move(target));
  CopySlots(*this, &out, mappings);
  return out;
}

void CopySlots(const SlottedRow &source, SlottedRow *target,
               const std::vector<SlotMapping> &mappings) {
  RG_CHECK(target != nullptr, common::InternalError, "target row is null");
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source_offset)) {
      continue;
    }
    target->CopySlotFrom(source, mapping.source_offset, mapping.target_offset);
  }
}

bool TryBindSlot(SlottedRow *row, std::size_t offset, Value value) {
  RG_CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(offset)) {
    row->Set(offset, std::move(value));
    return true;
  }
  return ValuesEqual(row->Get(offset), value);
}

bool TryBindEdge(SlottedRow *row, std::size_t offset, graphdb::Edge edge) {
  RG_CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(offset)) {
    row->Set(offset, std::move(edge));
    return true;
  }
  const Value existing = row->Get(offset);
  return existing.IsRelationship() &&
         existing.AsRelationship().id == edge.GetId() &&
         existing.AsRelationship().type_id == edge.GetTypeId();
}

}  // namespace rg
