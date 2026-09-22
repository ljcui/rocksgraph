#include "runtime/execution_row.h"

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

RowLayout::RowLayout(std::vector<std::string> columns) {
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

std::optional<std::size_t> RowLayout::Find(std::string_view name) const {
  const auto found = offsets_.find(name);
  if (found == offsets_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::size_t RowLayout::At(std::string_view name) const {
  const std::optional<std::size_t> offset = Find(name);
  RG_CHECK(offset.has_value(), common::ErrorCode::InvalidParameter,
           "column is not allocated: " + std::string(name));
  return *offset;
}

bool RowLayout::Contains(std::string_view name) const {
  return Find(name).has_value();
}

ExecutionRow::ExecutionRow(RowLayoutPtr layout) : layout_(std::move(layout)) {
  RG_CHECK(layout_ != nullptr, common::ErrorCode::InvalidParameter,
           "row layout is null");
  values_.resize(layout_->CellCount());
}

void ExecutionRow::Reset() {
  for (auto &value : values_) {
    value = UninitializedCell{};
  }
}

void ExecutionRow::Reset(RowLayoutPtr layout) {
  RG_CHECK(layout != nullptr, common::ErrorCode::InvalidParameter,
           "row layout is null");
  if (layout_ != layout) {
    layout_ = std::move(layout);
    values_.clear();
    values_.resize(layout_->CellCount());
    return;
  }
  Reset();
}

bool ExecutionRow::IsInitialized(std::size_t offset) const {
  RG_CHECK(offset < values_.size(), common::ErrorCode::InternalError,
           "row offset is out of range");
  return !std::holds_alternative<UninitializedCell>(values_[offset]);
}

std::int64_t ExecutionRow::EntityIdAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "entity offset is not initialized");
  const RowCell &stored = values_[offset];
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    return vertex->GetId();
  }
  if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
    return edge->GetId();
  }
  const auto *value = std::get_if<Value>(&stored);
  RG_CHECK(value != nullptr, common::ErrorCode::InternalError,
           "initialized offset contains no value");
  if (value->IsNull()) {
    return -1;
  }
  if (value->IsNode()) {
    return value->AsNode().id;
  }
  if (value->IsRelationship()) {
    return value->AsRelationship().id;
  }
  RG_THROW(common::ErrorCode::InvalidParameter,
           "offset does not contain a graph entity");
}

const graphdb::Vertex &ExecutionRow::VertexAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "node offset is not initialized");
  const auto *vertex = std::get_if<graphdb::Vertex>(&values_[offset]);
  RG_CHECK(vertex != nullptr, common::ErrorCode::InvalidParameter,
           "node offset is null");
  return *vertex;
}

const graphdb::Edge &ExecutionRow::EdgeAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "relationship offset is not initialized");
  const auto *edge = std::get_if<graphdb::Edge>(&values_[offset]);
  RG_CHECK(edge != nullptr, common::ErrorCode::InvalidParameter,
           "relationship offset is null");
  return *edge;
}

const Value &ExecutionRow::ValueAt(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "value offset is not initialized");
  const auto *value = std::get_if<Value>(&values_[offset]);
  RG_CHECK(value != nullptr, common::ErrorCode::InternalError,
           "offset does not contain a Value");
  return *value;
}

bool ExecutionRow::CellEquals(std::size_t offset, const ExecutionRow &other,
                              std::size_t other_offset) const {
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "left offset is not initialized");
  RG_CHECK(other.IsInitialized(other_offset),
           common::ErrorCode::InvalidParameter,
           "right offset is not initialized");
  const RowCell &left = values_[offset];
  const RowCell &right = other.values_[other_offset];

  auto node_id = [](const RowCell &stored) -> std::optional<std::int64_t> {
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
      [](const RowCell &stored) -> std::optional<std::int64_t> {
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
           common::ErrorCode::InternalError,
           "initialized cells contain no comparable values");
  return ValuesEqual(*left_value, *right_value);
}

bool ExecutionRow::ReadProperty(std::size_t offset,
                                std::string_view property_key,
                                Value *value) const {
  RG_CHECK(value != nullptr, common::ErrorCode::InternalError,
           "property output is null");
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "offset is not initialized");
  const RowCell &stored = values_[offset];
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

void ExecutionRow::Set(std::size_t offset, graphdb::Vertex vertex) {
  SetRowCell(offset, std::move(vertex));
}

void ExecutionRow::Set(std::size_t offset, graphdb::Edge edge) {
  SetRowCell(offset, std::move(edge));
}

void ExecutionRow::Set(std::size_t offset, Value value) {
  SetRowCell(offset, std::move(value));
}

void ExecutionRow::SetRowCell(std::size_t offset, RowCell value) {
  RG_CHECK(offset < values_.size(), common::ErrorCode::InternalError,
           "row offset is out of range");
  values_[offset] = std::move(value);
}

void ExecutionRow::SetNull(std::size_t offset) { Set(offset, Value::Null()); }

void ExecutionRow::CopyCellFrom(const ExecutionRow &source,
                                std::size_t source_offset,
                                std::size_t target_offset) {
  RG_CHECK(source.IsInitialized(source_offset),
           common::ErrorCode::InvalidParameter,
           "source offset is not initialized");
  values_[target_offset] = source.values_[source_offset];
}

void ExecutionRow::MaterializeGraphEntities() {
  for (RowCell &stored : values_) {
    if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
      stored = Value(MaterializeGraphDBVertex(*vertex));
    } else if (const auto *edge = std::get_if<graphdb::Edge>(&stored)) {
      stored = Value(MaterializeGraphDBEdge(*edge));
    }
  }
}

Value ExecutionRow::Get(std::size_t offset) const {
  RG_CHECK(IsInitialized(offset), common::ErrorCode::InvalidParameter,
           "offset is not initialized");
  const RowCell &stored = values_[offset];
  if (const auto *value = std::get_if<Value>(&stored)) {
    return *value;
  }
  if (const auto *vertex = std::get_if<graphdb::Vertex>(&stored)) {
    return Value(MaterializeGraphDBVertex(*vertex));
  }
  const auto *edge = std::get_if<graphdb::Edge>(&stored);
  RG_CHECK(edge != nullptr, common::ErrorCode::InternalError,
           "initialized offset contains no value");
  return Value(MaterializeGraphDBEdge(*edge));
}

std::size_t ExecutionRow::EstimatedHeapUsage() const {
  std::size_t bytes =
      sizeof(ExecutionRow) + values_.capacity() * sizeof(RowCell);
  for (const auto &cell : values_) {
    if (const auto *value = std::get_if<Value>(&cell)) {
      bytes += EstimatedValueHeapUsage(*value);
    }
  }
  return bytes;
}

ExecutionRow ExecutionRow::CopyTo(
    RowLayoutPtr target, const std::vector<RowMapping> &mappings) const {
  ExecutionRow out(std::move(target));
  CopyCells(*this, &out, mappings);
  return out;
}

void CopyCells(const ExecutionRow &source, ExecutionRow *target,
               const std::vector<RowMapping> &mappings) {
  RG_CHECK(target != nullptr, common::ErrorCode::InternalError,
           "target row is null");
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source_offset)) {
      continue;
    }
    target->CopyCellFrom(source, mapping.source_offset, mapping.target_offset);
  }
}

bool TryBindAt(ExecutionRow *row, std::size_t offset, Value value) {
  RG_CHECK(row != nullptr, common::ErrorCode::InternalError,
           "query row is null");
  if (!row->IsInitialized(offset)) {
    row->Set(offset, std::move(value));
    return true;
  }
  return ValuesEqual(row->Get(offset), value);
}

bool TryBindEdge(ExecutionRow *row, std::size_t offset, graphdb::Edge edge) {
  RG_CHECK(row != nullptr, common::ErrorCode::InternalError,
           "query row is null");
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
