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

namespace {

SlotKind KindFor(ast::SemanticVariableType type) {
  if (type == ast::SemanticVariableType::kNode) {
    return SlotKind::kNode;
  }
  if (type == ast::SemanticVariableType::kRelationship) {
    return SlotKind::kRelationship;
  }
  return SlotKind::kReference;
}

bool IsEntitySlot(const Slot &slot) {
  return slot.kind == SlotKind::kNode || slot.kind == SlotKind::kRelationship;
}

}  // namespace

SlotConfiguration::SlotConfiguration(std::vector<SlotDefinition> definitions) {
  columns_.reserve(definitions.size());
  for (auto &definition : definitions) {
    if (definition.name.empty() || slots_.contains(definition.name)) {
      continue;
    }
    Slot slot{
        .kind = definition.storage_kind.value_or(KindFor(definition.type)),
        .type = definition.type};
    slot.offset = slot_count_++;
    columns_.push_back(definition.name);
    slots_.emplace(std::move(definition.name), slot);
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
  CHECK(IsEntitySlot(slot), common::InvalidArgumentError,
        "slot does not contain an entity id");
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "entity slot is not initialized");
  if (const auto *value = std::get_if<Value>(&values_[slot.offset])) {
    CHECK(value->IsNull(), common::InternalError,
          "entity slot contains an invalid value");
    return -1;
  }
  return slot.kind == SlotKind::kNode ? VertexAt(slot).GetNativeId()
                                      : EdgeAt(slot).GetNativeId();
}

const graphdb::Vertex &SlottedRow::VertexAt(const Slot &slot) const {
  CHECK(slot.kind == SlotKind::kNode, common::InvalidArgumentError,
        "slot does not contain a vertex");
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "node slot is not initialized");
  const auto *vertex = std::get_if<graphdb::Vertex>(&values_[slot.offset]);
  CHECK(vertex != nullptr, common::InvalidArgumentError, "node slot is null");
  return *vertex;
}

const graphdb::Edge &SlottedRow::EdgeAt(const Slot &slot) const {
  CHECK(slot.kind == SlotKind::kRelationship, common::InvalidArgumentError,
        "slot does not contain an edge");
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "relationship slot is not initialized");
  const auto *edge = std::get_if<graphdb::Edge>(&values_[slot.offset]);
  CHECK(edge != nullptr, common::InvalidArgumentError,
        "relationship slot is null");
  return *edge;
}

const Value &SlottedRow::ReferenceAt(const Slot &slot) const {
  CHECK(slot.kind == SlotKind::kReference, common::InvalidArgumentError,
        "slot is not a reference slot");
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "reference slot is not initialized");
  const auto *value = std::get_if<Value>(&values_[slot.offset]);
  CHECK(value != nullptr, common::InternalError,
        "reference slot contains an invalid value");
  return *value;
}

void SlottedRow::SetVertex(const Slot &slot, graphdb::Vertex vertex) {
  CHECK(slot.kind == SlotKind::kNode, common::InvalidArgumentError,
        "slot is not a node slot");
  values_[slot.offset] = std::move(vertex);
}

void SlottedRow::SetEdge(const Slot &slot, graphdb::Edge edge) {
  CHECK(slot.kind == SlotKind::kRelationship, common::InvalidArgumentError,
        "slot is not a relationship slot");
  values_[slot.offset] = std::move(edge);
}

void SlottedRow::SetReference(const Slot &slot, Value value) {
  CHECK(slot.kind == SlotKind::kReference, common::InvalidArgumentError,
        "slot is not a reference slot");
  values_[slot.offset] = std::move(value);
}

void SlottedRow::Set(const Slot &slot, Value value,
                     txn::Transaction &transaction) {
  switch (slot.kind) {
    case SlotKind::kNode:
      CHECK(value.IsNull() || value.IsNode(), common::InvalidArgumentError,
            "node slot received a non-node value");
      if (value.IsNull()) {
        SetNull(slot);
      } else {
        SetVertex(slot, GraphDBVertexById(transaction, value.AsNode().id));
      }
      return;
    case SlotKind::kRelationship:
      CHECK(value.IsNull() || value.IsRelationship(),
            common::InvalidArgumentError,
            "relationship slot received a non-relationship value");
      if (value.IsNull()) {
        SetNull(slot);
      } else {
        SetEdge(slot,
                GraphDBEdgeById(transaction,
                                {.id = value.AsRelationship().id,
                                 .type_id = value.AsRelationship().type_id}));
      }
      return;
    case SlotKind::kReference:
      SetReference(slot, std::move(value));
      return;
  }
}

void SlottedRow::Set(std::string_view name, Value value,
                     txn::Transaction &transaction) {
  Set(slots_->At(name), std::move(value), transaction);
}

void SlottedRow::SetNull(const Slot &slot) {
  values_[slot.offset] = Value::Null();
}

void SlottedRow::SetNull(std::string_view name) { SetNull(slots_->At(name)); }

void SlottedRow::CopySlotFrom(const SlottedRow &source, const Slot &source_slot,
                              const Slot &target_slot) {
  CHECK(source_slot.kind == target_slot.kind, common::InvalidArgumentError,
        "slot copy requires matching slot kinds");
  CHECK(source.IsInitialized(source_slot), common::InvalidArgumentError,
        "source slot is not initialized");
  const SlotValue &value = source.values_[source_slot.offset];
  values_[target_slot.offset] = value;
}

Value SlottedRow::Get(std::string_view name) const {
  return Get(slots_->At(name));
}

Value SlottedRow::Get(const Slot &slot) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "slot is not initialized");
  if (slot.kind == SlotKind::kReference) {
    return ReferenceAt(slot);
  }
  if (std::holds_alternative<Value>(values_[slot.offset])) {
    return Value::Null();
  }
  return slot.kind == SlotKind::kNode
             ? Value(MaterializeGraphDBVertex(VertexAt(slot)))
             : Value(MaterializeGraphDBEdge(EdgeAt(slot)));
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
                              const std::vector<SlotMapping> &mappings,
                              txn::Transaction &transaction) const {
  SlottedRow out(std::move(target));
  CopySlots(*this, &out, mappings, transaction);
  return out;
}

void CopySlots(const SlottedRow &source, SlottedRow *target,
               const std::vector<SlotMapping> &mappings,
               txn::Transaction &transaction) {
  CHECK(target != nullptr, common::InternalError, "target row is null");
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source)) {
      continue;
    }
    if (mapping.source.kind == mapping.target.kind) {
      target->CopySlotFrom(source, mapping.source, mapping.target);
      continue;
    }
    target->Set(mapping.target, source.Get(mapping.source), transaction);
  }
}

bool TryBindSlot(SlottedRow *row, const Slot &slot, Value value,
                 txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(slot)) {
    row->Set(slot, std::move(value), transaction);
    return true;
  }
  return ValuesEqual(row->Get(slot), value);
}

bool TryBindSlot(SlottedRow *row, std::string_view name, Value value,
                 txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindSlot(row, row->Slots()->At(name), std::move(value),
                     transaction);
}

bool TryBindVertex(SlottedRow *row, const Slot &slot, graphdb::Vertex vertex,
                   txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (slot.kind == SlotKind::kNode) {
    if (!row->IsInitialized(slot)) {
      row->SetVertex(slot, std::move(vertex));
      return true;
    }
    return row->EntityIdAt(slot) == vertex.GetNativeId();
  }
  return TryBindSlot(row, slot,
                     Value(MaterializeGraphDBVertex(std::move(vertex))),
                     transaction);
}

bool TryBindVertex(SlottedRow *row, std::string_view name,
                   graphdb::Vertex vertex, txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindVertex(row, row->Slots()->At(name), std::move(vertex),
                       transaction);
}

bool TryBindEdge(SlottedRow *row, const Slot &slot, graphdb::Edge edge,
                 txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (slot.kind == SlotKind::kRelationship) {
    if (!row->IsInitialized(slot)) {
      row->SetEdge(slot, std::move(edge));
      return true;
    }
    if (row->EntityIdAt(slot) < 0) {
      return false;
    }
    const graphdb::Edge &bound = row->EdgeAt(slot);
    return bound.GetNativeId() == edge.GetNativeId() &&
           bound.GetTypeId() == edge.GetTypeId();
  }
  return TryBindSlot(row, slot, Value(MaterializeGraphDBEdge(std::move(edge))),
                     transaction);
}

bool TryBindEdge(SlottedRow *row, std::string_view name, graphdb::Edge edge,
                 txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindEdge(row, row->Slots()->At(name), std::move(edge), transaction);
}

}  // namespace rg
