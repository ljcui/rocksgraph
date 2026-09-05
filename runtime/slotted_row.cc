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

void SlottedRow::Set(std::string_view name, Value value) {
  const Slot &slot = slots_->At(name);
  switch (slot.kind) {
    case SlotKind::kNode:
      CHECK(value.IsNull() || value.IsNode(), common::InvalidArgumentError,
            "node slot received a non-node value: " + std::string(name));
      SetEntityId(slot, value.IsNull() ? -1 : value.AsNode().id);
      return;
    case SlotKind::kRelationship:
      CHECK(value.IsNull() || value.IsRelationship(),
            common::InvalidArgumentError,
            "relationship slot received a non-relationship value: " +
                std::string(name));
      SetEntityId(slot, value.IsNull() ? -1 : value.AsRelationship().id);
      return;
    case SlotKind::kReference:
      SetReference(slot, std::move(value));
      return;
  }
}

void SlottedRow::SetNull(std::string_view name) {
  const Slot &slot = slots_->At(name);
  if (IsEntitySlot(slot)) {
    SetEntityId(slot, -1);
  } else {
    SetReference(slot, Value::Null());
  }
}

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

QueryRow SlottedRow::Materialize(const GraphReader &graph_reader) const {
  QueryRow row;
  for (const auto &column : slots_->Columns()) {
    if (IsInitialized(column)) {
      row.emplace(column, Get(column, graph_reader));
    }
  }
  return row;
}

std::size_t SlottedRow::EstimatedHeapUsage() const {
  std::size_t bytes = sizeof(SlottedRow) +
                      entity_ids_.capacity() * sizeof(std::int64_t) +
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
                              const GraphReader &graph_reader) const {
  SlottedRow out(std::move(target));
  CopySlots(*this, &out, ComputeSlotMappings(*slots_, *out.Slots()),
            graph_reader);
  return out;
}

SlottedRow SlottedRow::FromQueryRow(SlotConfigurationPtr slots,
                                    const QueryRow &row) {
  SlottedRow out(std::move(slots));
  for (const auto &[name, value] : row) {
    if (out.Slots()->Contains(name)) {
      out.Set(name, value);
    }
  }
  return out;
}

std::vector<SlotMapping> ComputeSlotMappings(const SlotConfiguration &source,
                                             const SlotConfiguration &target) {
  std::vector<SlotMapping> mappings;
  for (const auto &column : target.Columns()) {
    const Slot *source_slot = source.Find(column);
    if (source_slot != nullptr) {
      mappings.push_back({.source_name = column,
                          .target_name = column,
                          .source = *source_slot,
                          .target = target.At(column)});
    }
  }
  return mappings;
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
      } else {
        target->SetEntityId(mapping.target, source.EntityIdAt(mapping.source));
      }
      continue;
    }

    target->Set(mapping.target_name,
                source.Get(mapping.source_name, graph_reader));
  }
}

bool TryBindSlot(SlottedRow *row, std::string_view name, Value value,
                 const GraphReader &graph_reader) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (name.empty()) {
    return true;
  }
  if (!row->IsInitialized(name)) {
    row->Set(name, std::move(value));
    return true;
  }
  return ValuesEqual(row->Get(name, graph_reader), value);
}

bool TryBindEntityId(SlottedRow *row, std::string_view name, SlotKind kind,
                     std::int64_t id, const GraphReader &graph_reader) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  CHECK(kind == SlotKind::kNode || kind == SlotKind::kRelationship,
        common::InvalidArgumentError, "entity binding kind is invalid");
  if (name.empty()) {
    return true;
  }
  const Slot &slot = row->Slots()->At(name);
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
  return TryBindSlot(row, name, std::move(value), graph_reader);
}

}  // namespace rg
