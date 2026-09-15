#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "graphdb/graph_entity.h"
#include "value/value.h"

namespace rg {

class SlotConfiguration final {
 public:
  explicit SlotConfiguration(std::vector<std::string> columns = {});

  [[nodiscard]] std::optional<std::size_t> Find(std::string_view name) const;
  [[nodiscard]] std::size_t At(std::string_view name) const;
  [[nodiscard]] bool Contains(std::string_view name) const;
  [[nodiscard]] const std::vector<std::string> &Columns() const noexcept {
    return columns_;
  }
  [[nodiscard]] std::size_t SlotCount() const noexcept {
    return columns_.size();
  }

 private:
  std::vector<std::string> columns_;
  std::unordered_map<std::string, std::size_t> offsets_;
};

using SlotConfigurationPtr = std::shared_ptr<const SlotConfiguration>;

struct SlotMapping {
  std::size_t source_offset = 0;
  std::size_t target_offset = 0;
};

class SlottedRow final {
 public:
  explicit SlottedRow(SlotConfigurationPtr slots);

  // Reuse the row's storage for another logical row. If the layout is
  // unchanged this only resets the variant tags and does not allocate.
  void Reset();
  void Reset(SlotConfigurationPtr slots);

  [[nodiscard]] const SlotConfigurationPtr &Slots() const noexcept {
    return slots_;
  }
  [[nodiscard]] bool IsInitialized(std::size_t offset) const;
  [[nodiscard]] std::int64_t EntityIdAt(std::size_t offset) const;
  [[nodiscard]] const graphdb::Vertex &VertexAt(std::size_t offset) const;
  [[nodiscard]] const graphdb::Edge &EdgeAt(std::size_t offset) const;
  [[nodiscard]] const Value &ValueAt(std::size_t offset) const;
  [[nodiscard]] bool ReadProperty(std::size_t offset,
                                  std::string_view property_key,
                                  Value *value) const;

  void SetVertex(std::size_t offset, graphdb::Vertex vertex);
  void SetEdge(std::size_t offset, graphdb::Edge edge);
  void Set(std::size_t offset, Value value);
  void SetNull(std::size_t offset);
  void CopySlotFrom(const SlottedRow &source, std::size_t source_offset,
                    std::size_t target_offset);
  void MaterializeGraphEntities();

  [[nodiscard]] Value Get(std::size_t offset) const;
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
[[nodiscard]] bool TryBindSlot(SlottedRow *row, std::size_t offset,
                               Value value);
[[nodiscard]] bool TryBindEdge(SlottedRow *row, std::size_t offset,
                               graphdb::Edge edge);
[[nodiscard]] std::size_t EstimatedValueHeapUsage(const Value &value);

}  // namespace rg
