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

#include "graph_entity.h"

#include <rocksdb/utilities/write_batch_with_index.h>

#include "common/byte_utils.h"
#include "common/exceptions.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graphdb/vector_property.h"
#include "graphdb/vertex_index_updater.h"
#include "transaction/transaction.h"
using namespace boost::endian;
using common::AsChars;
using common::ReadValue;
namespace graphdb {
namespace {

constexpr char kEdgeLockKeyPrefix = static_cast<char>(0xFF);

std::string BuildEdgeLockKey(int64_t eid) {
  std::string key(1, kEdgeLockKeyPrefix);
  key.append(AsChars(eid), sizeof(eid));
  return key;
}

std::string VertexPropertyKey(int64_t vid, uint32_t pid) {
  std::string key(AsChars(vid), sizeof(vid));
  key.append(AsChars(pid), sizeof(pid));
  return key;
}

VertexSerializedProperties LoadVertexSerializedProperties(
    txn::Transaction *txn, int64_t vid,
    const std::unordered_set<uint32_t> &pids) {
  VertexSerializedProperties props;
  rocksdb::ReadOptions ro;
  for (auto pid : pids) {
    std::string pkey = VertexPropertyKey(vid, pid);
    std::string value;
    auto s = txn->dbtxn()->Get(ro, txn->db()->graph_cf().vertex_property, pkey,
                               &value);
    if (s.IsNotFound()) {
      continue;
    }
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    props.emplace(pid, std::move(value));
  }
  return props;
}

VertexVectorProperties LoadVertexVectorProperties(
    txn::Transaction *txn, int64_t vid,
    const std::unordered_set<uint32_t> &lids,
    const std::unordered_set<uint32_t> &pids) {
  VertexVectorProperties props;
  rocksdb::ReadOptions ro;
  for (auto pid : pids) {
    for (const auto &field :
         txn->db()->meta_info().GetVertexVectorFields(lids, pid)) {
      uint32_t lid = native_to_big(field->label_id());
      std::string key = VertexVectorPropertyKey(lid, pid, vid);
      std::string value;
      auto s = txn->dbtxn()->Get(
          ro, txn->db()->graph_cf().vertex_vector_property, key, &value);
      if (s.IsNotFound()) {
        continue;
      }
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      props[pid] = DeserializeVector(value, field->dimensions());
    }
  }
  return props;
}

VertexVectorProperties LoadAllVertexVectorProperties(
    txn::Transaction *txn, int64_t vid,
    const std::unordered_set<uint32_t> &lids,
    std::unordered_set<uint32_t> *pids, std::vector<std::string> *keys) {
  VertexVectorProperties props;
  rocksdb::ReadOptions ro;
  for (const auto &field : txn->db()->meta_info().GetVertexVectorFields(lids)) {
    uint32_t lid = native_to_big(field->label_id());
    uint32_t pid = native_to_big(field->property_id());
    std::string key = VertexVectorPropertyKey(lid, pid, vid);
    std::string value;
    auto s = txn->dbtxn()->Get(ro, txn->db()->graph_cf().vertex_vector_property,
                               key, &value);
    if (s.IsNotFound()) {
      continue;
    }
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    props[pid] = DeserializeVector(value, field->dimensions());
    if (pids != nullptr) {
      pids->insert(pid);
    }
    if (keys != nullptr) {
      keys->push_back(std::move(key));
    }
  }
  return props;
}

std::unordered_set<uint32_t> CollectVertexIndexPropertyIds(
    txn::Transaction *txn, const std::unordered_set<uint32_t> &old_lids,
    const std::unordered_set<uint32_t> &new_lids,
    const std::unordered_set<uint32_t> &touched_pids) {
  std::unordered_set<uint32_t> required_pids;
  bool labels_changed = old_lids != new_lids;
  if (!labels_changed && touched_pids.empty()) {
    return required_pids;
  }

  auto &meta_info = txn->db()->meta_info();
  for (const auto &index : meta_info.GetVertexPropertyIndexes()) {
    bool old_label_match = old_lids.count(index->lid());
    bool new_label_match = new_lids.count(index->lid());
    if (!old_label_match && !new_label_match) {
      continue;
    }
    if (!labels_changed && !index->TouchesAnyProperty(touched_pids)) {
      continue;
    }
    required_pids.insert(index->pids().begin(), index->pids().end());
  }

  for (const auto &index : meta_info.GetVertexFullTextIndexes()) {
    if (!index->MatchLabelIds(old_lids) && !index->MatchLabelIds(new_lids)) {
      continue;
    }
    if (!labels_changed && !index->MatchPropertyIds(touched_pids)) {
      continue;
    }
    const auto &pids = index->PropertyIds();
    required_pids.insert(pids.begin(), pids.end());
  }

  for (const auto &index : meta_info.GetVertexVectorIndexes()) {
    bool old_label_match = old_lids.count(index->lid());
    bool new_label_match = new_lids.count(index->lid());
    if (!old_label_match && !new_label_match) {
      continue;
    }
    if (!labels_changed && !touched_pids.count(index->pid())) {
      continue;
    }
    required_pids.insert(index->pid());
  }

  return required_pids;
}

}  // namespace

std::unique_ptr<EdgeIterator> Vertex::NewEdgeIterator(
    EdgeDirection direction, const std::unordered_set<std::string> &types,
    const std::unordered_map<std::string, Value> &props) {
  std::unordered_map<uint32_t, Value> prop_map;
  for (auto &[name, val] : props) {
    auto pid = txn_->db()->id_generator().GetPid(name);
    if (pid) {
      prop_map.emplace(pid.value(), val);
    } else {
      return std::make_unique<NoEdgeFound>(txn_);
    }
  }
  std::unordered_set<uint32_t> type_set;
  for (auto &type : types) {
    auto tid = txn_->db()->id_generator().GetTid(type);
    if (tid) {
      type_set.insert(tid.value());
    }
  }
  if (!types.empty() && type_set.empty()) {
    return std::make_unique<NoEdgeFound>(txn_);
  }
  return std::make_unique<ScanEdgeByVidDirectionTypesProperties>(
      txn_, id_, direction, std::move(type_set), std::move(prop_map));
}

std::unique_ptr<EdgeIterator> Vertex::NewEdgeIterator(
    EdgeDirection direction, const std::unordered_set<std::string> &types,
    const std::unordered_map<std::string, Value> &props,
    const std::unordered_set<std::string> &other_node_labels,
    const std::unordered_map<std::string, Value> &other_node_props) {
  std::unordered_map<uint32_t, Value> prop_map;
  if (!props.empty()) {
    for (auto &[name, val] : props) {
      auto pid = txn_->db()->id_generator().GetPid(name);
      if (pid) {
        prop_map.emplace(pid.value(), val);
      } else {
        return std::make_unique<NoEdgeFound>(txn_);
      }
    }
  }
  std::unordered_set<uint32_t> type_set;
  if (!types.empty()) {
    for (auto &type : types) {
      auto tid = txn_->db()->id_generator().GetTid(type);
      if (tid) {
        type_set.insert(tid.value());
      }
    }
    if (type_set.empty()) {
      return std::make_unique<NoEdgeFound>(txn_);
    }
  }
  std::unordered_set<uint32_t> other_node_label_set;
  if (!other_node_labels.empty()) {
    for (auto &label : other_node_labels) {
      auto lid = txn_->db()->id_generator().GetLid(label);
      if (lid) {
        other_node_label_set.insert(lid.value());
      }
    }
    if (other_node_label_set.empty()) {
      return std::make_unique<NoEdgeFound>(txn_);
    }
  }
  std::unordered_map<uint32_t, Value> other_node_prop_map;
  if (!other_node_props.empty()) {
    for (auto &[name, val] : other_node_props) {
      auto pid = txn_->db()->id_generator().GetPid(name);
      if (pid) {
        other_node_prop_map.emplace(pid.value(), val);
      } else {
        return std::make_unique<NoEdgeFound>(txn_);
      }
    }
  }
  return std::make_unique<ScanEdgeByVidDirectionTypesPropertiesOtherNode>(
      txn_, id_, direction, std::move(type_set), std::move(prop_map),
      std::move(other_node_label_set), std::move(other_node_prop_map));
}

std::unique_ptr<EdgeIterator> Vertex::NewEdgeIterator(
    EdgeDirection direction, const std::unordered_set<std::string> &types,
    const std::unordered_map<std::string, Value> &props,
    const Vertex &other_node) {
  std::unordered_map<uint32_t, Value> prop_map;
  if (!props.empty()) {
    for (auto &[name, val] : props) {
      auto pid = txn_->db()->id_generator().GetPid(name);
      if (pid) {
        prop_map.emplace(pid.value(), val);
      } else {
        return std::make_unique<NoEdgeFound>(txn_);
      }
    }
  }
  std::unordered_set<uint32_t> type_set;
  if (!types.empty()) {
    for (auto &type : types) {
      auto tid = txn_->db()->id_generator().GetTid(type);
      if (tid) {
        type_set.insert(tid.value());
      }
    }
    if (type_set.empty()) {
      return std::make_unique<NoEdgeFound>(txn_);
    }
  }

  return std::make_unique<ScanEdgeByVidDirectionTypesPropertiesOtherVid>(
      txn_, id_, direction, std::move(type_set), std::move(prop_map),
      other_node);
}

std::unique_ptr<EdgeIterator> Vertex::NewEdgeIterator(
    EdgeDirection direction, const std::string &type,
    const std::unordered_map<std::string, Value> &props,
    const Vertex &other_node) {
  if (direction == EdgeDirection::BOTH) {
    THROW_CODE(InputError, "EdgeDirection can not be BOTH");
  }
  auto tid = txn_->db()->id_generator().GetTid(type);
  if (!tid) {
    return std::make_unique<NoEdgeFound>(txn_);
  }
  std::unordered_map<uint32_t, Value> prop_map;
  if (!props.empty()) {
    for (auto &[name, val] : props) {
      auto pid = txn_->db()->id_generator().GetPid(name);
      if (pid) {
        prop_map.emplace(pid.value(), val);
      } else {
        return std::make_unique<NoEdgeFound>(txn_);
      }
    }
  }
  return std::make_unique<ScanEdgeByVidDirectionTypePropertiesOtherNode>(
      txn_, id_, direction, tid.value(), prop_map, other_node);
}

std::unordered_set<uint32_t> Vertex::GetLabelIds() {
  rocksdb::ReadOptions ro;
  std::string val;
  auto s = txn_->dbtxn()->Get(ro, txn_->db()->graph_cf().graph_topology,
                              rocksdb::Slice{AsChars(id_), sizeof(id_)}, &val);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  std::unordered_set<uint32_t> ret;
  for (size_t i = 0; i < val.size(); i += sizeof(uint32_t)) {
    ret.insert(ReadValue<uint32_t>(val.data() + i));
  }
  return ret;
}

void Vertex::Lock() {
  rocksdb::ReadOptions ro;
  auto s = txn_->dbtxn()->GetForUpdate(
      ro, txn_->db()->graph_cf().graph_topology, GetIdView(),
      static_cast<std::string *>(nullptr));
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

int Vertex::Delete() {
  int deleted_edge = 0;
  rocksdb::ReadOptions ro;
  // lock vertex
  Lock();
  std::unique_ptr<rocksdb::Iterator> iter;
  rocksdb::Slice prefix(AsChars(id_), sizeof(id_));
  iter.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().graph_topology));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key().ToString();  // must copy
    auto val = iter->value();
    if (key.size() == sizeof(int64_t)) {
      std::unordered_set<uint32_t> labelIds;
      for (size_t i = 0; i < val.size(); i += sizeof(uint32_t)) {
        labelIds.insert(ReadValue<uint32_t>(val.data() + i));
      }
      std::unordered_set<uint32_t> pids;
      std::unordered_map<uint32_t, std::string> props;
      VertexVectorProperties vector_props;
      std::vector<std::string> prop_keys;
      std::vector<std::string> vector_prop_keys;

      std::unique_ptr<rocksdb::Iterator> vp_iter;
      rocksdb::Slice vp_prefix = key;
      vp_iter.reset(txn_->dbtxn()->GetIterator(
          ro, txn_->db()->graph_cf().vertex_property));
      for (vp_iter->Seek(vp_prefix);
           vp_iter->Valid() && vp_iter->key().starts_with(vp_prefix);
           vp_iter->Next()) {
        auto p_key = vp_iter->key().ToString();  // must copy
        auto pid = ReadValue<uint32_t>(p_key.data() + sizeof(int64_t));
        pids.insert(pid);
        props.emplace(pid, vp_iter->value().ToString());
        prop_keys.push_back(std::move(p_key));
      }
      vector_props = LoadAllVertexVectorProperties(txn_, id_, labelIds, &pids,
                                                   &vector_prop_keys);
      // delete label vid
      for (auto labelId : labelIds) {
        std::string labelVid;
        labelVid.append(AsChars(labelId), sizeof(labelId));
        labelVid.append(key.data(), key.size());
        auto s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
            txn_->db()->graph_cf().vertex_label_vid, labelVid);
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      std::unordered_set<uint32_t> empty_lids;
      VertexSerializedProperties empty_properties;
      VertexVectorProperties empty_vector_properties;
      UpdateVertexIndexes(txn_, id_, labelIds, empty_lids, props,
                          empty_properties, vector_props,
                          empty_vector_properties, pids);
      for (auto &p_key : prop_keys) {
        auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
            txn_->db()->graph_cf().vertex_property, p_key);
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      for (auto &p_key : vector_prop_keys) {
        auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
            txn_->db()->graph_cf().vertex_vector_property, p_key);
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
          txn_->db()->graph_cf().graph_topology, key);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    } else {
      assert(key.size() == 29);
      auto p = key.data();
      int64_t vid1 = ReadValue<int64_t>(p);
      p += sizeof(int64_t);
      auto dir = static_cast<EdgeDirection>(*(p));
      p += sizeof(char);
      uint32_t etid = ReadValue<uint32_t>(p);
      p += sizeof(uint32_t);
      int64_t vid2 = ReadValue<int64_t>(p);
      p += sizeof(int64_t);
      int64_t eid = ReadValue<int64_t>(p);
      {
        // lock edge
        auto edge_lock_key = BuildEdgeLockKey(eid);
        auto s = txn_->dbtxn()->GetForUpdate(
            ro, txn_->db()->graph_cf().graph_topology,
            rocksdb::Slice(edge_lock_key), static_cast<std::string *>(nullptr));
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      // delete other edge key
      std::string other_edge_key;
      other_edge_key.append(AsChars(vid2), sizeof(vid2));
      other_edge_key.append(1,
                            static_cast<char>(dir == EdgeDirection::OUTGOING
                                                  ? EdgeDirection::INCOMING
                                                  : EdgeDirection::OUTGOING));
      other_edge_key.append(AsChars(etid), sizeof(etid));
      other_edge_key.append(AsChars(vid1), sizeof(vid1));
      other_edge_key.append(AsChars(eid), sizeof(eid));
      auto s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
          txn_->db()->graph_cf().graph_topology, other_edge_key);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      deleted_edge++;
      // delete type eid
      std::string typeEid;
      typeEid.append(AsChars(etid), sizeof(etid));
      typeEid.append(AsChars(eid), sizeof(eid));
      s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
          txn_->db()->graph_cf().edge_type_eid, typeEid);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      // delete edge properties
      std::unique_ptr<rocksdb::Iterator> ep_iter;
      rocksdb::Slice ep_prefix(AsChars(eid), sizeof(eid));
      ep_iter.reset(
          txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().edge_property));
      for (ep_iter->Seek(ep_prefix);
           ep_iter->Valid() && ep_iter->key().starts_with(ep_prefix);
           ep_iter->Next()) {
        auto prop_key = ep_iter->key().ToString();  // must copy
        s = txn_->dbtxn()->GetWriteBatch()->Delete(
            txn_->db()->graph_cf().edge_property, prop_key);
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
          txn_->db()->graph_cf().graph_topology, key);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
  }
  return deleted_edge;
}

std::unordered_set<std::string> Vertex::GetLabels() {
  std::unordered_set<std::string> ret;
  for (auto id : GetLabelIds()) {
    auto optional = txn_->db()->id_generator().GetVertexLabelName(id);
    if (optional.has_value()) {
      ret.emplace(std::move(optional.value()));
    }
  }
  return ret;
}

void Vertex::AddLabels(const std::unordered_set<std::string> &labels) {
  if (labels.empty()) {
    return;
  }
  std::unordered_set<uint32_t> add_lids, new_lids;
  for (auto &label : labels) {
    auto lid = txn_->db()->id_generator().GetOrCreateLid(label);
    add_lids.insert(lid);
  }
  Lock();
  auto labelIds = GetLabelIds();
  for (auto &lid : add_lids) {
    if (labelIds.count(lid)) {
      continue;
    }
    new_lids.insert(lid);
  }
  if (new_lids.empty()) {
    return;
  }
  auto updated_lids = labelIds;
  updated_lids.insert(new_lids.begin(), new_lids.end());
  std::unordered_set<uint32_t> touched_pids;
  auto required_pids =
      CollectVertexIndexPropertyIds(txn_, labelIds, updated_lids, touched_pids);
  auto updated_vector_fields =
      txn_->db()->meta_info().GetVertexVectorFields(updated_lids);
  for (const auto &field : updated_vector_fields) {
    required_pids.insert(native_to_big(field->property_id()));
  }
  std::vector<std::pair<std::string, std::string>> vector_writes;
  if (!required_pids.empty()) {
    auto props = LoadVertexSerializedProperties(txn_, id_, required_pids);
    auto updated_props = props;
    auto vector_props =
        LoadVertexVectorProperties(txn_, id_, labelIds, required_pids);
    auto updated_vector_props =
        LoadVertexVectorProperties(txn_, id_, updated_lids, required_pids);
    for (const auto &field : updated_vector_fields) {
      uint32_t lid = native_to_big(field->label_id());
      uint32_t pid = native_to_big(field->property_id());
      auto prop_iter = props.find(pid);
      auto vector_iter = updated_vector_props.find(pid);
      if (vector_iter == updated_vector_props.end() &&
          prop_iter != props.end()) {
        Value value;
        value.Deserialize(prop_iter->second.data(), prop_iter->second.size());
        auto vector = ParseVectorValue(value, field->dimensions());
        vector_iter =
            updated_vector_props.emplace(pid, std::move(vector)).first;
      }
      if (vector_iter != updated_vector_props.end()) {
        if (vector_iter->second.size() != field->dimensions()) {
          THROW_CODE(InvalidParameter,
                     "vector field [label:{}, property:{}] dimension "
                     "mismatch, expect {}, actual {}",
                     field->label(), field->property(), field->dimensions(),
                     vector_iter->second.size());
        }
        vector_writes.emplace_back(VertexVectorPropertyKey(lid, pid, id_),
                                   SerializeVector(vector_iter->second));
      }
      if (prop_iter != props.end()) {
        updated_props.erase(pid);
      }
    }
    UpdateVertexIndexes(txn_, id_, labelIds, updated_lids, props, updated_props,
                        vector_props, updated_vector_props, touched_pids);
  }
  labelIds = std::move(updated_lids);
  std::string buffer;
  for (auto l : labelIds) {
    buffer.append(AsChars(l), sizeof(l));
  }
  auto s = txn_->dbtxn()->GetWriteBatch()->Put(
      txn_->db()->graph_cf().graph_topology, GetIdView(), buffer);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  for (auto &id : new_lids) {
    std::string key(AsChars(id), sizeof(id));
    key.append(GetIdView());
    s = txn_->dbtxn()->GetWriteBatch()->Put(
        txn_->db()->graph_cf().vertex_label_vid, key, {});
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  for (const auto &[key, value] : vector_writes) {
    s = txn_->dbtxn()->GetWriteBatch()->Put(
        txn_->db()->graph_cf().vertex_vector_property, key, value);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
}

void Vertex::DeleteLabels(const std::unordered_set<std::string> &labels) {
  std::unordered_set<uint32_t> lids, remove_lids;
  for (auto &label : labels) {
    auto optional = txn_->db()->id_generator().GetLid(label);
    if (optional.has_value()) {
      lids.insert(optional.value());
    }
  }
  if (lids.empty()) {
    return;
  }
  Lock();
  auto labelIds = GetLabelIds();
  for (auto id : lids) {
    if (labelIds.count(id)) {
      remove_lids.insert(id);
    }
  }
  if (remove_lids.empty()) {
    return;
  }
  auto remaining_lids = labelIds;
  for (auto id : remove_lids) {
    remaining_lids.erase(id);
  }
  std::vector<std::string> removed_vector_keys;
  for (const auto &field :
       txn_->db()->meta_info().GetVertexVectorFields(remove_lids)) {
    uint32_t lid = native_to_big(field->label_id());
    uint32_t pid = native_to_big(field->property_id());
    removed_vector_keys.push_back(VertexVectorPropertyKey(lid, pid, id_));
  }
  std::unordered_set<uint32_t> touched_pids;
  auto required_pids = CollectVertexIndexPropertyIds(
      txn_, labelIds, remaining_lids, touched_pids);
  if (!required_pids.empty()) {
    auto props = LoadVertexSerializedProperties(txn_, id_, required_pids);
    auto vector_props =
        LoadVertexVectorProperties(txn_, id_, labelIds, required_pids);
    auto updated_vector_props =
        LoadVertexVectorProperties(txn_, id_, remaining_lids, required_pids);
    UpdateVertexIndexes(txn_, id_, labelIds, remaining_lids, props, props,
                        vector_props, updated_vector_props, touched_pids);
  }
  labelIds = std::move(remaining_lids);
  std::string buffer;
  for (auto l : labelIds) {
    buffer.append(AsChars(l), sizeof(l));
  }
  auto s = txn_->dbtxn()->GetWriteBatch()->Put(
      txn_->db()->graph_cf().graph_topology, GetIdView(), buffer);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  for (auto id : remove_lids) {
    std::string key(AsChars(id), sizeof(id));
    key.append(GetIdView());
    s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
        txn_->db()->graph_cf().vertex_label_vid, key);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  for (const auto &key : removed_vector_keys) {
    s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().vertex_vector_property, key);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
}

Value Vertex::GetProperty(const std::string &name) {
  auto optional = txn_->db()->id_generator().GetPid(name);
  if (!optional.has_value()) {
    return {};
  }
  return GetProperty(optional.value());
}

Value Vertex::GetProperty(uint32_t pid) {
  auto lids = GetLabelIds();
  if (!txn_->db()->meta_info().GetVertexVectorFields(lids, pid).empty()) {
    std::unordered_set<uint32_t> pids{pid};
    auto vector_properties = LoadVertexVectorProperties(txn_, id_, lids, pids);
    auto iter = vector_properties.find(pid);
    if (iter != vector_properties.end()) {
      return Value(iter->second);
    }
    return {};
  }
  rocksdb::ReadOptions ro;
  rocksdb::PinnableSlice pval;
  Value ret;
  std::string pkey(AsChars(id_), sizeof(id_));
  pkey.append(AsChars(pid), sizeof(pid));
  auto s = txn_->dbtxn()->Get(ro, txn_->db()->graph_cf().vertex_property, pkey,
                              &pval);
  if (s.ok()) {
    ret.Deserialize(pval.data(), pval.size());
  } else if (!s.IsNotFound()) {
    THROW_CODE(StorageEngineError, s.ToString());
  }
  return ret;
}

bool Vertex::TryGetVectorPropertyRaw(uint32_t pid, rocksdb::PinnableSlice *out,
                                     size_t *dimensions) {
  if (out == nullptr || dimensions == nullptr) {
    THROW_CODE(InvalidParameter, "output vector should not be null");
  }
  out->Reset();
  *dimensions = 0;
  auto lids = GetLabelIds();
  rocksdb::ReadOptions ro;
  for (const auto &field :
       txn_->db()->meta_info().GetVertexVectorFields(lids, pid)) {
    uint32_t lid = native_to_big(field->label_id());
    std::string key = VertexVectorPropertyKey(lid, pid, id_);
    auto s = txn_->dbtxn()->Get(
        ro, txn_->db()->graph_cf().vertex_vector_property, key, out);
    if (s.IsNotFound()) {
      continue;
    }
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    size_t expected_size = field->dimensions() * sizeof(float);
    if (out->size() != expected_size) {
      THROW_CODE(StorageEngineError,
                 "vector field value has invalid size, expect {}, actual {}",
                 expected_size, out->size());
    }
    *dimensions = field->dimensions();
    return true;
  }
  return false;
}

std::unordered_map<std::string, Value> Vertex::GetAllProperty() {
  std::string prefix(AsChars(id_), sizeof(id_));
  rocksdb::ReadOptions ro;
  std::unordered_map<std::string, Value> ret;
  auto lids = GetLabelIds();
  std::unordered_set<uint32_t> vector_pids;
  for (const auto &field :
       txn_->db()->meta_info().GetVertexVectorFields(lids)) {
    vector_pids.insert(native_to_big(field->property_id()));
  }
  std::unique_ptr<rocksdb::Iterator> p_iter;
  p_iter.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().vertex_property));
  for (p_iter->Seek(prefix);
       p_iter->Valid() && p_iter->key().starts_with(prefix); p_iter->Next()) {
    auto key = p_iter->key();
    auto value = p_iter->value();
    key.remove_prefix(sizeof(int64_t));
    uint32_t pid = ReadValue<uint32_t>(key.data());
    if (vector_pids.count(pid)) {
      continue;
    }
    auto optional = txn_->db()->id_generator().GetPropertyName(pid);
    if (optional.has_value()) {
      Value v;
      v.Deserialize(value.data(), value.size());
      ret.emplace(std::move(optional.value()), std::move(v));
    }
  }
  return ret;
}

int Vertex::GetDegree(graphdb::EdgeDirection direction) {
  int count = 0;
  for (auto eiter = NewEdgeIterator(direction, {}, {}); eiter->Valid();
       eiter->Next()) {
    count++;
  }
  return count;
}

void Vertex::SetProperties(
    const std::unordered_map<std::string, Value> &values) {
  if (values.empty()) {
    return;
  }
  VertexSerializedProperties serialized;
  VertexVectorProperties vector_properties;
  std::unordered_set<uint32_t> pids;
  std::vector<std::pair<uint32_t, const Value *>> property_values;
  property_values.reserve(values.size());
  for (const auto &[name, val] : values) {
    auto pid = txn_->db()->id_generator().GetOrCreatePid(name);
    pids.insert(pid);
    property_values.emplace_back(pid, &val);
  }
  Lock();
  auto lids = GetLabelIds();
  for (const auto &[pid, val] : property_values) {
    bool is_vector_property = false;
    for (const auto &field :
         txn_->db()->meta_info().GetVertexVectorFields(lids, pid)) {
      is_vector_property = true;
      auto vector = ParseVectorValue(*val, field->dimensions());
      vector_properties[pid] = vector;
    }
    if (!is_vector_property) {
      serialized[pid] = val->Serialize();
    }
  }
  auto required_pids = CollectVertexIndexPropertyIds(txn_, lids, lids, pids);
  if (required_pids.empty()) {
    for (auto &[pid, pval] : serialized) {
      std::string pkey = VertexPropertyKey(id_, pid);
      auto s = txn_->dbtxn()->GetWriteBatch()->Put(
          txn_->db()->graph_cf().vertex_property, pkey, pval);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
    for (const auto &[pid, vector] : vector_properties) {
      for (const auto &field :
           txn_->db()->meta_info().GetVertexVectorFields(lids, pid)) {
        uint32_t lid = native_to_big(field->label_id());
        std::string key = VertexVectorPropertyKey(lid, pid, id_);
        auto s = txn_->dbtxn()->GetWriteBatch()->Put(
            txn_->db()->graph_cf().vertex_vector_property, key,
            SerializeVector(vector));
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
    }
    return;
  }
  auto props = LoadVertexSerializedProperties(txn_, id_, required_pids);
  auto vector_props =
      LoadVertexVectorProperties(txn_, id_, lids, required_pids);
  auto updated_props = props;
  auto updated_vector_props = vector_props;
  for (const auto &[pid, pval] : serialized) {
    updated_props[pid] = pval;
  }
  for (const auto &[pid, vector] : vector_properties) {
    updated_vector_props[pid] = vector;
    updated_props.erase(pid);
  }
  UpdateVertexIndexes(txn_, id_, lids, lids, props, updated_props, vector_props,
                      updated_vector_props, pids);
  for (auto &[pid, pval] : serialized) {
    std::string pkey = VertexPropertyKey(id_, pid);
    auto s = txn_->dbtxn()->GetWriteBatch()->Put(
        txn_->db()->graph_cf().vertex_property, pkey, pval);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  for (const auto &[pid, vector] : vector_properties) {
    for (const auto &field :
         txn_->db()->meta_info().GetVertexVectorFields(lids, pid)) {
      uint32_t lid = native_to_big(field->label_id());
      std::string key = VertexVectorPropertyKey(lid, pid, id_);
      auto s = txn_->dbtxn()->GetWriteBatch()->Put(
          txn_->db()->graph_cf().vertex_vector_property, key,
          SerializeVector(vector));
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
  }
}

void Vertex::RemoveAllProperty() {
  Lock();
  auto lids = GetLabelIds();
  std::string prefix(AsChars(id_), sizeof(id_));
  rocksdb::ReadOptions ro;
  std::unordered_set<uint32_t> pids;
  std::unordered_map<uint32_t, std::string> props;
  VertexVectorProperties vector_props;
  std::vector<std::string> prop_keys;
  std::vector<std::string> vector_prop_keys;
  std::unique_ptr<rocksdb::Iterator> p_iter;
  p_iter.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().vertex_property));
  for (p_iter->Seek(prefix);
       p_iter->Valid() && p_iter->key().starts_with(prefix); p_iter->Next()) {
    auto key = p_iter->key();
    prop_keys.push_back(key.ToString());
    auto value = p_iter->value();
    key.remove_prefix(sizeof(int64_t));
    uint32_t pid = ReadValue<uint32_t>(key.data());
    props.emplace(pid, value.ToString());
    pids.insert(pid);
  }
  p_iter.reset();
  vector_props =
      LoadAllVertexVectorProperties(txn_, id_, lids, &pids, &vector_prop_keys);
  VertexSerializedProperties empty_properties;
  VertexVectorProperties empty_vector_properties;
  UpdateVertexIndexes(txn_, id_, lids, lids, props, empty_properties,
                      vector_props, empty_vector_properties, pids);
  for (auto &key : prop_keys) {
    auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().vertex_property, key);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  for (auto &key : vector_prop_keys) {
    auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().vertex_vector_property, key);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
}

void Vertex::RemoveProperty(const std::string &name) {
  auto optional = txn_->db()->id_generator().GetPid(name);
  if (!optional.has_value()) {
    return;
  }
  auto pid = optional.value();
  Lock();
  auto lids = GetLabelIds();
  auto vector_fields = txn_->db()->meta_info().GetVertexVectorFields(lids, pid);
  if (!vector_fields.empty()) {
    std::unordered_set<uint32_t> touched_pids{pid};
    auto props = LoadVertexSerializedProperties(txn_, id_, touched_pids);
    auto vector_props =
        LoadVertexVectorProperties(txn_, id_, lids, touched_pids);
    if (props.empty() && vector_props.empty()) {
      return;
    }
    VertexSerializedProperties empty_properties;
    VertexVectorProperties empty_vector_properties;
    UpdateVertexIndexes(txn_, id_, lids, lids, props, empty_properties,
                        vector_props, empty_vector_properties, touched_pids);
    for (const auto &field : vector_fields) {
      uint32_t lid = native_to_big(field->label_id());
      std::string key = VertexVectorPropertyKey(lid, pid, id_);
      auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
          txn_->db()->graph_cf().vertex_vector_property, key);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
    auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().vertex_property, VertexPropertyKey(id_, pid));
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    return;
  }
  std::string pkey = VertexPropertyKey(id_, pid);
  auto required_pids = CollectVertexIndexPropertyIds(txn_, lids, lids, {pid});
  if (required_pids.empty()) {
    rocksdb::ReadOptions ro;
    rocksdb::PinnableSlice pval;
    auto s = txn_->dbtxn()->Get(ro, txn_->db()->graph_cf().vertex_property,
                                pkey, &pval);
    if (s.IsNotFound()) {
      return;
    }
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().vertex_property, pkey);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    return;
  }
  auto props = LoadVertexSerializedProperties(txn_, id_, required_pids);
  if (!props.count(pid)) {
    return;
  }
  auto updated_props = props;
  updated_props.erase(pid);
  VertexVectorProperties empty_vector_properties;
  UpdateVertexIndexes(txn_, id_, lids, lids, props, updated_props,
                      empty_vector_properties, empty_vector_properties, {pid});
  auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
      txn_->db()->graph_cf().vertex_property, pkey);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void Edge::Delete() {
  Lock();
  // delete out edge key
  std::string key;
  key.append(AsChars(startId_), sizeof(startId_));
  key.append(1, static_cast<char>(EdgeDirection::OUTGOING));
  key.append(AsChars(typeId_), sizeof(typeId_));
  key.append(AsChars(endId_), sizeof(endId_));
  key.append(AsChars(id_), sizeof(id_));
  auto s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
      txn_->db()->graph_cf().graph_topology, key);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  key.clear();
  // delete in edge key
  key.append(AsChars(endId_), sizeof(endId_));
  key.append(1, static_cast<char>(EdgeDirection::INCOMING));
  key.append(AsChars(typeId_), sizeof(typeId_));
  key.append(AsChars(startId_), sizeof(startId_));
  key.append(AsChars(id_), sizeof(id_));
  s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
      txn_->db()->graph_cf().graph_topology, key);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  // delete type eid
  key.clear();
  key.append(AsChars(typeId_), sizeof(typeId_));
  key.append(AsChars(id_), sizeof(id_));
  s = txn_->dbtxn()->GetWriteBatch()->SingleDelete(
      txn_->db()->graph_cf().edge_type_eid, key);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  // delete edge properties
  std::unique_ptr<rocksdb::Iterator> ep_iter;
  rocksdb::ReadOptions ro;
  rocksdb::Slice ep_prefix(AsChars(id_), sizeof(id_));
  ep_iter.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().edge_property));
  for (ep_iter->Seek(ep_prefix);
       ep_iter->Valid() && ep_iter->key().starts_with(ep_prefix);
       ep_iter->Next()) {
    auto prop_key = ep_iter->key().ToString();  // must copy
    s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().edge_property, prop_key);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
}

std::string Edge::GetType() {
  auto id = GetTypeId();
  auto ret = txn_->db()->id_generator().GetEdgeTypeName(id);
  assert(ret.has_value());
  return ret.value();
}

Value Edge::GetProperty(uint32_t pid) {
  rocksdb::ReadOptions ro;
  rocksdb::PinnableSlice pinnable_val;
  Value ret;
  std::string pkey(AsChars(id_), sizeof(id_));
  pkey.append(AsChars(pid), sizeof(pid));
  auto s = txn_->dbtxn()->Get(ro, txn_->db()->graph_cf().edge_property, pkey,
                              &pinnable_val);
  if (s.ok()) {
    ret.Deserialize(pinnable_val.data(), pinnable_val.size());
  } else if (!s.IsNotFound()) {
    THROW_CODE(StorageEngineError, s.ToString());
  }
  return ret;
}

Vertex Edge::GetOtherEnd(int64_t vid) const {
  if (startId_ == vid) {
    return GetEnd();
  } else {
    if (vid != endId_) {
      THROW_CODE(
          UnknownError, "GetOtherEnd error, startId:{}, endId:{}, vid:{}",
          big_to_native(startId_), big_to_native(endId_), big_to_native(vid));
    }
    return GetStart();
  }
}

Value Edge::GetProperty(const std::string &name) {
  auto optional = txn_->db()->id_generator().GetPid(name);
  if (!optional.has_value()) {
    return {};
  }
  return GetProperty(optional.value());
}

std::unordered_map<std::string, Value> Edge::GetAllProperty() {
  std::string prefix(AsChars(id_), sizeof(id_));
  rocksdb::ReadOptions ro;
  std::unordered_map<std::string, Value> ret;
  std::unique_ptr<rocksdb::Iterator> p_iter;
  p_iter.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().edge_property));
  for (p_iter->Seek(prefix);
       p_iter->Valid() && p_iter->key().starts_with(prefix); p_iter->Next()) {
    auto key = p_iter->key();
    auto value = p_iter->value();
    key.remove_prefix(sizeof(int64_t));
    uint32_t pid = ReadValue<uint32_t>(key.data());
    auto optional = txn_->db()->id_generator().GetPropertyName(pid);
    if (optional.has_value()) {
      Value v;
      v.Deserialize(value.data(), value.size());
      ret.emplace(std::move(optional.value()), std::move(v));
    }
  }
  return ret;
}

void Edge::SetProperties(
    const std::unordered_map<std::string, Value> &properties) {
  if (properties.empty()) {
    return;
  }
  Lock();
  for (auto &[name, value] : properties) {
    auto pid = txn_->db()->id_generator().GetOrCreatePid(name);
    std::string pkey(AsChars(id_), sizeof(id_));
    pkey.append(AsChars(pid), sizeof(pid));
    auto s = txn_->dbtxn()->GetWriteBatch()->Put(
        txn_->db()->graph_cf().edge_property, pkey, value.Serialize());
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
}

void Edge::RemoveProperty(const std::string &name) {
  auto optional = txn_->db()->id_generator().GetPid(name);
  if (!optional.has_value()) {
    return;
  }
  Lock();
  auto pid = optional.value();
  std::string pkey(AsChars(id_), sizeof(id_));
  pkey.append(AsChars(pid), sizeof(pid));
  auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
      txn_->db()->graph_cf().edge_property, pkey);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void Edge::RemoveAllProperty() {
  Lock();
  std::vector<std::string> prop_keys;
  std::string prefix(AsChars(id_), sizeof(id_));
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> p_iter;
  p_iter.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().edge_property));
  for (p_iter->Seek(prefix);
       p_iter->Valid() && p_iter->key().starts_with(prefix); p_iter->Next()) {
    auto key = p_iter->key();
    prop_keys.push_back(key.ToString());
  }
  p_iter.reset();
  for (auto &key : prop_keys) {
    auto s = txn_->dbtxn()->GetWriteBatch()->Delete(
        txn_->db()->graph_cf().edge_property, key);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
}

void Edge::Lock() {
  rocksdb::ReadOptions ro;
  // Use a prefixed synthetic per-edge key as the transaction lock point so
  // edge locks do not share the same key namespace as vertex records.
  auto edge_lock_key = BuildEdgeLockKey(id_);
  // In TransactionDB, GetForUpdate() with a null value buffer still acquires
  // the lock even when the key does not exist, so edge mutations can serialize
  // on eid without materializing an extra record in graph_topology.
  auto s = txn_->dbtxn()->GetForUpdate(
      ro, txn_->db()->graph_cf().graph_topology, rocksdb::Slice(edge_lock_key),
      static_cast<std::string *>(nullptr));
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}
}  // namespace graphdb
