/**
 * Copyright 2024 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

//
// Created by botu.wzy
//

#include "graphdb/vertex_index_updater.h"

#include <optional>
#include <vector>

#include "common/value.h"
#include "graphdb/graph_db.h"
#include "graphdb/index.h"
#include "transaction/transaction.h"

namespace graphdb {

namespace {

struct FullTextDocument {
  std::vector<std::string> fields;
  std::vector<std::string> values;

  bool Empty() const { return fields.empty(); }
};

Value DeserializePropertyValue(const std::string& value) {
  Value ret;
  ret.Deserialize(value.data(), value.size());
  return ret;
}

std::unordered_set<uint32_t> CollectTouchedPropertyIds(
    const VertexSerializedProperties& old_properties,
    const VertexSerializedProperties& new_properties) {
  std::unordered_set<uint32_t> touched_pids;
  for (const auto& [pid, old_value] : old_properties) {
    auto iter = new_properties.find(pid);
    if (iter == new_properties.end() || iter->second != old_value) {
      touched_pids.insert(pid);
    }
  }
  for (const auto& [pid, new_value] : new_properties) {
    auto iter = old_properties.find(pid);
    if (iter == old_properties.end() || iter->second != new_value) {
      touched_pids.insert(pid);
    }
  }
  return touched_pids;
}

std::optional<std::vector<Value>> BuildPropertyIndexValues(
    const std::shared_ptr<VertexPropertyIndex>& index,
    const VertexSerializedProperties& properties) {
  std::vector<Value> values;
  values.reserve(index->PropertyCount());
  for (auto pid : index->pids()) {
    auto iter = properties.find(pid);
    if (iter == properties.end()) {
      return std::nullopt;
    }
    values.emplace_back(DeserializePropertyValue(iter->second));
  }
  return values;
}

FullTextDocument BuildFullTextDocument(
    txn::Transaction* txn, const std::shared_ptr<VertexFullTextIndex>& index,
    const std::unordered_set<uint32_t>& lids,
    const VertexSerializedProperties& properties) {
  FullTextDocument document;
  if (!index->MatchLabelIds(lids)) {
    return document;
  }
  for (auto pid : index->PropertyIds()) {
    auto iter = properties.find(pid);
    if (iter == properties.end()) {
      continue;
    }
    auto value = DeserializePropertyValue(iter->second);
    if (!value.IsString() || value.AsString().empty()) {
      continue;
    }
    document.fields.push_back(
        txn->db()->id_generator().GetPropertyName(pid).value());
    document.values.push_back(value.AsString());
  }
  return document;
}

bool IsSameFullTextDocument(const FullTextDocument& lhs,
                            const FullTextDocument& rhs) {
  return lhs.fields == rhs.fields && lhs.values == rhs.values;
}

meta::FullTextIndexUpdate BuildFullTextAddUpdate(int64_t vid,
                                                 FullTextDocument document) {
  meta::FullTextIndexUpdate add;
  add.set_vid(vid);
  add.set_type(meta::UpdateType::Add);
  for (auto& field : document.fields) {
    add.add_fields(std::move(field));
  }
  for (auto& value : document.values) {
    add.add_values(std::move(value));
  }
  return add;
}

std::optional<std::vector<float>> BuildVectorValues(
    const std::shared_ptr<VertexVectorIndex>& index,
    const std::unordered_set<uint32_t>& lids,
    const VertexVectorProperties& vector_properties) {
  if (!lids.count(index->lid())) {
    return std::nullopt;
  }
  auto iter = vector_properties.find(index->pid());
  if (iter == vector_properties.end()) {
    return std::nullopt;
  }
  if (iter->second.size() != index->meta().dimensions()) {
    THROW_CODE(InvalidParameter,
               "vector field [label:{}, property:{}] dimension mismatch, "
               "expect {}, actual {}",
               index->meta().label(), index->meta().property(),
               index->meta().dimensions(), iter->second.size());
  }
  return iter->second;
}

meta::VectorIndexUpdate BuildVectorAddUpdate(const std::vector<float>& vector) {
  meta::VectorIndexUpdate add;
  add.set_type(meta::UpdateType::Add);
  for (auto item : vector) {
    add.add_vector(item);
  }
  return add;
}

bool IsSameVectorValues(const std::optional<std::vector<float>>& lhs,
                        const std::optional<std::vector<float>>& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  if (!lhs.has_value()) {
    return true;
  }
  return *lhs == *rhs;
}

void UpdatePropertyIndexes(txn::Transaction* txn, int64_t vid,
                           const std::unordered_set<uint32_t>& old_lids,
                           const std::unordered_set<uint32_t>& new_lids,
                           const VertexSerializedProperties& old_properties,
                           const VertexSerializedProperties& new_properties,
                           const std::unordered_set<uint32_t>& touched_pids,
                           bool labels_changed) {
  for (const auto& index : txn->db()->meta_info().GetVertexPropertyIndexes()) {
    bool old_label_match = old_lids.count(index->lid());
    bool new_label_match = new_lids.count(index->lid());
    if (!old_label_match && !new_label_match) {
      continue;
    }
    if (!labels_changed && !index->TouchesAnyProperty(touched_pids)) {
      continue;
    }
    auto old_values = old_label_match
                          ? BuildPropertyIndexValues(index, old_properties)
                          : std::nullopt;
    auto new_values = new_label_match
                          ? BuildPropertyIndexValues(index, new_properties)
                          : std::nullopt;
    index->UpdateIndex(txn, vid, new_values, old_values);
  }
}

void UpdateFullTextIndexes(txn::Transaction* txn, int64_t vid,
                           const std::unordered_set<uint32_t>& old_lids,
                           const std::unordered_set<uint32_t>& new_lids,
                           const VertexSerializedProperties& old_properties,
                           const VertexSerializedProperties& new_properties,
                           const std::unordered_set<uint32_t>& touched_pids,
                           bool labels_changed) {
  for (const auto& index : txn->db()->meta_info().GetVertexFullTextIndexes()) {
    if (!index->MatchLabelIds(old_lids) && !index->MatchLabelIds(new_lids)) {
      continue;
    }
    if (!labels_changed && !index->MatchPropertyIds(touched_pids)) {
      continue;
    }
    auto old_document =
        BuildFullTextDocument(txn, index, old_lids, old_properties);
    auto new_document =
        BuildFullTextDocument(txn, index, new_lids, new_properties);
    if (IsSameFullTextDocument(old_document, new_document)) {
      continue;
    }
    if (!old_document.Empty()) {
      meta::FullTextIndexUpdate del;
      del.set_vid(vid);
      del.set_type(meta::UpdateType::Delete);
      index->DeleteIndex(txn, vid, del);
    }
    if (!new_document.Empty()) {
      auto add = BuildFullTextAddUpdate(vid, std::move(new_document));
      index->AddIndex(txn, vid, add);
    }
  }
}

void UpdateVectorIndexes(txn::Transaction* txn, int64_t vid,
                         const std::unordered_set<uint32_t>& old_lids,
                         const std::unordered_set<uint32_t>& new_lids,
                         const VertexVectorProperties& old_vector_properties,
                         const VertexVectorProperties& new_vector_properties,
                         const std::unordered_set<uint32_t>& touched_pids,
                         bool labels_changed) {
  for (const auto& index : txn->db()->meta_info().GetVertexVectorIndexes()) {
    bool old_label_match = old_lids.count(index->lid());
    bool new_label_match = new_lids.count(index->lid());
    if (!old_label_match && !new_label_match) {
      continue;
    }
    if (!labels_changed && !touched_pids.count(index->pid())) {
      continue;
    }
    auto old_vector = BuildVectorValues(index, old_lids, old_vector_properties);
    auto new_vector = BuildVectorValues(index, new_lids, new_vector_properties);
    if (IsSameVectorValues(old_vector, new_vector)) {
      continue;
    }
    if (old_vector.has_value()) {
      index->DeleteIfPresent(txn, vid);
    }
    if (new_vector.has_value()) {
      auto add = BuildVectorAddUpdate(*new_vector);
      index->AddIndex(txn, vid, add);
    }
  }
}

}  // namespace

void UpdateVertexIndexes(txn::Transaction* txn, int64_t vid,
                         const std::unordered_set<uint32_t>& old_lids,
                         const std::unordered_set<uint32_t>& new_lids,
                         const VertexSerializedProperties& old_properties,
                         const VertexSerializedProperties& new_properties,
                         const VertexVectorProperties& old_vector_properties,
                         const VertexVectorProperties& new_vector_properties,
                         const std::unordered_set<uint32_t>& touched_pids) {
  bool labels_changed = old_lids != new_lids;
  auto effective_touched_pids = touched_pids;
  if (effective_touched_pids.empty()) {
    effective_touched_pids =
        CollectTouchedPropertyIds(old_properties, new_properties);
  }
  if (!labels_changed && effective_touched_pids.empty()) {
    return;
  }

  UpdatePropertyIndexes(txn, vid, old_lids, new_lids, old_properties,
                        new_properties, effective_touched_pids, labels_changed);
  UpdateFullTextIndexes(txn, vid, old_lids, new_lids, old_properties,
                        new_properties, effective_touched_pids, labels_changed);
  UpdateVectorIndexes(txn, vid, old_lids, new_lids, old_vector_properties,
                      new_vector_properties, effective_touched_pids,
                      labels_changed);
}

}  // namespace graphdb
