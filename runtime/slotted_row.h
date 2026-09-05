#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast/semantic_table.h"
#include "storage/graph_reader.h"
#include "value/value.h"

namespace rg {

enum class SlotKind {
  kNode,
  kRelationship,
  kReference,
};

struct Slot {
  std::size_t offset = 0;
  SlotKind kind = SlotKind::kReference;
  ast::SemanticVariableType type = ast::SemanticVariableType::kUnknown;
  bool nullable = true;
};

struct SlotDefinition {
  std::string name;
  ast::SemanticVariableType type = ast::SemanticVariableType::kUnknown;
  bool nullable = true;
  std::optional<SlotKind> storage_kind;
};

class SlotConfiguration final {
 public:
  explicit SlotConfiguration(std::vector<SlotDefinition> definitions = {});

  [[nodiscard]] const Slot *Find(std::string_view name) const;
  [[nodiscard]] const Slot &At(std::string_view name) const;
  [[nodiscard]] bool Contains(std::string_view name) const;
  [[nodiscard]] const std::vector<std::string> &Columns() const noexcept {
    return columns_;
  }
  [[nodiscard]] std::size_t EntitySlotCount() const noexcept {
    return entity_slot_count_;
  }
  [[nodiscard]] std::size_t ReferenceSlotCount() const noexcept {
    return reference_slot_count_;
  }

 private:
  std::vector<std::string> columns_;
  std::unordered_map<std::string, Slot> slots_;
  std::size_t entity_slot_count_ = 0;
  std::size_t reference_slot_count_ = 0;
};

using SlotConfigurationPtr = std::shared_ptr<const SlotConfiguration>;

struct SlotMapping {
  std::string source_name;
  std::string target_name;
  Slot source;
  Slot target;
};

class SlottedRow final {
 public:
  explicit SlottedRow(SlotConfigurationPtr slots);

  [[nodiscard]] const SlotConfigurationPtr &Slots() const noexcept {
    return slots_;
  }
  [[nodiscard]] bool IsInitialized(const Slot &slot) const;
  [[nodiscard]] bool IsInitialized(std::string_view name) const;
  [[nodiscard]] std::int64_t EntityIdAt(const Slot &slot) const;
  [[nodiscard]] const Value &ReferenceAt(const Slot &slot) const;

  void SetEntityId(const Slot &slot, std::int64_t id);
  void SetReference(const Slot &slot, Value value);
  void Set(std::string_view name, Value value);
  void SetNull(std::string_view name);

  [[nodiscard]] Value Get(std::string_view name,
                          const GraphReader &graph_reader) const;
  [[nodiscard]] Value Get(const Slot &slot,
                          const GraphReader &graph_reader) const;
  [[nodiscard]] std::size_t EstimatedHeapUsage() const;
  [[nodiscard]] SlottedRow CopyTo(SlotConfigurationPtr target,
                                  const GraphReader &graph_reader) const;

 private:
  SlotConfigurationPtr slots_;
  std::vector<std::int64_t> entity_ids_;
  std::vector<Value> references_;
  std::vector<bool> entity_initialized_;
  std::vector<bool> reference_initialized_;
};

[[nodiscard]] std::vector<SlotMapping> ComputeSlotMappings(
    const SlotConfiguration &source, const SlotConfiguration &target);
void CopySlots(const SlottedRow &source, SlottedRow *target,
               const std::vector<SlotMapping> &mappings,
               const GraphReader &graph_reader);
[[nodiscard]] bool TryBindSlot(SlottedRow *row, std::string_view name,
                               Value value, const GraphReader &graph_reader);
[[nodiscard]] bool TryBindEntityId(SlottedRow *row, std::string_view name,
                                   SlotKind kind, std::int64_t id,
                                   const GraphReader &graph_reader);
[[nodiscard]] std::size_t EstimatedValueHeapUsage(const Value &value);

}  // namespace rg
