#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast/semantic_table.h"
#include "graphdb/graph_entity.h"
#include "value/value.h"

namespace rg {

struct Slot {
  std::size_t offset = 0;
  ast::SemanticVariableType type = ast::SemanticVariableType::kUnknown;
};

struct SlotDefinition {
  std::string name;
  ast::SemanticVariableType type = ast::SemanticVariableType::kUnknown;
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
  [[nodiscard]] std::size_t SlotCount() const noexcept { return slot_count_; }

 private:
  std::vector<std::string> columns_;
  std::unordered_map<std::string, Slot> slots_;
  std::size_t slot_count_ = 0;
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
  [[nodiscard]] const graphdb::Vertex &VertexAt(const Slot &slot) const;
  [[nodiscard]] const graphdb::Edge &EdgeAt(const Slot &slot) const;
  [[nodiscard]] const Value &ValueAt(const Slot &slot) const;
  [[nodiscard]] bool ReadProperty(const Slot &slot,
                                  std::string_view property_key,
                                  Value *value) const;

  void SetVertex(const Slot &slot, graphdb::Vertex vertex);
  void SetEdge(const Slot &slot, graphdb::Edge edge);
  void Set(const Slot &slot, Value value);
  void Set(std::string_view name, Value value);
  void SetNull(const Slot &slot);
  void SetNull(std::string_view name);
  void CopySlotFrom(const SlottedRow &source, const Slot &source_slot,
                    const Slot &target_slot);
  void MaterializeGraphEntities();

  [[nodiscard]] Value Get(std::string_view name) const;
  [[nodiscard]] Value Get(const Slot &slot) const;
  [[nodiscard]] std::size_t EstimatedHeapUsage() const;
  [[nodiscard]] SlottedRow CopyTo(
      SlotConfigurationPtr target,
      const std::vector<SlotMapping> &mappings) const;

 private:
  struct UninitializedSlot {};
  using SlotValue =
      std::variant<UninitializedSlot, graphdb::Vertex, graphdb::Edge, Value>;

  SlotConfigurationPtr slots_;
  std::vector<SlotValue> values_;
};

void CopySlots(const SlottedRow &source, SlottedRow *target,
               const std::vector<SlotMapping> &mappings);
[[nodiscard]] bool TryBindSlot(SlottedRow *row, const Slot &slot, Value value);
[[nodiscard]] bool TryBindSlot(SlottedRow *row, std::string_view name,
                               Value value);
[[nodiscard]] bool TryBindVertex(SlottedRow *row, const Slot &slot,
                                 graphdb::Vertex vertex);
[[nodiscard]] bool TryBindVertex(SlottedRow *row, std::string_view name,
                                 graphdb::Vertex vertex);
[[nodiscard]] bool TryBindEdge(SlottedRow *row, const Slot &slot,
                               graphdb::Edge edge);
[[nodiscard]] bool TryBindEdge(SlottedRow *row, std::string_view name,
                               graphdb::Edge edge);
[[nodiscard]] std::size_t EstimatedValueHeapUsage(const Value &value);

}  // namespace rg
