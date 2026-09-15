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
    if (column.empty() || slots_.contains(column)) {
      continue;
    }
    Slot slot{.offset = slot_count_++};
    columns_.push_back(column);
    slots_.emplace(std::move(column), slot);
  }
}

const Slot *SlotConfiguration::Find(std::string_view name) const {
  const auto found = slots_.find(std::string(name));
  return found == slots_.end() ? nullptr : &found->second;
}

const Slot &SlotConfiguration::At(std::string_view name) const {
  const Slot *slot = Find(name);
  CHECK(slot != nullptr, common::InvalidArgumentError,
        "slot is not allocated: " + std::string(name));
  return *slot;
}

bool SlotConfiguration::Contains(std::string_view name) const {
  return Find(name) != nullptr;
}

SlottedRow::SlottedRow(SlotConfigurationPtr slots) : slots_(std::move(slots)) {
  CHECK(slots_ != nullptr, common::InvalidArgumentError,
        "slot configuration is null");
  values_.resize(slots_->SlotCount());
}

bool SlottedRow::IsInitialized(const Slot &slot) const {
  CHECK(slot.offset < values_.size(), common::InternalError,
        "slot offset is out of range");
  return !std::holds_alternative<UninitializedSlot>(values_[slot.offset]);
}

bool SlottedRow::IsInitialized(std::string_view name) const {
  const Slot *slot = slots_->Find(name);
  return slot != nullptr && IsInitialized(*slot);
}

std::int64_t SlottedRow::EntityIdAt(const Slot &slot) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "entity slot is not initialized");
  const SlotValue &stored = values_[slot.offset];
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    return vertex->GetNativeId();
  }
  if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
    return edge->GetNativeId();
  }
  const auto *value = std::get_if<Value>(&stored);
  CHECK(value != nullptr, common::InternalError,
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
  THROW(common::InvalidArgumentError, "slot does not contain a graph entity");
}

const graphdb::Vertex &SlottedRow::VertexAt(const Slot &slot) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "node slot is not initialized");
  const auto *vertex = std::get_if<graphdb::Vertex>(&values_[slot.offset]);
  CHECK(vertex != nullptr, common::InvalidArgumentError, "node slot is null");
  return *vertex;
}

const graphdb::Edge &SlottedRow::EdgeAt(const Slot &slot) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "relationship slot is not initialized");
  const auto *edge = std::get_if<graphdb::Edge>(&values_[slot.offset]);
  CHECK(edge != nullptr, common::InvalidArgumentError,
        "relationship slot is null");
  return *edge;
}

const Value &SlottedRow::ValueAt(const Slot &slot) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "value slot is not initialized");
  const auto *value = std::get_if<Value>(&values_[slot.offset]);
  CHECK(value != nullptr, common::InternalError,
        "slot does not contain a Value");
  return *value;
}

bool SlottedRow::ReadProperty(const Slot &slot, std::string_view property_key,
                              Value *value) const {
  CHECK(value != nullptr, common::InternalError, "property output is null");
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "slot is not initialized");
  const SlotValue &stored = values_[slot.offset];
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

void SlottedRow::SetVertex(const Slot &slot, graphdb::Vertex vertex) {
  values_[slot.offset] = std::move(vertex);
}

void SlottedRow::SetEdge(const Slot &slot, graphdb::Edge edge) {
  values_[slot.offset] = std::move(edge);
}

void SlottedRow::Set(const Slot &slot, Value value) {
  values_[slot.offset] = std::move(value);
}

void SlottedRow::Set(std::string_view name, Value value) {
  Set(slots_->At(name), std::move(value));
}

void SlottedRow::SetNull(const Slot &slot) {
  values_[slot.offset] = Value::Null();
}

void SlottedRow::SetNull(std::string_view name) { SetNull(slots_->At(name)); }

void SlottedRow::CopySlotFrom(const SlottedRow &source, const Slot &source_slot,
                              const Slot &target_slot) {
  CHECK(source.IsInitialized(source_slot), common::InvalidArgumentError,
        "source slot is not initialized");
  values_[target_slot.offset] = source.values_[source_slot.offset];
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

Value SlottedRow::Get(std::string_view name) const {
  return Get(slots_->At(name));
}

Value SlottedRow::Get(const Slot &slot) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "slot is not initialized");
  const SlotValue &stored = values_[slot.offset];
  if (const auto *value = std::get_if<Value>(&stored)) {
    return *value;
  }
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    return Value(MaterializeGraphDBVertex(*vertex));
  }
  const auto *edge = std::get_if<graphdb::Edge>(&stored);
  CHECK(edge != nullptr, common::InternalError,
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
  CHECK(target != nullptr, common::InternalError, "target row is null");
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source)) {
      continue;
    }
    target->CopySlotFrom(source, mapping.source, mapping.target);
  }
}

bool TryBindSlot(SlottedRow *row, const Slot &slot, Value value) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(slot)) {
    row->Set(slot, std::move(value));
    return true;
  }
  return ValuesEqual(row->Get(slot), value);
}

bool TryBindSlot(SlottedRow *row, std::string_view name, Value value) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindSlot(row, row->Slots()->At(name), std::move(value));
}

bool TryBindVertex(SlottedRow *row, const Slot &slot, graphdb::Vertex vertex) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(slot)) {
    row->SetVertex(slot, std::move(vertex));
    return true;
  }
  const Value existing = row->Get(slot);
  return existing.IsNode() && existing.AsNode().id == vertex.GetNativeId();
}

bool TryBindVertex(SlottedRow *row, std::string_view name,
                   graphdb::Vertex vertex) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindVertex(row, row->Slots()->At(name), std::move(vertex));
}

bool TryBindEdge(SlottedRow *row, const Slot &slot, graphdb::Edge edge) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(slot)) {
    row->SetEdge(slot, std::move(edge));
    return true;
  }
  const Value existing = row->Get(slot);
  return existing.IsRelationship() &&
         existing.AsRelationship().id == edge.GetNativeId() &&
         existing.AsRelationship().type_id == edge.GetTypeId();
}

bool TryBindEdge(SlottedRow *row, std::string_view name, graphdb::Edge edge) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindEdge(row, row->Slots()->At(name), std::move(edge));
}

}  // namespace rg
