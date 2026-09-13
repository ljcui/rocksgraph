#include "runtime/slotted_row.h"

#include <utility>

#include "common/exception.h"

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
        .type = definition.type,
        .nullable = definition.nullable};
    if (slot.kind == SlotKind::kReference) {
      slot.offset = reference_slot_count_++;
    } else {
      slot.offset = entity_slot_count_++;
    }
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
  entity_ids_.resize(slots_->EntitySlotCount(), -1);
  relationship_type_ids_.resize(slots_->EntitySlotCount(), 0);
  references_.resize(slots_->ReferenceSlotCount(), Value::Null());
  entity_initialized_.resize(slots_->EntitySlotCount(), false);
  reference_initialized_.resize(slots_->ReferenceSlotCount(), false);
}

bool SlottedRow::IsInitialized(const Slot &slot) const {
  if (IsEntitySlot(slot)) {
    CHECK(slot.offset < entity_initialized_.size(), common::InternalError,
          "entity slot offset is out of range");
    return entity_initialized_[slot.offset];
  }
  CHECK(slot.offset < reference_initialized_.size(), common::InternalError,
        "reference slot offset is out of range");
  return reference_initialized_[slot.offset];
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
  return entity_ids_[slot.offset];
}

RelationshipReference SlottedRow::RelationshipAt(const Slot &slot) const {
  CHECK(slot.kind == SlotKind::kRelationship, common::InvalidArgumentError,
        "slot does not contain a relationship");
  return {.id = EntityIdAt(slot),
          .type_id = relationship_type_ids_[slot.offset]};
}

const Value &SlottedRow::ReferenceAt(const Slot &slot) const {
  CHECK(slot.kind == SlotKind::kReference, common::InvalidArgumentError,
        "slot is not a reference slot");
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "reference slot is not initialized");
  return references_[slot.offset];
}

void SlottedRow::SetEntityId(const Slot &slot, std::int64_t id) {
  CHECK(IsEntitySlot(slot), common::InvalidArgumentError,
        "slot does not contain an entity id");
  CHECK(id >= 0 || slot.nullable, common::InvalidArgumentError,
        "null assigned to non-nullable entity slot");
  entity_ids_[slot.offset] = id;
  if (slot.kind == SlotKind::kRelationship) {
    relationship_type_ids_[slot.offset] = 0;
  }
  entity_initialized_[slot.offset] = true;
}

void SlottedRow::SetRelationship(const Slot &slot,
                                 RelationshipReference relationship) {
  CHECK(slot.kind == SlotKind::kRelationship, common::InvalidArgumentError,
        "slot does not contain a relationship");
  CHECK(relationship.id >= 0 || slot.nullable, common::InvalidArgumentError,
        "null assigned to non-nullable relationship slot");
  entity_ids_[slot.offset] = relationship.id;
  relationship_type_ids_[slot.offset] = relationship.type_id;
  entity_initialized_[slot.offset] = true;
}

void SlottedRow::SetReference(const Slot &slot, Value value) {
  CHECK(slot.kind == SlotKind::kReference, common::InvalidArgumentError,
        "slot is not a reference slot");
  CHECK(!value.IsNull() || slot.nullable, common::InvalidArgumentError,
        "null assigned to non-nullable reference slot");
  references_[slot.offset] = std::move(value);
  reference_initialized_[slot.offset] = true;
}

void SlottedRow::Set(const Slot &slot, Value value) {
  switch (slot.kind) {
    case SlotKind::kNode:
      CHECK(value.IsNull() || value.IsNode(), common::InvalidArgumentError,
            "node slot received a non-node value");
      SetEntityId(slot, value.IsNull() ? -1 : value.AsNode().id);
      return;
    case SlotKind::kRelationship:
      CHECK(value.IsNull() || value.IsRelationship(),
            common::InvalidArgumentError,
            "relationship slot received a non-relationship value");
      SetRelationship(slot,
                      value.IsNull()
                          ? RelationshipReference{}
                          : RelationshipReference{
                                .id = value.AsRelationship().id,
                                .type_id = value.AsRelationship().type_id});
      return;
    case SlotKind::kReference:
      SetReference(slot, std::move(value));
      return;
  }
}

void SlottedRow::Set(std::string_view name, Value value) {
  Set(slots_->At(name), std::move(value));
}

void SlottedRow::SetNull(const Slot &slot) {
  if (IsEntitySlot(slot)) {
    SetEntityId(slot, -1);
  } else {
    SetReference(slot, Value::Null());
  }
}

void SlottedRow::SetNull(std::string_view name) { SetNull(slots_->At(name)); }

Value SlottedRow::Get(std::string_view name,
                      const GraphReader &graph_reader) const {
  const Slot &slot = slots_->At(name);
  return Get(slot, graph_reader);
}

Value SlottedRow::Get(const Slot &slot, const GraphReader &graph_reader) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "slot is not initialized");
  if (slot.kind == SlotKind::kReference) {
    return references_[slot.offset];
  }
  const std::int64_t id = entity_ids_[slot.offset];
  if (id < 0) {
    return Value::Null();
  }
  return slot.kind == SlotKind::kNode
             ? Value(graph_reader.NodeById(id))
             : Value(graph_reader.RelationshipById(id));
}

Value SlottedRow::Get(std::string_view name,
                      txn::Transaction &transaction) const {
  return Get(slots_->At(name), transaction);
}

Value SlottedRow::Get(const Slot &slot, txn::Transaction &transaction) const {
  CHECK(IsInitialized(slot), common::InvalidArgumentError,
        "slot is not initialized");
  if (slot.kind == SlotKind::kReference) {
    return references_[slot.offset];
  }
  const std::int64_t id = entity_ids_[slot.offset];
  if (id < 0) {
    return Value::Null();
  }
  return slot.kind == SlotKind::kNode
             ? Value(MaterializeGraphDBVertex(transaction, id))
             : Value(MaterializeGraphDBEdge(transaction, RelationshipAt(slot)));
}

std::size_t SlottedRow::EstimatedHeapUsage() const {
  std::size_t bytes =
      sizeof(SlottedRow) + entity_ids_.capacity() * sizeof(std::int64_t) +
      relationship_type_ids_.capacity() * sizeof(std::uint32_t) +
      references_.capacity() * sizeof(Value) +
      entity_initialized_.capacity() / 8 +
      reference_initialized_.capacity() / 8;
  for (std::size_t index = 0; index < references_.size(); ++index) {
    if (reference_initialized_[index]) {
      bytes += EstimatedValueHeapUsage(references_[index]);
    }
  }
  return bytes;
}

SlottedRow SlottedRow::CopyTo(SlotConfigurationPtr target,
                              const std::vector<SlotMapping> &mappings,
                              const GraphReader &graph_reader) const {
  SlottedRow out(std::move(target));
  CopySlots(*this, &out, mappings, graph_reader);
  return out;
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
               const GraphReader &graph_reader) {
  CHECK(target != nullptr, common::InternalError, "target row is null");
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source)) {
      continue;
    }
    if (mapping.source.kind == mapping.target.kind) {
      if (mapping.source.kind == SlotKind::kReference) {
        target->SetReference(mapping.target,
                             source.ReferenceAt(mapping.source));
      } else if (mapping.source.kind == SlotKind::kRelationship) {
        target->SetRelationship(mapping.target,
                                source.RelationshipAt(mapping.source));
      } else {
        target->SetEntityId(mapping.target, source.EntityIdAt(mapping.source));
      }
      continue;
    }

    target->Set(mapping.target, source.Get(mapping.source, graph_reader));
  }
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
      if (mapping.source.kind == SlotKind::kReference) {
        target->SetReference(mapping.target,
                             source.ReferenceAt(mapping.source));
      } else if (mapping.source.kind == SlotKind::kRelationship) {
        target->SetRelationship(mapping.target,
                                source.RelationshipAt(mapping.source));
      } else {
        target->SetEntityId(mapping.target, source.EntityIdAt(mapping.source));
      }
      continue;
    }
    target->Set(mapping.target, source.Get(mapping.source, transaction));
  }
}

bool TryBindSlot(SlottedRow *row, const Slot &slot, Value value,
                 const GraphReader &graph_reader) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(slot)) {
    row->Set(slot, std::move(value));
    return true;
  }
  return ValuesEqual(row->Get(slot, graph_reader), value);
}

bool TryBindSlot(SlottedRow *row, std::string_view name, Value value,
                 const GraphReader &graph_reader) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindSlot(row, row->Slots()->At(name), std::move(value),
                     graph_reader);
}

bool TryBindSlot(SlottedRow *row, const Slot &slot, Value value,
                 txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(slot)) {
    row->Set(slot, std::move(value));
    return true;
  }
  return ValuesEqual(row->Get(slot, transaction), value);
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

bool TryBindEntityId(SlottedRow *row, const Slot &slot, SlotKind kind,
                     std::int64_t id, const GraphReader &graph_reader) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  CHECK(kind == SlotKind::kNode || kind == SlotKind::kRelationship,
        common::InvalidArgumentError, "entity binding kind is invalid");
  if (slot.kind == kind) {
    if (!row->IsInitialized(slot)) {
      row->SetEntityId(slot, id);
      return true;
    }
    return row->EntityIdAt(slot) == id;
  }
  Value value = kind == SlotKind::kNode
                    ? Value(graph_reader.NodeById(id))
                    : Value(graph_reader.RelationshipById(id));
  return TryBindSlot(row, slot, std::move(value), graph_reader);
}

bool TryBindEntityId(SlottedRow *row, std::string_view name, SlotKind kind,
                     std::int64_t id, const GraphReader &graph_reader) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindEntityId(row, row->Slots()->At(name), kind, id, graph_reader);
}

bool TryBindEntityId(SlottedRow *row, const Slot &slot, SlotKind kind,
                     std::int64_t id, txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  CHECK(kind == SlotKind::kNode, common::InvalidArgumentError,
        "GraphDB relationship binding requires a type id");
  if (slot.kind == kind) {
    if (!row->IsInitialized(slot)) {
      row->SetEntityId(slot, id);
      return true;
    }
    return row->EntityIdAt(slot) == id;
  }
  return TryBindSlot(
      row, slot, Value(MaterializeGraphDBVertex(transaction, id)), transaction);
}

bool TryBindEntityId(SlottedRow *row, std::string_view name, SlotKind kind,
                     std::int64_t id, txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindEntityId(row, row->Slots()->At(name), kind, id, transaction);
}

bool TryBindRelationship(SlottedRow *row, const Slot &slot,
                         RelationshipReference relationship,
                         txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (slot.kind == SlotKind::kRelationship) {
    if (!row->IsInitialized(slot)) {
      row->SetRelationship(slot, relationship);
      return true;
    }
    return row->RelationshipAt(slot) == relationship;
  }
  return TryBindSlot(row, slot,
                     Value(MaterializeGraphDBEdge(transaction, relationship)),
                     transaction);
}

bool TryBindRelationship(SlottedRow *row, std::string_view name,
                         RelationshipReference relationship,
                         txn::Transaction &transaction) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  return TryBindRelationship(row, row->Slots()->At(name), relationship,
                             transaction);
}

}  // namespace rg
