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

#include "common/string_view_hash.h"
#include "graphdb/graph_entity.h"
#include "value/value.h"

namespace rg {

class RowLayout final {
 public:
  explicit RowLayout(std::vector<std::string> columns = {});

  [[nodiscard]] std::optional<std::size_t> Find(std::string_view name) const;
  [[nodiscard]] std::size_t At(std::string_view name) const;
  [[nodiscard]] bool Contains(std::string_view name) const;
  [[nodiscard]] const std::vector<std::string> &Columns() const noexcept {
    return columns_;
  }
  [[nodiscard]] std::size_t CellCount() const noexcept {
    return columns_.size();
  }

 private:
  std::vector<std::string> columns_;
  std::unordered_map<std::string, std::size_t, common::StringViewHash,
                     std::equal_to<>>
      offsets_;
};

using RowLayoutPtr = std::shared_ptr<const RowLayout>;

struct RowMapping {
  std::size_t source_offset = 0;
  std::size_t target_offset = 0;
};

class ExecutionRow final {
 public:
  explicit ExecutionRow(RowLayoutPtr layout);

  // Reuse the row's storage for another logical row. If the layout is
  // unchanged this only resets the variant tags and does not allocate.
  void Reset();
  void Reset(RowLayoutPtr layout);

  [[nodiscard]] const RowLayoutPtr &Layout() const noexcept { return layout_; }
  [[nodiscard]] bool IsInitialized(std::size_t offset) const;
  [[nodiscard]] std::int64_t EntityIdAt(std::size_t offset) const;
  [[nodiscard]] const graphdb::Vertex &VertexAt(std::size_t offset) const;
  [[nodiscard]] const graphdb::Edge &EdgeAt(std::size_t offset) const;
  [[nodiscard]] const Value &ValueAt(std::size_t offset) const;
  [[nodiscard]] bool CellEquals(std::size_t offset, const ExecutionRow &other,
                                std::size_t other_offset) const;
  [[nodiscard]] bool ReadProperty(std::size_t offset,
                                  std::string_view property_key,
                                  Value *value) const;

  void Set(std::size_t offset, graphdb::Vertex vertex);
  void Set(std::size_t offset, graphdb::Edge edge);
  void Set(std::size_t offset, Value value);
  void SetNull(std::size_t offset);
  void CopyCellFrom(const ExecutionRow &source, std::size_t source_offset,
                    std::size_t target_offset);
  void MaterializeGraphEntities();

  [[nodiscard]] Value Get(std::size_t offset) const;
  [[nodiscard]] std::size_t EstimatedHeapUsage() const;
  [[nodiscard]] ExecutionRow CopyTo(
      RowLayoutPtr target, const std::vector<RowMapping> &mappings) const;

 private:
  struct UninitializedCell {};
  using RowCell =
      std::variant<UninitializedCell, graphdb::Vertex, graphdb::Edge, Value>;

  void SetRowCell(std::size_t offset, RowCell value);

  RowLayoutPtr layout_;
  std::vector<RowCell> values_;
};

void CopyCells(const ExecutionRow &source, ExecutionRow *target,
               const std::vector<RowMapping> &mappings);
[[nodiscard]] bool TryBindAt(ExecutionRow *row, std::size_t offset,
                             Value value);
[[nodiscard]] bool TryBindEdge(ExecutionRow *row, std::size_t offset,
                               graphdb::Edge edge);
[[nodiscard]] std::size_t EstimatedValueHeapUsage(const Value &value);

}  // namespace rg
