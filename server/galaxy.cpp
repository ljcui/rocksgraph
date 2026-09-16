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

#include "galaxy.h"

#include <boost/endian/conversion.hpp>
#include <filesystem>
#include <utility>

#include "common/exceptions.h"
#include "common/logger.h"
using namespace graphdb;
using namespace boost::endian;
namespace server {
namespace {

constexpr std::string_view kSystemGraphName = "system";

std::string BuildGraphMetaKey(uint64_t graph_id) {
  std::string key;
  key.append(1, static_cast<char>(GalaxyMetaDataType::GraphDB));
  native_to_big_inplace(graph_id);
  key.append((const char *)&graph_id, sizeof(graph_id));
  return key;
}

std::string BuildGalaxyMetaKey(GalaxyMetaDataType type) {
  return std::string(1, static_cast<char>(type));
}

void ValidateRaftNodeInfos(const meta::RaftNodeInfos &node_infos,
                           std::string_view graph_name) {
  if (node_infos.nodes().empty()) {
    THROW_CODE(InvalidParameter, "raft node infos should not be empty");
  }
  for (const auto &[node_id, node_info] : node_infos.nodes()) {
    if (node_id == 0) {
      THROW_CODE(InvalidParameter, "raft node_id should be greater than 0");
    }
    if (node_info.node_id() != node_id) {
      THROW_CODE(InvalidParameter,
                 "raft node info key [{}] does not match node_id [{}]", node_id,
                 node_info.node_id());
    }
    if (node_info.ip().empty()) {
      THROW_CODE(InvalidParameter, "raft node [{}] ip should not be empty",
                 node_id);
    }
    if (node_info.bolt_port() <= 0) {
      THROW_CODE(InvalidParameter,
                 "raft node [{}] bolt_port should be greater than 0", node_id);
    }
    if (node_info.raft_poft() <= 0) {
      THROW_CODE(InvalidParameter,
                 "raft node [{}] raft_port should be greater than 0", node_id);
    }
    if (node_info.graph().empty()) {
      THROW_CODE(InvalidParameter, "raft node [{}] graph should not be empty",
                 node_id);
    }
    if (!graph_name.empty() && node_info.graph() != graph_name) {
      THROW_CODE(InvalidParameter,
                 "raft node [{}] graph [{}] does not match graph [{}]", node_id,
                 node_info.graph(), graph_name);
    }
  }
}

void ValidateGraphNameForCreate(std::string_view name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "graph name should not be empty");
  }
  if (name == kSystemGraphName) {
    THROW_CODE(InvalidParameter, "graph name [{}] is reserved", name);
  }
}

void ApplyRaftRequest(GraphDB *graph_db, uint64_t index,
                      const meta::RaftRequest &request) {
  graph_db->ApplyRaftRequest(index, request);
}

raft::LocalNodeConfig BuildLocalNodeConfig(
    const std::string &graph_name, const LocalNodeOptions &local_node_options) {
  raft::LocalNodeConfig local_node;
  local_node.graph = graph_name;
  local_node.ip = local_node_options.host;
  local_node.bolt_port = static_cast<int32_t>(local_node_options.bolt_port);
  local_node.raft_poft = static_cast<int32_t>(local_node_options.raft_port);
  return local_node;
}

raft::RaftLogStoreConfig BuildRaftLogStoreConfig(
    const std::string &path,
    std::shared_ptr<rocksdb::Cache> shared_block_cache) {
  raft::RaftLogStoreConfig store_config;
  store_config.path = path;
  store_config.shared_block_cache = std::move(shared_block_cache);
  store_config.total_threads = 2;
  store_config.keep_logs = 100000;
  store_config.gc_interval = 1;
  return store_config;
}

raft::RaftConfig BuildRaftConfig() {
  raft::RaftConfig raft_config;
  raft_config.tick_interval = 100;
  raft_config.election_tick = 10;
  raft_config.heartbeat_tick = 1;
  return raft_config;
}

std::vector<eraft::Peer> BuildInitPeers(const meta::RaftNodeInfos &node_infos) {
  std::vector<eraft::Peer> init_peers;
  init_peers.reserve(node_infos.nodes_size());
  for (const auto &[node_id, node_info] : node_infos.nodes()) {
    eraft::Peer peer;
    peer.id_ = node_id;
    peer.context_ = node_info.SerializeAsString();
    init_peers.emplace_back(std::move(peer));
  }
  return init_peers;
}

}  // namespace

Galaxy::~Galaxy() {
  graphs_.clear();
  auto s = meta_db_->Close();
  if (!s.ok()) {
    LOG_WARN("meta db close error : {}", s.ToString());
  }
  delete meta_db_;
  meta_db_ = nullptr;
  LOG_INFO("Close galaxy");
}

std::unique_ptr<Galaxy> Galaxy::Open(const std::string &path,
                                     const GalaxyOptions &galaxy_options,
                                     LocalNodeOptions local_node_options) {
  LOG_INFO("Open galaxy: {}", path);
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;
  rocksdb::TransactionDBOptions txn_db_options;

  std::string meta_path = path + "/meta";
  std::filesystem::create_directories(meta_path);
  rocksdb::TransactionDB *db = nullptr;
  auto s =
      rocksdb::TransactionDB::Open(options, txn_db_options, meta_path, &db);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  auto galaxy = std::make_unique<Galaxy>();
  galaxy->path_ = path;
  raft::RaftManager::Configure(galaxy_options.raft_scheduler_shards);
  galaxy->block_cache_ = rocksdb::NewLRUCache(galaxy_options.block_cache_size);
  galaxy->options_ = galaxy_options;
  galaxy->local_node_options_ = std::move(local_node_options);
  galaxy->meta_db_ = db;
  galaxy->raft_log_block_cache_ =
      rocksdb::NewLRUCache(galaxy_options.raft_log_block_cache_size);
  galaxy->assistant_pool_ = std::make_shared<graphdb::AssistantPool>(
      galaxy_options.assistant_thread_num);

  rocksdb::ReadOptions ro;
  {
    std::string next_graph_id_key(
        1, static_cast<char>(GalaxyMetaDataType::NextGraphID));
    std::string val;
    s = galaxy->meta_db_->Get(ro, next_graph_id_key, &val);
    if (s.ok()) {
      galaxy->next_graph_id_ = *(uint64_t *)val.data();
    } else if (s.IsNotFound()) {
      galaxy->next_graph_id_ = 1;
    } else {
      THROW_CODE(StorageEngineError, s.ToString());
    }
  }
  std::unique_ptr<rocksdb::Iterator> iter(galaxy->meta_db_->NewIterator(ro));
  std::string prefix;
  prefix.append(1, static_cast<char>(GalaxyMetaDataType::GraphDB));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto val = iter->value();
    meta::GraphDBMetaInfo meta;
    bool f = meta.ParseFromString(val.ToString());
    assert(f);
    LOG_INFO("Load GraphDB: [{}]", meta.ShortDebugString());
    std::string graph_path =
        galaxy->path_ + "/graph" + std::to_string(meta.graph_id());
    auto graph_db = GraphDB::Open(
        graph_path,
        {.block_cache = galaxy->block_cache_,
         .assistant_pool = galaxy->assistant_pool_,
         .ft_apply_interval_ = galaxy->options_.ft_apply_interval,
         .ft_writer_threads_ = galaxy->options_.ft_writer_threads,
         .ft_writer_memory_budget_ = galaxy->options_.ft_writer_memory_budget,
         .vt_apply_interval_ = galaxy->options_.vt_apply_interval});
    graph_db->db_meta() = meta;
    if (meta.enable_raft()) {
      galaxy->StartGraphRaft(graph_db.get(), nullptr);
    }
    galaxy->graphs_.emplace(meta.graph_name(), std::move(graph_db));
  }
  if (galaxy->graphs_.empty()) {
    galaxy->CreateGraph("default");
  }
  return galaxy;
}

std::shared_ptr<GraphDB> Galaxy::OpenGraph(const std::string &name) {
  std::shared_lock<std::shared_mutex> read_lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  if (iter != graphs_.end()) {
    return iter->second;
  } else {
    THROW_CODE(NoSuchGraph, "No such graph: {}", name);
  }
}

GraphDB *Galaxy::CreateGraph(const std::string &name) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  return CreateGraphInternal(name, nullptr);
}

GraphDB *Galaxy::CreateGraphWithRaft(const std::string &name,
                                     const meta::RaftNodeInfos &node_infos) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  ValidateGraphNameForCreate(name);
  ValidateRaftNodeInfos(node_infos, name);
  return CreateGraphInternal(name, &node_infos);
}

GraphDB *Galaxy::CreateGraphInternal(const std::string &name,
                                     const meta::RaftNodeInfos *node_infos) {
  ValidateGraphNameForCreate(name);
  meta::GraphDBMetaInfo meta;
  uint64_t graph_id = next_graph_id_.load();
  meta.set_graph_id(graph_id);
  meta.set_graph_name(name);
  meta.set_enable_raft(node_infos != nullptr);
  return CreateGraphWithId(meta, node_infos);
}

GraphDB *Galaxy::CreateGraphWithId(const meta::GraphDBMetaInfo &meta,
                                   const meta::RaftNodeInfos *node_infos) {
  std::unique_lock<std::shared_mutex> write_lock(graphs_mutex_);
  auto iter = graphs_.find(meta.graph_name());
  if (iter != graphs_.end()) {
    THROW_CODE(GraphAlreadyExists, "The graph already exists: {}",
               meta.graph_name());
  }

  if (node_infos != nullptr) {
    ValidateRaftNodeInfos(*node_infos, meta.graph_name());
  }

  uint64_t next = static_cast<uint64_t>(meta.graph_id()) + 1;
  std::string graph_path = path_ + "/graph" + std::to_string(meta.graph_id());
  auto graph_db = GraphDB::Open(
      graph_path, {.block_cache = block_cache_,
                   .assistant_pool = assistant_pool_,
                   .ft_apply_interval_ = options_.ft_apply_interval,
                   .ft_writer_threads_ = options_.ft_writer_threads,
                   .ft_writer_memory_budget_ = options_.ft_writer_memory_budget,
                   .vt_apply_interval_ = options_.vt_apply_interval});
  graph_db->db_meta() = meta;
  if (node_infos) {
    StartGraphRaft(graph_db.get(), node_infos);
  }
  rocksdb::WriteBatch wb;
  wb.Put(BuildGraphMetaKey(meta.graph_id()), meta.SerializeAsString());
  wb.Put(BuildGalaxyMetaKey(GalaxyMetaDataType::NextGraphID),
         std::string((const char *)&next, sizeof(next)));
  auto s = meta_db_->Write({}, {}, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  uint64_t current_next = next_graph_id_.load();
  while (current_next < next &&
         !next_graph_id_.compare_exchange_weak(current_next, next)) {
  }
  LOG_INFO("Create graph:{}, path:{}", meta.graph_name(), graph_path);
  graphs_.emplace(meta.graph_name(), std::move(graph_db));
  return graphs_[meta.graph_name()].get();
}

void Galaxy::StartGraphRaft(GraphDB *graph_db,
                            const meta::RaftNodeInfos *node_infos) {
  auto local_node = BuildLocalNodeConfig(graph_db->db_meta().graph_name(),
                                         local_node_options_);
  auto store_config = BuildRaftLogStoreConfig(graph_db->path() + "/raft",
                                              raft_log_block_cache_);
  auto raft_config = BuildRaftConfig();
  auto *graph_db_ptr = graph_db;
  auto apply_id = graph_db->GetRaftApplyIndex();

  std::unique_ptr<raft::RaftDriver> raft_driver;
  if (node_infos == nullptr) {
    raft_driver = std::make_unique<raft::RaftDriver>(
        [graph_db_ptr](uint64_t index, const meta::RaftRequest &request) {
          ApplyRaftRequest(graph_db_ptr, index, request);
        },
        apply_id, std::move(local_node), store_config, raft_config);
  } else {
    auto init_peers = BuildInitPeers(*node_infos);
    raft_driver = std::make_unique<raft::RaftDriver>(
        [graph_db_ptr](uint64_t index, const meta::RaftRequest &request) {
          ApplyRaftRequest(graph_db_ptr, index, request);
        },
        apply_id, std::move(local_node), std::move(init_peers), store_config,
        raft_config);
  }

  auto err = raft_driver->Run();
  if (err != nullptr) {
    THROW_CODE(StorageEngineError,
               "failed to run raft driver for graph [{}]: {}",
               graph_db->db_meta().graph_name(), err.String());
  }
  graph_db->SetRaftDriver(std::move(raft_driver));
}

GraphDB *Galaxy::ClearGraph(const std::string &name) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  std::unique_lock<std::shared_mutex> write_lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  if (iter == graphs_.end()) {
    THROW_CODE(NoSuchGraph, "No such graph: {}", name);
  }
  LOG_INFO("Clear graph:{}, path:{}", name, iter->second->path());
  if (iter->second->db_meta().enable_raft()) {
    iter->second->ClearDataInternal();
  } else {
    iter->second->ClearData();
  }
  return iter->second.get();
}

void Galaxy::DeleteGraph(const std::string &name) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  std::unique_lock<std::shared_mutex> write_lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  if (iter == graphs_.end()) {
    THROW_CODE(NoSuchGraph, "No such graph: {}", name);
  }

  if (iter->second->db_meta().enable_raft()) {
    iter->second->StopRaft();
  }

  rocksdb::WriteOptions wo;
  uint64_t graph_id = iter->second->db_meta().graph_id();
  auto s = meta_db_->Delete(wo, BuildGraphMetaKey(graph_id));
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  iter->second->drop_on_close() = true;
  std::string graph_path = iter->second->path();
  LOG_INFO("Erase graph:{}, path:{}", name, graph_path);
  graphs_.erase(iter);
}

}  // namespace server
