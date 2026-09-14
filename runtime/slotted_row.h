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

#include "ast/semantic_table.h"
#include "graphdb/graph_entity.h"
#include "value/value.h"

namespace txn {
class Transaction;
}

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
};

struct SlotDefinition {
  std::string name;
  ast::SemanticVariableType type = ast::SemanticVariableType::kUnknown;
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
  [[nodiscard]] const Value &ReferenceAt(const Slot &slot) const;

  void SetVertex(const Slot &slot, graphdb::Vertex vertex);
  void SetEdge(const Slot &slot, graphdb::Edge edge);
  void SetReference(const Slot &slot, Value value);
  void Set(const Slot &slot, Value value, txn::Transaction &transaction);
  void Set(std::string_view name, Value value, txn::Transaction &transaction);
  void SetNull(const Slot &slot);
  void SetNull(std::string_view name);
  void CopySlotFrom(const SlottedRow &source, const Slot &source_slot,
                    const Slot &target_slot);

  [[nodiscard]] Value Get(std::string_view name) const;
  [[nodiscard]] Value Get(const Slot &slot) const;
  [[nodiscard]] std::size_t EstimatedHeapUsage() const;
  [[nodiscard]] SlottedRow CopyTo(SlotConfigurationPtr target,
                                  const std::vector<SlotMapping> &mappings,
                                  txn::Transaction &transaction) const;

 private:
  struct UninitializedSlot {};
  using SlotValue =
      std::variant<UninitializedSlot, graphdb::Vertex, graphdb::Edge, Value>;

  SlotConfigurationPtr slots_;
  std::vector<SlotValue> values_;
};

void CopySlots(const SlottedRow &source, SlottedRow *target,
               const std::vector<SlotMapping> &mappings,
               txn::Transaction &transaction);
[[nodiscard]] bool TryBindSlot(SlottedRow *row, const Slot &slot, Value value,
                               txn::Transaction &transaction);
[[nodiscard]] bool TryBindSlot(SlottedRow *row, std::string_view name,
                               Value value, txn::Transaction &transaction);
[[nodiscard]] bool TryBindVertex(SlottedRow *row, const Slot &slot,
                                 graphdb::Vertex vertex,
                                 txn::Transaction &transaction);
[[nodiscard]] bool TryBindVertex(SlottedRow *row, std::string_view name,
                                 graphdb::Vertex vertex,
                                 txn::Transaction &transaction);
[[nodiscard]] bool TryBindEdge(SlottedRow *row, const Slot &slot,
                               graphdb::Edge edge,
                               txn::Transaction &transaction);
[[nodiscard]] bool TryBindEdge(SlottedRow *row, std::string_view name,
                               graphdb::Edge edge,
                               txn::Transaction &transaction);
[[nodiscard]] std::size_t EstimatedValueHeapUsage(const Value &value);

}  // namespace rg
