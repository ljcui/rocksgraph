/**
 * Copyright 2026 AntGroup CO., Ltd.
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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "etcd_raft/raft.h"
#include "graphdb/graph_db.h"
#include "graphdb/transaction.h"
#include "graphdb/vertex_iterator.h"
#include "proto/graph_replication.pb.h"
#include "proto/graph_storage.pb.h"
#include "raft_driver/raft_driver.h"
#include "runtime/query_executor.h"
#include "server/graph_server.h"
#include "test_util.h"
#include "value/value.h"

using namespace graphdb;
using rg::Value;
namespace fs = std::filesystem;

namespace {

constexpr char kGraphName[] = "raft_cluster_graph";

const std::unordered_map<std::string, Value> kProperties = {
    {"property1", Value(true)},
    {"property2", Value(100)},
    {"property3", Value("string")},
    {"property4", Value(1.1314)},
    {"property5", Value(Value::List{Value(true), Value(false)})},
    {"property6", Value(Value::List{Value(1), Value(2), Value(3)})},
    {"property7", Value(Value::List{Value("string1"), Value("string2")})},
    {"property8", Value(Value::List{Value(11.11), Value(22.22)})}};

bool WaitUntil(
    const std::function<bool()>& condition, std::chrono::milliseconds timeout,
    std::chrono::milliseconds interval = std::chrono::milliseconds(10)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(interval);
  }
  return condition();
}

class TestServerCluster;

bool WaitForAllPropertyIndexesReady(
    const TestServerCluster& cluster, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10));

int32_t AllocateFreePort() {
  boost::asio::io_service service;
  boost::asio::ip::tcp::acceptor acceptor(
      service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
  return static_cast<int32_t>(acceptor.local_endpoint().port());
}

struct TestServerConfig {
  uint64_t node_id = 0;
  int32_t bolt_port = 0;
  int32_t raft_port = 0;
  std::string data_path;
};

class TestServerCluster final {
 public:
  explicit TestServerCluster(std::string base_path, size_t node_count = 3,
                             uint64_t first_node_id = 1)
      : base_path_(std::move(base_path)),
        node_count_(node_count),
        first_node_id_(first_node_id) {}

  ~TestServerCluster() { Stop(); }

  void Start() {
    Stop();
    if (node_count_ == 0) {
      throw std::runtime_error("raft test cluster must have at least one node");
    }
    fs::remove_all(base_path_);
    server_configs_ = BuildServerConfigs();
    graph_node_infos_ = BuildNodeInfos(server_configs_, kGraphName);
    servers_.clear();
    servers_.resize(server_configs_.size());
    try {
      for (size_t i = 0; i < server_configs_.size(); ++i) {
        StartServer(i);
      }

      CreateRaftGraphOnAllServers(kGraphName, graph_node_infos_);
      if (!WaitForGraphCreated(std::chrono::seconds(15))) {
        throw std::runtime_error("failed to create graph on all servers: " +
                                 StatusSummary());
      }
    } catch (...) {
      Stop();
      throw;
    }
  }

  void Stop() {
    for (auto& server : servers_) {
      if (server != nullptr) {
        server->Stop();
      }
    }
    servers_.clear();
    fs::remove_all(base_path_);
  }

  void StopServer(size_t index) {
    if (index >= servers_.size()) {
      throw std::out_of_range("raft test server index is out of range");
    }
    if (servers_[index] == nullptr) {
      return;
    }
    servers_[index]->Stop();
    servers_[index].reset();
  }

  void StartServer(size_t index) {
    if (index >= server_configs_.size()) {
      throw std::out_of_range("raft test server index is out of range");
    }
    if (index < servers_.size() && servers_[index] != nullptr) {
      return;
    }
    if (servers_.size() < server_configs_.size()) {
      servers_.resize(server_configs_.size());
    }
    auto server = MakeServer(server_configs_[index]);
    if (!server->Start()) {
      throw std::runtime_error("failed to start lgraph server");
    }
    servers_[index] = std::move(server);
  }

  void CreateRaftGraphOnAllServers(const std::string& graph_name,
                                   const meta::RaftNodeInfos& node_infos) {
    for (const auto& server : servers_) {
      if (server == nullptr) {
        continue;
      }
      server->graph_manager()->CreateGraphWithRaft(graph_name, node_infos);
    }
  }

  server::GraphServer* WaitForLeader(
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitForLeader(kGraphName, timeout);
  }

  server::GraphServer* WaitForLeader(
      const std::string& graph_name,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    auto leader_index = WaitForLeaderIndex(graph_name, timeout);
    if (!leader_index.has_value()) {
      return nullptr;
    }
    return servers_[*leader_index].get();
  }

  std::optional<size_t> WaitForLeaderIndex(
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitForLeaderIndex(kGraphName, timeout);
  }

  std::optional<size_t> WaitForLeaderIndex(
      const std::string& graph_name,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    std::optional<size_t> leader_index;
    if (!WaitUntil(
            [this, &graph_name, &leader_index]() {
              leader_index = FindConsistentLeaderIndex(graph_name);
              return leader_index.has_value();
            },
            timeout)) {
      return std::nullopt;
    }
    return leader_index;
  }

  bool WaitForGraphCreated(
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitForGraphCreated(kGraphName, timeout);
  }

  bool WaitForGraphCreated(
      const std::string& graph_name,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitUntil(
        [this, &graph_name]() {
          return std::all_of(servers_.begin(), servers_.end(),
                             [&graph_name](const auto& server) {
                               if (server == nullptr) {
                                 return true;
                               }
                               try {
                                 server->graph_manager()->OpenGraph(graph_name);
                                 return true;
                               } catch (const std::exception&) {
                                 return false;
                               }
                             });
        },
        timeout);
  }

  bool WaitForGraphDeleted(
      const std::string& graph_name,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitUntil(
        [this, &graph_name]() {
          return std::all_of(servers_.begin(), servers_.end(),
                             [&graph_name](const auto& server) {
                               if (server == nullptr) {
                                 return true;
                               }
                               try {
                                 server->graph_manager()->OpenGraph(graph_name);
                                 return false;
                               } catch (const std::exception&) {
                                 return true;
                               }
                             });
        },
        timeout);
  }

  bool WaitForGraphVertexCount(
      const std::string& graph_name, size_t expected_count,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitUntil(
        [this, &graph_name, expected_count]() {
          return std::all_of(
              servers_.begin(), servers_.end(),
              [&graph_name, expected_count](const auto& server) {
                if (server == nullptr) {
                  return true;
                }
                auto graph = server->graph_manager()->OpenGraph(graph_name);
                auto txn = graph->BeginTransaction();
                size_t vertex_count = 0;
                for (auto iter = txn->NewVertexIterator(); iter->Valid();
                     iter->Next()) {
                  ++vertex_count;
                }
                txn->Commit();
                return vertex_count == expected_count;
              });
        },
        timeout);
  }

  size_t VertexCount(size_t server_index,
                     const std::string& graph_name = kGraphName) const {
    auto* target = server(server_index);
    if (target == nullptr) {
      throw std::runtime_error("raft test server is not running");
    }
    auto graph = target->graph_manager()->OpenGraph(graph_name);
    auto txn = graph->BeginTransaction();
    size_t vertex_count = 0;
    for (auto iter = txn->NewVertexIterator(); iter->Valid(); iter->Next()) {
      ++vertex_count;
    }
    txn->Commit();
    return vertex_count;
  }

  bool WaitForApplyIndex(
      uint64_t apply_index,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitForApplyIndex(kGraphName, apply_index, timeout);
  }

  bool WaitForApplyIndex(
      const std::string& graph_name, uint64_t apply_index,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    return WaitUntil(
        [this, &graph_name, apply_index]() {
          return std::all_of(
              servers_.begin(), servers_.end(),
              [this, &graph_name, apply_index](const auto& server) {
                if (server == nullptr) {
                  return true;
                }
                return OpenGraph(server.get(), graph_name)
                           ->GetRaftApplyIndex() >= apply_index;
              });
        },
        timeout);
  }

  std::vector<server::GraphServer*> servers() const {
    std::vector<server::GraphServer*> ret;
    ret.reserve(servers_.size());
    for (const auto& server : servers_) {
      if (server != nullptr) {
        ret.push_back(server.get());
      }
    }
    return ret;
  }

  server::GraphServer* server(size_t index) const {
    if (index >= servers_.size() || servers_[index] == nullptr) {
      return nullptr;
    }
    return servers_[index].get();
  }

  size_t size() const { return servers_.size(); }

  uint64_t node_id(size_t index) const {
    if (index >= server_configs_.size()) {
      throw std::out_of_range("raft test server index is out of range");
    }
    return server_configs_[index].node_id;
  }

  meta::RaftNodeInfos NodeInfosForGraph(const std::string& graph_name) const {
    return BuildNodeInfos(server_configs_, graph_name);
  }

  std::string StatusSummary(const std::string& graph_name = kGraphName) const {
    std::ostringstream out;
    for (size_t i = 0; i < servers_.size(); ++i) {
      const auto& server = servers_[i];
      if (server == nullptr) {
        out << "[node_id=" << server_configs_[i].node_id << ", stopped]";
        continue;
      }
      auto graph = OpenGraph(server.get(), graph_name);
      auto status = graph->raft_driver()->GetRaftStatus();
      out << "[bolt_port=" << server->options().local_node_options.bolt_port
          << ", raft_port=" << server->options().local_node_options.raft_port
          << ", lead=" << status.s.basicStatus_.softState_.lead_ << ", state="
          << eraft::ToString(status.s.basicStatus_.softState_.raftState_)
          << ", apply=" << graph->GetRaftApplyIndex()
          << ", first_log=" << status.first_log
          << ", last_log=" << status.last_log << "]";
    }
    return out.str();
  }

 private:
  std::shared_ptr<GraphDB> OpenGraph(
      const server::GraphServer* server,
      const std::string& graph_name = kGraphName) const {
    return server->graph_manager()->OpenGraph(graph_name);
  }

  std::unique_ptr<server::GraphServer> MakeServer(
      const TestServerConfig& config) const {
    server::GraphServerOptions options;
    options.data_path = config.data_path;
    options.local_node_options.host = "127.0.0.1";
    options.local_node_options.bolt_port = config.bolt_port;
    options.bolt_io_thread_num = 1;
    options.local_node_options.raft_port = config.raft_port;
    options.local_node_options.raft_node_id = config.node_id;
    return std::make_unique<server::GraphServer>(std::move(options));
  }

  std::vector<TestServerConfig> BuildServerConfigs() const {
    std::vector<TestServerConfig> configs;
    configs.reserve(node_count_);
    std::unordered_set<int32_t> used_ports;
    for (size_t i = 0; i < node_count_; ++i) {
      TestServerConfig config;
      config.node_id = first_node_id_ + static_cast<uint64_t>(i);
      do {
        config.bolt_port = AllocateFreePort();
      } while (!used_ports.insert(config.bolt_port).second);
      do {
        config.raft_port = AllocateFreePort();
      } while (!used_ports.insert(config.raft_port).second);
      config.data_path = base_path_ + "/node" + std::to_string(i + 1);
      configs.emplace_back(std::move(config));
    }
    return configs;
  }

  meta::RaftNodeInfos BuildNodeInfos(
      const std::vector<TestServerConfig>& server_configs,
      const std::string& graph_name) const {
    meta::RaftNodeInfos node_infos;
    for (const auto& config : server_configs) {
      meta::RaftNodeInfo node_info;
      node_info.set_node_id(config.node_id);
      node_info.set_ip("127.0.0.1");
      node_info.set_bolt_port(config.bolt_port);
      node_info.set_raft_poft(config.raft_port);
      node_info.set_graph(graph_name);
      (*node_infos.mutable_nodes())[config.node_id] = node_info;
    }
    return node_infos;
  }

  std::optional<size_t> FindConsistentLeaderIndex(
      const std::string& graph_name) const {
    std::optional<size_t> leader_index;
    uint64_t leader_id = 0;
    bool has_running_server = false;

    for (size_t i = 0; i < servers_.size(); ++i) {
      if (servers_[i] == nullptr) {
        continue;
      }
      has_running_server = true;
      auto graph = OpenGraph(servers_[i].get(), graph_name);
      auto status = graph->raft_driver()->GetRaftStatus();
      auto reported_leader = status.s.basicStatus_.softState_.lead_;
      if (reported_leader == 0) {
        return std::nullopt;
      }
      if (leader_id == 0) {
        leader_id = reported_leader;
      } else if (leader_id != reported_leader) {
        return std::nullopt;
      }
      if (status.s.basicStatus_.softState_.raftState_ == eraft::StateLeader) {
        if (leader_index.has_value()) {
          return std::nullopt;
        }
        leader_index = i;
      }
    }

    if (!has_running_server) {
      return std::nullopt;
    }
    if (!leader_index.has_value()) {
      return std::nullopt;
    }
    if (server_configs_[*leader_index].node_id != leader_id) {
      return std::nullopt;
    }
    return leader_index;
  }

  std::string base_path_;
  size_t node_count_;
  uint64_t first_node_id_;
  std::vector<TestServerConfig> server_configs_;
  meta::RaftNodeInfos graph_node_infos_;
  std::vector<std::unique_ptr<server::GraphServer>> servers_;
};

raft::LocalNodeConfig MakeLocalNodeConfig(const std::string& graph_name) {
  raft::LocalNodeConfig local_node;
  local_node.graph = graph_name;
  local_node.ip = "127.0.0.1";
  local_node.bolt_port = AllocateFreePort();
  local_node.raft_poft = AllocateFreePort();
  return local_node;
}

raft::RaftLogStoreConfig MakeRaftLogStoreConfig(const std::string& path) {
  raft::RaftLogStoreConfig store_config;
  store_config.path = path;
  store_config.shared_block_cache = rocksdb::NewLRUCache(64 * 1024 * 1024L);
  store_config.total_threads = 2;
  store_config.keep_logs = 100000;
  store_config.gc_interval = 1;
  return store_config;
}

raft::RaftConfig MakeRaftConfig() {
  raft::RaftConfig raft_config;
  raft_config.tick_interval = 100;
  raft_config.election_tick = 10;
  raft_config.heartbeat_tick = 1;
  return raft_config;
}

meta::RaftNodeInfo MakeNodeInfo(const raft::LocalNodeConfig& local_node,
                                uint64_t node_id) {
  meta::RaftNodeInfo node_info;
  node_info.set_node_id(node_id);
  node_info.set_graph(local_node.graph);
  node_info.set_ip(local_node.ip);
  node_info.set_bolt_port(local_node.bolt_port);
  node_info.set_raft_poft(local_node.raft_poft);
  return node_info;
}

std::vector<eraft::Peer> MakeInitPeers(const raft::LocalNodeConfig& local_node,
                                       uint64_t node_id = 1) {
  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = node_id;
  peer.context_ = MakeNodeInfo(local_node, node_id).SerializeAsString();
  init_peers.emplace_back(std::move(peer));
  return init_peers;
}

class TestRaftStateMachine {
 public:
  void SetAppliedIndex(uint64_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    applied_index_ = index;
  }

  void ApplyConfChange(uint64_t index, const raftpb::ConfState& conf_state,
                       const meta::RaftNodeInfos& node_infos) {
    std::lock_guard<std::mutex> lock(mutex_);
    conf_state_ = conf_state;
    node_infos_ = node_infos;
    applied_index_ = index;
  }

  raft::RaftDriver::ApplyConfChange ConfChangeCallback() {
    return [this](uint64_t index, const raftpb::ConfState& conf_state,
                  const meta::RaftNodeInfos& node_infos) {
      ApplyConfChange(index, conf_state, node_infos);
    };
  }

  uint64_t AppliedIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return applied_index_;
  }

  std::optional<raftpb::ConfState> ConfState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conf_state_;
  }

  std::optional<meta::RaftNodeInfos> NodeInfos() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return node_infos_;
  }

 private:
  mutable std::mutex mutex_;
  uint64_t applied_index_ = 0;
  std::optional<raftpb::ConfState> conf_state_;
  std::optional<meta::RaftNodeInfos> node_infos_;
};

bool WaitForRaftDriverLeader(
    raft::RaftDriver* driver, uint64_t expected_leader_id = 1,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  return WaitUntil(
      [driver, expected_leader_id]() {
        auto status = driver->GetRaftStatus();
        return status.s.basicStatus_.softState_.lead_ == expected_leader_id &&
               status.s.basicStatus_.softState_.raftState_ ==
                   eraft::StateLeader;
      },
      timeout);
}

meta::RaftRequest MakeGraphWriteRequest(const std::string& key,
                                        const std::string& value) {
  rocksdb::WriteBatch wb;
  auto status = wb.Put(key, value);
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }

  meta::RaftRequest request;
  request.set_wb_kind(meta::WriteBatchKind::GRAPH_WRITE);
  request.set_wb_data(wb.Data());
  return request;
}

raft::PromiseContext::ApplyResult WaitForAppliedResult(
    const std::shared_ptr<raft::PromiseContext>& context,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto future = context->applied.get_future();
  if (future.wait_for(timeout) != std::future_status::ready) {
    return {eraft::Error("raft proposal did not apply before timeout"), 0};
  }
  return future.get();
}

struct TestRaftLogStorage {
  explicit TestRaftLogStorage(std::unique_ptr<raft::RaftLogStorage> storage)
      : storage(std::move(storage)) {}

  TestRaftLogStorage(TestRaftLogStorage&&) = default;
  TestRaftLogStorage& operator=(TestRaftLogStorage&&) = default;

  ~TestRaftLogStorage() { Close(); }

  raft::RaftLogStorage* operator->() const { return storage.get(); }

  void Close() {
    if (storage != nullptr) {
      storage->Close();
      storage.reset();
    }
  }

  std::unique_ptr<raft::RaftLogStorage> storage;
};

TestRaftLogStorage OpenRaftLogStorage(const std::string& path) {
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;

  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, options);
  cfs.emplace_back("meta", options);

  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
  std::unique_ptr<rocksdb::DB> db;
  fs::create_directories(path);
  auto status = rocksdb::DB::Open(options, path, cfs, &cf_handles, &db);
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }
  if (cf_handles.size() != 2) {
    throw std::runtime_error("unexpected raft log storage column families");
  }
  return TestRaftLogStorage(std::make_unique<raft::RaftLogStorage>(
      db.release(), cf_handles[0], cf_handles[1]));
}

raftpb::Entry MakeLogEntry(uint64_t index, uint64_t term,
                           std::string data = {}) {
  raftpb::Entry entry;
  entry.set_index(index);
  entry.set_term(term);
  entry.set_data(std::move(data));
  return entry;
}

size_t FirstFollowerIndex(const TestServerCluster& cluster,
                          size_t leader_index) {
  for (size_t i = 0; i < cluster.size(); ++i) {
    if (i != leader_index && cluster.server(i) != nullptr) {
      return i;
    }
  }
  throw std::runtime_error("raft test cluster has no running follower");
}

std::optional<size_t> WaitForLeaderIndexExcluding(
    const TestServerCluster& cluster,
    const std::unordered_set<size_t>& excluded,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  std::optional<size_t> leader_index;
  if (!WaitUntil(
          [&cluster, &excluded, &leader_index]() {
            leader_index.reset();
            uint64_t leader_id = 0;
            bool has_running_server = false;

            for (size_t i = 0; i < cluster.size(); ++i) {
              if (excluded.count(i)) {
                continue;
              }
              auto* server = cluster.server(i);
              if (server == nullptr) {
                continue;
              }
              has_running_server = true;
              auto graph = server->graph_manager()->OpenGraph(kGraphName);
              auto status = graph->raft_driver()->GetRaftStatus();
              auto reported_leader = status.s.basicStatus_.softState_.lead_;
              if (reported_leader == 0) {
                return false;
              }
              if (leader_id == 0) {
                leader_id = reported_leader;
              } else if (leader_id != reported_leader) {
                return false;
              }
              if (status.s.basicStatus_.softState_.raftState_ ==
                  eraft::StateLeader) {
                if (leader_index.has_value()) {
                  return false;
                }
                leader_index = i;
              }
            }

            if (!has_running_server || !leader_index.has_value()) {
              return false;
            }
            return cluster.node_id(*leader_index) == leader_id;
          },
          timeout)) {
    return std::nullopt;
  }
  return leader_index;
}

bool WaitForAllPropertyIndexesReady(const TestServerCluster& cluster,
                                    const std::string& graph_name,
                                    const std::string& index_name,
                                    std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&cluster, &graph_name, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(graph_name);
          if (!graph->meta_info().GetReadyVertexPropertyIndex(index_name)) {
            auto index = graph->meta_info().GetVertexPropertyIndex(index_name);
            if (index && index->state() == meta::IndexBuildState::FAILED) {
              ADD_FAILURE() << "property index build failed: "
                            << index->meta().build_error();
            }
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllPropertyIndexesReady(const TestServerCluster& cluster,
                                    const std::string& index_name,
                                    std::chrono::milliseconds timeout) {
  return WaitForAllPropertyIndexesReady(cluster, kGraphName, index_name,
                                        timeout);
}

bool WaitForAllFullTextIndexDefinitions(
    const TestServerCluster& cluster, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          auto index = graph->meta_info().GetVertexFullTextIndex(index_name);
          if (!index || index->state() == meta::IndexBuildState::FAILED) {
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllFullTextIndexesReady(
    const TestServerCluster& cluster, const std::string& graph_name,
    const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &graph_name, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(graph_name);
          if (!graph->meta_info().GetReadyVertexFullTextIndex(index_name)) {
            auto index = graph->meta_info().GetVertexFullTextIndex(index_name);
            if (index && index->state() == meta::IndexBuildState::FAILED) {
              ADD_FAILURE() << "fulltext index build failed: "
                            << index->meta().build_error();
            }
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllVectorIndexDefinitions(
    const TestServerCluster& cluster, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          auto index = graph->meta_info().GetVertexVectorIndex(index_name);
          if (!index || index->state() == meta::IndexBuildState::FAILED) {
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllVectorIndexesReady(
    const TestServerCluster& cluster, const std::string& graph_name,
    const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &graph_name, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(graph_name);
          if (!graph->meta_info().GetReadyVertexVectorIndex(index_name)) {
            auto index = graph->meta_info().GetVertexVectorIndex(index_name);
            if (index && index->state() == meta::IndexBuildState::FAILED) {
              ADD_FAILURE() << "vector index build failed: "
                            << index->meta().build_error();
            }
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllPropertyIndexesDeleted(
    const TestServerCluster& cluster, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          if (graph->meta_info().GetVertexPropertyIndex(index_name)) {
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllFullTextIndexesDeleted(
    const TestServerCluster& cluster, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          if (graph->meta_info().GetVertexFullTextIndex(index_name)) {
            return false;
          }
        }
        return true;
      },
      timeout);
}

bool WaitForAllVectorIndexesDeleted(
    const TestServerCluster& cluster, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  return WaitUntil(
      [&cluster, &index_name]() {
        for (auto* server : cluster.servers()) {
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          if (graph->meta_info().GetVertexVectorIndex(index_name)) {
            return false;
          }
        }
        return true;
      },
      timeout);
}

size_t CountPropertyIndexResults(GraphDB* graph, const std::string& index_name,
                                 const Value& query) {
  auto txn = graph->BeginTransaction();
  auto result = txn->QueryVertexByPropertyIndex(index_name, query);
  size_t count = 0;
  for (; result->Valid(); result->Next()) {
    ++count;
  }
  txn->Commit();
  return count;
}

uint64_t ExecuteCypherAndCommit(GraphDB* graph, const std::string& cypher) {
  auto txn = graph->BeginTransaction();
  try {
    (void)rg::ExecuteQuery(*txn, cypher);
    txn->Commit();
  } catch (...) {
    if (txn->GetState() == Transaction::State::kActive) {
      txn->Rollback();
    }
    throw;
  }
  return graph->GetRaftApplyIndex();
}

rg::QueryResult ExecuteSystemCypherAndCommit(
    server::GraphManager* graph_manager, const std::string& cypher,
    rg::QueryParameters parameters = {}) {
  rg::QueryOptions options;
  options.parameters = std::move(parameters);
  return rg::ExecuteSystemQuery(*graph_manager, cypher, std::move(options));
}

rg::Value RaftMembersValue(const meta::RaftNodeInfos& node_infos) {
  rg::Value::List members;
  members.reserve(node_infos.nodes().size());
  for (const auto& [node_id, node_info] : node_infos.nodes()) {
    members.emplace_back(rg::Value::Map{
        {"node_id", Value(static_cast<std::int64_t>(node_id))},
        {"ip", Value(node_info.ip())},
        {"bolt_port", Value(node_info.bolt_port())},
        {"raft_port", Value(node_info.raft_poft())},
        {"is_learner", Value(node_info.is_learner())},
        {"graph", Value(node_info.graph())},
    });
  }
  return rg::Value(std::move(members));
}

rg::Value RaftMemberValue(const meta::RaftNodeInfo& node_info) {
  return rg::Value(rg::Value::Map{
      {"node_id", Value(static_cast<std::int64_t>(node_info.node_id()))},
      {"ip", Value(node_info.ip())},
      {"bolt_port", Value(node_info.bolt_port())},
      {"raft_port", Value(node_info.raft_poft())},
  });
}

std::vector<std::string> CollectCypherStringColumn(GraphDB* graph,
                                                   const std::string& cypher) {
  auto txn = graph->BeginTransaction();
  std::vector<std::string> names;
  try {
    const rg::QueryResult result = rg::ExecuteQuery(*txn, cypher);
    for (const auto& row : result.rows) {
      if (row.size() != 1 || !row.front().IsString()) {
        throw std::runtime_error("expected a single string Cypher column");
      }
      names.push_back(row.front().AsString());
    }
    txn->Commit();
  } catch (...) {
    if (txn->GetState() == Transaction::State::kActive) {
      txn->Rollback();
    }
    throw;
  }
  std::sort(names.begin(), names.end());
  return names;
}

uint64_t QueryLeaderNodeId(GraphDB* graph) {
  auto txn = graph->BeginTransaction();
  rg::QueryResult result;
  try {
    const std::string graph_name = graph->db_meta().graph_name();
    result = rg::ExecuteQuery(
        *txn, "CALL dbms.graph.getRaftNodeInfos('" + graph_name +
                  "') YIELD node_id, is_leader WHERE is_leader RETURN node_id");
    txn->Commit();
  } catch (...) {
    if (txn->GetState() == Transaction::State::kActive) {
      txn->Rollback();
    }
    throw;
  }
  if (result.rows.size() != 1 || result.rows.front().size() != 1 ||
      !result.rows.front().front().IsInteger() ||
      result.rows.front().front().AsInteger() <= 0) {
    throw std::runtime_error("expected exactly one positive Raft leader id");
  }
  return static_cast<uint64_t>(result.rows.front().front().AsInteger());
}

TEST(RaftCluster, configuredNodeIdSharedAcrossGraphsAndRestarts) {
  TestServerCluster cluster("testdb_raft_configured_node_id", 3, 42);
  ASSERT_NO_THROW(cluster.Start());

  const std::string missing_id_graph = "missing_configured_id_graph";
  auto missing_id_infos = cluster.NodeInfosForGraph(missing_id_graph);
  missing_id_infos.mutable_nodes()->erase(42);
  EXPECT_THROW(cluster.server(0)->graph_manager()->CreateGraphWithRaft(
                   missing_id_graph, missing_id_infos),
               common::Exception);

  const std::string extra_graph = "configured_id_graph";
  ASSERT_NO_THROW(cluster.CreateRaftGraphOnAllServers(
      extra_graph, cluster.NodeInfosForGraph(extra_graph)));
  const std::vector<std::string> graph_names = {kGraphName, extra_graph};
  for (size_t server_index = 0; server_index < cluster.size(); ++server_index) {
    for (const auto& graph_name : graph_names) {
      auto graph =
          cluster.server(server_index)->graph_manager()->OpenGraph(graph_name);
      EXPECT_EQ(graph->raft_driver()->GetRaftStatus().s.basicStatus_.id_,
                cluster.node_id(server_index));
    }
  }

  cluster.StopServer(0);
  ASSERT_NO_THROW(cluster.StartServer(0));
  for (const auto& graph_name : graph_names) {
    auto graph = cluster.server(0)->graph_manager()->OpenGraph(graph_name);
    EXPECT_EQ(graph->raft_driver()->GetRaftStatus().s.basicStatus_.id_, 42U);
  }
}

TEST(RaftCluster, rejectsZeroConfiguredNodeId) {
  const std::string db_path = "testdb_raft_zero_node_id";
  fs::remove_all(db_path);
  EXPECT_THROW(server::GraphManager::Open(db_path, {}, {.raft_node_id = 0}),
               common::Exception);
  EXPECT_FALSE(fs::exists(db_path));
}

TEST(RaftCluster, threeServersElectLeaderAndReplicateTransaction) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* leader = cluster.WaitForLeader(std::chrono::seconds(15));
  ASSERT_NE(leader, nullptr) << cluster.StatusSummary();

  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);
  auto txn = leader_graph->BeginTransaction();
  auto v1 = txn->CreateVertex({"label1"}, kProperties);
  auto v2 = txn->CreateVertex({"label2"}, kProperties);
  auto e1 = txn->CreateEdge(v1, v2, "edge_type12", kProperties);
  txn->Commit();

  const auto applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_GT(applied_index, 0U);
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();

  for (auto* server : cluster.servers()) {
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    EXPECT_GE(graph->GetRaftApplyIndex(), applied_index)
        << cluster.StatusSummary();
    auto read_txn = graph->BeginTransaction();
    auto persisted_v1 = read_txn->GetVertexById(v1.GetId());
    auto persisted_v2 = read_txn->GetVertexById(v2.GetId());
    auto persisted_e1 = read_txn->GetEdgeById(e1.GetTypeId(), e1.GetId());
    EXPECT_EQ(persisted_v1.GetAllProperty(), kProperties);
    EXPECT_EQ(persisted_v2.GetAllProperty(), kProperties);
    EXPECT_EQ(persisted_e1.GetAllProperty(), kProperties);
    read_txn->Commit();
  }
}

TEST(RaftCluster, replicatesVertexPropertyIndexDdlAndIndexedWrites) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* leader = cluster.WaitForLeader(std::chrono::seconds(15));
  ASSERT_NE(leader, nullptr) << cluster.StatusSummary();
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);

  {
    auto txn = leader_graph->BeginTransaction();
    txn->CreateVertex({"person"},
                      {{"name", Value("alice")}, {"age", Value(31)}});
    txn->CreateVertex({"person"}, {{"name", Value("bob")}, {"age", Value(41)}});
    txn->Commit();
  }
  uint64_t applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();

  ASSERT_NO_THROW(leader_graph->AddVertexPropertyIndex("person_age", false,
                                                       "person", {"age"}));
  applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(WaitForAllPropertyIndexesReady(cluster, "person_age",
                                             std::chrono::seconds(15)))
      << cluster.StatusSummary();

  {
    auto txn = leader_graph->BeginTransaction();
    txn->CreateVertex({"person"},
                      {{"name", Value("carol")}, {"age", Value(31)}});
    txn->Commit();
  }
  applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();

  for (auto* server : cluster.servers()) {
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    ASSERT_EQ(CountPropertyIndexResults(graph.get(), "person_age", Value(31)),
              2U);
  }

  ASSERT_NO_THROW(leader_graph->DeleteVertexPropertyIndex("person_age"));
  applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(WaitForAllPropertyIndexesDeleted(cluster, "person_age",
                                               std::chrono::seconds(15)))
      << cluster.StatusSummary();
}

TEST(RaftCluster, replicatesFullTextAndVectorIndexDefinitions) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* leader = cluster.WaitForLeader(std::chrono::seconds(15));
  ASSERT_NE(leader, nullptr) << cluster.StatusSummary();
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);

  ASSERT_NO_THROW(leader_graph->AddVertexFullTextIndex("person_bio_ft",
                                                       {"person"}, {"bio"}));
  auto applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(WaitForAllFullTextIndexDefinitions(cluster, "person_bio_ft",
                                                 std::chrono::seconds(15)))
      << cluster.StatusSummary();

  ASSERT_NO_THROW(leader_graph->AddVertexVectorField("person", "embedding", 4));
  ASSERT_NO_THROW(leader_graph->AddVertexVectorIndex(
      "person_embedding_vt", "person", "embedding", 4, "l2", 16, 100));
  applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(WaitForAllVectorIndexDefinitions(cluster, "person_embedding_vt",
                                               std::chrono::seconds(15)))
      << cluster.StatusSummary();

  for (auto* server : cluster.servers()) {
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    auto ft_index = graph->meta_info().GetVertexFullTextIndex("person_bio_ft");
    ASSERT_NE(ft_index, nullptr);
    EXPECT_EQ(ft_index->meta().path().find(graph->path()), 0U);
    auto lid = graph->id_generator().GetLid("person");
    auto pid = graph->id_generator().GetPid("embedding");
    ASSERT_TRUE(lid.has_value());
    ASSERT_TRUE(pid.has_value());
    auto vector_field =
        graph->meta_info().GetVertexVectorField(lid.value(), pid.value());
    ASSERT_NE(vector_field, nullptr);
    EXPECT_EQ(vector_field->dimensions(), 4);
    auto vt_index =
        graph->meta_info().GetVertexVectorIndex("person_embedding_vt");
    ASSERT_NE(vt_index, nullptr);
    EXPECT_EQ(vt_index->meta().path().find(graph->path()), 0U);
  }

  ASSERT_NO_THROW(leader_graph->DeleteVertexFullTextIndex("person_bio_ft"));
  applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(WaitForAllFullTextIndexesDeleted(cluster, "person_bio_ft",
                                               std::chrono::seconds(15)))
      << cluster.StatusSummary();

  ASSERT_NO_THROW(leader_graph->DeleteVertexVectorIndex("person_embedding_vt"));
  applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(WaitForAllVectorIndexesDeleted(cluster, "person_embedding_vt",
                                             std::chrono::seconds(15)))
      << cluster.StatusSummary();
}

TEST(RaftCluster, followerRejectsVertexPropertyIndexDdl) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);
  {
    auto txn = leader_graph->BeginTransaction();
    txn->CreateVertex({"person"}, {{"age", Value(10)}});
    txn->Commit();
  }
  ASSERT_TRUE(cluster.WaitForApplyIndex(leader_graph->GetRaftApplyIndex(),
                                        std::chrono::seconds(15)))
      << cluster.StatusSummary();

  auto follower_index = FirstFollowerIndex(cluster, *leader_index);
  auto* follower = cluster.server(follower_index);
  ASSERT_NE(follower, nullptr);

  auto follower_graph = follower->graph_manager()->OpenGraph(kGraphName);
  const auto before_apply_index = follower_graph->GetRaftApplyIndex();

  EXPECT_THROW_CODE(follower_graph->AddVertexPropertyIndex(
                        "follower_local_index", false, "person", {"age"}),
                    StorageEngineError);
  EXPECT_EQ(follower_graph->GetRaftApplyIndex(), before_apply_index);
  EXPECT_EQ(follower_graph->meta_info().GetVertexPropertyIndex(
                "follower_local_index"),
            nullptr);
}

TEST(RaftCluster, multipleRaftGraphsElectLeadersAndReplicateWrites) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  std::vector<std::string> graph_names = {kGraphName};
  for (int i = 0; i < 4; ++i) {
    graph_names.push_back("raft_multi_graph_" + std::to_string(i));
  }

  for (size_t i = 1; i < graph_names.size(); ++i) {
    const auto& graph_name = graph_names[i];
    auto node_infos = cluster.NodeInfosForGraph(graph_name);
    ASSERT_NO_THROW(cluster.CreateRaftGraphOnAllServers(graph_name, node_infos))
        << graph_name;
    ASSERT_TRUE(
        cluster.WaitForGraphCreated(graph_name, std::chrono::seconds(20)))
        << graph_name << " " << cluster.StatusSummary(graph_name);
  }

  struct WrittenVertex {
    std::string graph_name;
    int64_t vertex_id = 0;
    std::unordered_map<std::string, Value> properties;
    uint64_t applied_index = 0;
  };

  std::vector<WrittenVertex> written_vertices;
  written_vertices.reserve(graph_names.size());
  for (const auto& graph_name : graph_names) {
    auto* leader = cluster.WaitForLeader(graph_name, std::chrono::seconds(20));
    ASSERT_NE(leader, nullptr)
        << graph_name << " " << cluster.StatusSummary(graph_name);

    auto properties = kProperties;
    properties["graph_name"] = Value(graph_name);
    auto graph = leader->graph_manager()->OpenGraph(graph_name);
    auto txn = graph->BeginTransaction();
    auto vertex = txn->CreateVertex({graph_name + "_label"}, properties);
    txn->Commit();

    written_vertices.push_back(WrittenVertex{
        graph_name, vertex.GetId(), properties, graph->GetRaftApplyIndex()});
  }

  for (const auto& written : written_vertices) {
    ASSERT_GT(written.applied_index, 0U) << written.graph_name;
    ASSERT_TRUE(cluster.WaitForApplyIndex(
        written.graph_name, written.applied_index, std::chrono::seconds(20)))
        << written.graph_name << " "
        << cluster.StatusSummary(written.graph_name);
    ASSERT_TRUE(cluster.WaitForGraphVertexCount(written.graph_name, 1,
                                                std::chrono::seconds(15)))
        << written.graph_name;
  }

  for (auto* server : cluster.servers()) {
    for (const auto& written : written_vertices) {
      auto graph = server->graph_manager()->OpenGraph(written.graph_name);
      auto read_txn = graph->BeginTransaction();
      auto persisted_vertex = read_txn->GetVertexById(written.vertex_id);
      EXPECT_EQ(persisted_vertex.GetAllProperty(), written.properties)
          << written.graph_name;
      read_txn->Commit();
    }
  }
}

TEST(RaftCluster,
     multipleRaftGraphsUseCypherWithFullTextSingleAndCompositeIndexes) {
  constexpr char kFullTextIndex[] = "person_text_ft";
  constexpr char kRegionIndex[] = "person_region_idx";
  constexpr char kRegionScoreIndex[] = "person_region_score_idx";
  constexpr char kVectorIndex[] = "person_embedding_vt";

  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  struct GraphCase {
    std::string graph_name;
    std::string alias;
    std::string token;
    std::vector<std::string> region_names;
    std::vector<std::string> composite_names;
    std::vector<std::string> fulltext_names;
    std::vector<std::string> vector_knn_names;
  };

  std::vector<GraphCase> graph_cases = {{kGraphName,
                                         "g0",
                                         "apollo",
                                         {"g0_alice", "g0_carol", "g0_dave"},
                                         {"g0_carol", "g0_dave"},
                                         {"g0_carol"},
                                         {"g0_alice", "g0_carol"}},
                                        {"raft_index_graph_1",
                                         "g1",
                                         "borealis",
                                         {"g1_alice", "g1_carol", "g1_dave"},
                                         {"g1_carol", "g1_dave"},
                                         {"g1_carol"},
                                         {"g1_alice", "g1_carol"}},
                                        {"raft_index_graph_2",
                                         "g2",
                                         "cobalt",
                                         {"g2_alice", "g2_carol", "g2_dave"},
                                         {"g2_carol", "g2_dave"},
                                         {"g2_carol"},
                                         {"g2_alice", "g2_carol"}}};

  for (size_t i = 1; i < graph_cases.size(); ++i) {
    const auto& graph_name = graph_cases[i].graph_name;
    auto node_infos = cluster.NodeInfosForGraph(graph_name);
    ASSERT_NO_THROW(cluster.CreateRaftGraphOnAllServers(graph_name, node_infos))
        << graph_name;
    ASSERT_TRUE(
        cluster.WaitForGraphCreated(graph_name, std::chrono::seconds(20)))
        << graph_name << " " << cluster.StatusSummary(graph_name);
  }

  for (const auto& graph_case : graph_cases) {
    auto* leader =
        cluster.WaitForLeader(graph_case.graph_name, std::chrono::seconds(20));
    ASSERT_NE(leader, nullptr) << graph_case.graph_name << " "
                               << cluster.StatusSummary(graph_case.graph_name);
    auto graph = leader->graph_manager()->OpenGraph(graph_case.graph_name);

    uint64_t applied_index = 0;
    ASSERT_NO_THROW(applied_index = ExecuteCypherAndCommit(
                        graph.get(),
                        "CALL db.index.vector.createNodeField('person', "
                        "'embedding', {dimension:2})"));
    ASSERT_TRUE(cluster.WaitForApplyIndex(graph_case.graph_name, applied_index,
                                          std::chrono::seconds(20)))
        << graph_case.graph_name << " "
        << cluster.StatusSummary(graph_case.graph_name);

    ASSERT_NO_THROW(applied_index = ExecuteCypherAndCommit(
                        graph.get(),
                        "CALL db.index.vector.createNodeIndex("
                        "'person_embedding_vt', 'person', 'embedding', "
                        "{dimension:2, distance_type:'l2', hnsw_m:8, "
                        "hnsw_ef_construction:20})"));
    ASSERT_TRUE(cluster.WaitForApplyIndex(graph_case.graph_name, applied_index,
                                          std::chrono::seconds(20)))
        << graph_case.graph_name << " "
        << cluster.StatusSummary(graph_case.graph_name);
    ASSERT_TRUE(WaitForAllVectorIndexesReady(
        cluster, graph_case.graph_name, kVectorIndex, std::chrono::seconds(20)))
        << graph_case.graph_name;

    ASSERT_NO_THROW(applied_index = ExecuteCypherAndCommit(
                        graph.get(),
                        "CALL db.index.fulltext.createNodeIndex("
                        "'person_text_ft', ['person'], ['bio', 'team'])"));
    ASSERT_TRUE(cluster.WaitForApplyIndex(graph_case.graph_name, applied_index,
                                          std::chrono::seconds(20)))
        << graph_case.graph_name << " "
        << cluster.StatusSummary(graph_case.graph_name);
    ASSERT_TRUE(WaitForAllFullTextIndexesReady(cluster, graph_case.graph_name,
                                               kFullTextIndex,
                                               std::chrono::seconds(20)))
        << graph_case.graph_name;

    ASSERT_NO_THROW(applied_index = ExecuteCypherAndCommit(
                        graph.get(),
                        "CALL db.index.createNodeIndex('person_region_idx', "
                        "'person', ['region'], {unique:false})"));
    ASSERT_TRUE(cluster.WaitForApplyIndex(graph_case.graph_name, applied_index,
                                          std::chrono::seconds(20)))
        << graph_case.graph_name << " "
        << cluster.StatusSummary(graph_case.graph_name);
    ASSERT_TRUE(WaitForAllPropertyIndexesReady(
        cluster, graph_case.graph_name, kRegionIndex, std::chrono::seconds(20)))
        << graph_case.graph_name;

    ASSERT_NO_THROW(applied_index = ExecuteCypherAndCommit(
                        graph.get(),
                        "CALL db.index.createNodeIndex("
                        "'person_region_score_idx', 'person', "
                        "['region', 'score'], {unique:false})"));
    ASSERT_TRUE(cluster.WaitForApplyIndex(graph_case.graph_name, applied_index,
                                          std::chrono::seconds(20)))
        << graph_case.graph_name << " "
        << cluster.StatusSummary(graph_case.graph_name);
    ASSERT_TRUE(WaitForAllPropertyIndexesReady(cluster, graph_case.graph_name,
                                               kRegionScoreIndex,
                                               std::chrono::seconds(20)))
        << graph_case.graph_name;

    auto txn = graph->BeginTransaction();
    auto create_person = [&txn, &graph_case](int id, const char* region,
                                             int score, const char* suffix,
                                             std::string bio, const char* team,
                                             double x, double y) {
      txn->CreateVertex(
          {"person"}, {{"id", Value(id)},
                       {"shard", Value(graph_case.alias)},
                       {"region", Value(region)},
                       {"score", Value(score)},
                       {"name", Value(graph_case.alias + suffix)},
                       {"bio", Value(std::move(bio))},
                       {"team", Value(team)},
                       {"embedding", Value(Value::List{Value(x), Value(y)})}});
    };
    create_person(1, "north", 10, "_alice",
                  graph_case.token + " raft fulltext north alpha", "kernel",
                  1.0, 0.0);
    create_person(2, "south", 20, "_bob",
                  graph_case.token + " raft fulltext south beta", "storage",
                  0.0, 1.0);
    create_person(3, "north", 30, "_carol",
                  graph_case.token + " composite index fulltext gamma", "query",
                  0.8, 0.6);
    create_person(4, "north", 30, "_dave", "archive composite without token",
                  "raft", 0.6, 0.8);
    txn->Commit();
    applied_index = graph->GetRaftApplyIndex();
    ASSERT_TRUE(cluster.WaitForApplyIndex(graph_case.graph_name, applied_index,
                                          std::chrono::seconds(20)))
        << graph_case.graph_name << " "
        << cluster.StatusSummary(graph_case.graph_name);
  }

  for (auto* server : cluster.servers()) {
    for (const auto& graph_case : graph_cases) {
      auto graph = server->graph_manager()->OpenGraph(graph_case.graph_name);

      EXPECT_EQ(CollectCypherStringColumn(
                    graph.get(),
                    "CALL db.index.queryNodes('person_region_idx', 'north') "
                    "YIELD node RETURN node.name"),
                graph_case.region_names)
          << graph_case.graph_name;

      EXPECT_EQ(CollectCypherStringColumn(
                    graph.get(),
                    "CALL db.index.queryNodes('person_region_score_idx', "
                    "['north', 30]) YIELD node RETURN node.name"),
                graph_case.composite_names)
          << graph_case.graph_name;

      EXPECT_EQ(CollectCypherStringColumn(
                    graph.get(),
                    "CALL db.index.rangeQueryNodes("
                    "'person_region_score_idx', ['north', 20], "
                    "['north', 40], {left_closed:true, right_closed:true}) "
                    "YIELD node RETURN node.name"),
                graph_case.composite_names)
          << graph_case.graph_name;

      auto collect_fulltext_names = [&]() {
        return CollectCypherStringColumn(
            graph.get(),
            "CALL db.index.fulltext.queryNodes('person_text_ft', '" +
                graph_case.token +
                " AND composite', 10) YIELD node, score RETURN node.name");
      };
      ASSERT_TRUE(WaitUntil(
          [&]() {
            try {
              return collect_fulltext_names() == graph_case.fulltext_names;
            } catch (const common::Exception& e) {
              if (e.code() == common::ErrorCode::IndexNotReady) {
                return false;
              }
              throw;
            }
          },
          std::chrono::seconds(20), std::chrono::milliseconds(50)))
          << graph_case.graph_name;
      EXPECT_EQ(collect_fulltext_names(), graph_case.fulltext_names)
          << graph_case.graph_name;

      auto collect_vector_names = [&]() {
        return CollectCypherStringColumn(
            graph.get(),
            "CALL db.index.vector.knnSearchNodes('person_embedding_vt', "
            "[1.0, 0.0], {top_k:2}) YIELD node, distance RETURN node.name");
      };
      ASSERT_TRUE(WaitUntil(
          [&]() {
            try {
              return collect_vector_names() == graph_case.vector_knn_names;
            } catch (const common::Exception& e) {
              if (e.code() == common::ErrorCode::IndexNotReady) {
                return false;
              }
              throw;
            }
          },
          std::chrono::seconds(20), std::chrono::milliseconds(50)))
          << graph_case.graph_name;
      EXPECT_EQ(collect_vector_names(), graph_case.vector_knn_names)
          << graph_case.graph_name;
    }
  }
}

TEST(RaftCluster, followerRejectsGraphWriteProposal) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto follower_index = FirstFollowerIndex(cluster, *leader_index);
  auto* follower = cluster.server(follower_index);
  ASSERT_NE(follower, nullptr);

  auto follower_graph = follower->graph_manager()->OpenGraph(kGraphName);
  const auto before_apply_index = follower_graph->GetRaftApplyIndex();

  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("follower_rejected_key", "value").ok());
  auto result = follower_graph->raft_driver()->ProposeWriteBatch(
      meta::WriteBatchKind::GRAPH_WRITE, wb);

  ASSERT_NE(result.err, nullptr);
  EXPECT_NE(result.err.String().find("not leader"), std::string::npos);
  EXPECT_EQ(follower_graph->GetRaftApplyIndex(), before_apply_index);
}

TEST(RaftCluster, followerTransactionCommitRollsBackLocalWrites) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto follower_index = FirstFollowerIndex(cluster, *leader_index);
  auto* follower = cluster.server(follower_index);
  ASSERT_NE(follower, nullptr);

  auto follower_graph = follower->graph_manager()->OpenGraph(kGraphName);
  const std::string key = "follower_txn_rollback_key";
  {
    auto txn = follower_graph->BeginTransaction();
    auto status = txn->dbtxn()->Put(follower_graph->graph_cf().graph_topology,
                                    key, "value_that_must_not_commit");
    ASSERT_TRUE(status.ok()) << status.ToString();
    EXPECT_THROW(txn->Commit(), std::exception);
  }

  std::string value;
  auto status = follower_graph->raw_db()->Get(
      rocksdb::ReadOptions(), follower_graph->graph_cf().graph_topology, key,
      &value);
  EXPECT_TRUE(status.IsNotFound()) << status.ToString();
}

TEST(RaftCluster, proposalDoesNotCommitWithoutMajority) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  for (size_t i = 0; i < cluster.size(); ++i) {
    if (i != *leader_index) {
      ASSERT_NO_THROW(cluster.StopServer(i));
    }
  }

  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);
  const auto before_apply_index = leader_graph->GetRaftApplyIndex();

  auto context = leader_graph->raft_driver()->ProposeRaftRequest(
      MakeGraphWriteRequest("no_majority_key", "value"));
  auto applied = context->applied.get_future();
  auto status = applied.wait_for(std::chrono::seconds(2));
  if (status == std::future_status::ready) {
    auto result = applied.get();
    EXPECT_NE(result.err, nullptr);
  }
  EXPECT_EQ(leader_graph->GetRaftApplyIndex(), before_apply_index);
}

TEST(RaftCluster, pendingProposalIsRejectedWhenLeaderStops) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  for (size_t i = 0; i < cluster.size(); ++i) {
    if (i != *leader_index) {
      ASSERT_NO_THROW(cluster.StopServer(i));
    }
  }

  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);

  auto context = leader_graph->raft_driver()->ProposeRaftRequest(
      MakeGraphWriteRequest("leader_stop_key", "value"));
  auto applied = context->applied.get_future();
  leader_graph.reset();
  ASSERT_NO_THROW(cluster.StopServer(*leader_index));

  ASSERT_EQ(applied.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  auto result = applied.get();
  EXPECT_NE(result.err, nullptr);
  EXPECT_EQ(result.index, 0U);
}

TEST(RaftCluster, restartedFollowerCatchesUpMultipleTransactions) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto follower_index = FirstFollowerIndex(cluster, *leader_index);
  ASSERT_NO_THROW(cluster.StopServer(follower_index));

  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);

  std::vector<int64_t> vertex_ids;
  uint64_t applied_index = 0;
  for (int i = 0; i < 5; ++i) {
    auto txn = leader_graph->BeginTransaction();
    auto vertex = txn->CreateVertex({"catchup_label"}, kProperties);
    vertex_ids.push_back(vertex.GetId());
    txn->Commit();
    applied_index = leader_graph->GetRaftApplyIndex();
  }

  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  leader_graph.reset();

  ASSERT_NO_THROW(cluster.StartServer(follower_index));
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(30)))
      << cluster.StatusSummary();

  auto follower_graph =
      cluster.server(follower_index)->graph_manager()->OpenGraph(kGraphName);
  auto read_txn = follower_graph->BeginTransaction();
  for (auto vertex_id : vertex_ids) {
    auto persisted_vertex = read_txn->GetVertexById(vertex_id);
    EXPECT_EQ(persisted_vertex.GetAllProperty(), kProperties);
  }
  read_txn->Commit();
}

TEST(RaftCluster, nodeInfosMarkOnlyCurrentLeader) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto assert_current_leader_marked = [&cluster](size_t leader_index) {
    auto graph =
        cluster.server(leader_index)->graph_manager()->OpenGraph(kGraphName);
    auto node_infos = graph->raft_driver()->GetNodeInfosWithLeader();
    uint64_t marked_leader = 0;
    size_t leader_marks = 0;
    for (const auto& [node_id, node_info] : node_infos.nodes()) {
      if (node_info.is_leader()) {
        marked_leader = node_id;
        ++leader_marks;
      }
    }
    EXPECT_EQ(leader_marks, 1U) << node_infos.ShortDebugString();
    EXPECT_EQ(marked_leader, cluster.node_id(leader_index))
        << node_infos.ShortDebugString();
  };

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  assert_current_leader_marked(*leader_index);

  ASSERT_NO_THROW(cluster.StopServer(*leader_index));
  auto new_leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(20));
  ASSERT_TRUE(new_leader_index.has_value()) << cluster.StatusSummary();
  ASSERT_NE(*new_leader_index, *leader_index);
  assert_current_leader_marked(*new_leader_index);
}

TEST(RaftCluster, raftNodeInfosMarkCurrentLeader) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto graph = leader->graph_manager()->OpenGraph(kGraphName);

  EXPECT_EQ(QueryLeaderNodeId(graph.get()), cluster.node_id(*leader_index));
}

TEST(RaftCluster, createRaftGraphWithRaftUpdatesOnlyLocalMetadata) {
  constexpr char kLocalGraphName[] = "local_only_raft_graph";

  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* local_server = cluster.server(0);
  ASSERT_NE(local_server, nullptr);
  auto node_infos = cluster.NodeInfosForGraph(kLocalGraphName);
  ASSERT_NO_THROW(local_server->graph_manager()->CreateGraphWithRaft(
      kLocalGraphName, node_infos));
  ASSERT_NO_THROW(local_server->graph_manager()->OpenGraph(kLocalGraphName));

  for (size_t i = 1; i < cluster.size(); ++i) {
    auto* server = cluster.server(i);
    ASSERT_NE(server, nullptr);
    EXPECT_THROW_CODE(server->graph_manager()->OpenGraph(kLocalGraphName),
                      NoSuchGraph);
  }
}

TEST(RaftCluster, createAndDeleteRaftGraphThroughSystemProcedures) {
  constexpr char kLocalGraphName[] = "system_procedure_raft_graph";

  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* local_server = cluster.server(0);
  ASSERT_NE(local_server, nullptr);
  auto* graph_manager = local_server->graph_manager();
  const auto node_infos = cluster.NodeInfosForGraph(kLocalGraphName);

  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      graph_manager,
      "CALL dbms.graph.createGraphWithRaft($graph_name, $members)",
      {{"graph_name", Value(kLocalGraphName)},
       {"members", RaftMembersValue(node_infos)}}));
  auto graph = graph_manager->OpenGraph(kLocalGraphName);
  ASSERT_TRUE(graph->db_meta().enable_raft());
  ASSERT_NE(graph->raft_driver(), nullptr);
  graph.reset();

  auto listed =
      ExecuteSystemCypherAndCommit(graph_manager,
                                   "CALL dbms.graph.listGraph() YIELD name "
                                   "WHERE name = $graph_name RETURN name",
                                   {{"graph_name", Value(kLocalGraphName)}});
  ASSERT_EQ(listed.rows.size(), 1U);
  ASSERT_EQ(listed.rows.front().size(), 1U);
  EXPECT_EQ(listed.rows.front().front(), Value(kLocalGraphName));

  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      graph_manager, "CALL dbms.graph.deleteGraph($graph_name)",
      {{"graph_name", Value(kLocalGraphName)}}));
  EXPECT_THROW_CODE(graph_manager->OpenGraph(kLocalGraphName), NoSuchGraph);
}

TEST(RaftCluster, manageRaftMembershipThroughSystemProcedures) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  const size_t target_index = FirstFollowerIndex(cluster, *leader_index);
  const uint64_t target_node_id = cluster.node_id(target_index);
  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto* management = leader->graph_manager();

  auto status = ExecuteSystemCypherAndCommit(
      management,
      "CALL dbms.graph.getRaftStatus($graph_name) "
      "YIELD node_id, leader_id, term, raft_state "
      "RETURN node_id, leader_id, term, raft_state ORDER BY node_id",
      {{"graph_name", Value(kGraphName)}});
  ASSERT_EQ(status.rows.size(), cluster.size());
  for (const auto& row : status.rows) {
    ASSERT_EQ(row.size(), 4U);
    EXPECT_EQ(row[1],
              Value(static_cast<std::int64_t>(cluster.node_id(*leader_index))));
    EXPECT_TRUE(row[2].IsInteger());
    EXPECT_EQ(row[3], Value("StateLeader"));
  }

  auto demoted = ExecuteSystemCypherAndCommit(
      management,
      "CALL dbms.graph.demoteRaftNode($graph_name, $node_id) "
      "YIELD node_id, role, raft_index RETURN node_id, role, raft_index",
      {{"graph_name", Value(kGraphName)},
       {"node_id", Value(static_cast<std::int64_t>(target_node_id))}});
  ASSERT_EQ(demoted.rows.size(), 1U);
  ASSERT_EQ(demoted.rows.front().size(), 3U);
  EXPECT_EQ(demoted.rows.front()[0],
            Value(static_cast<std::int64_t>(target_node_id)));
  EXPECT_EQ(demoted.rows.front()[1], Value("learner"));
  EXPECT_GT(demoted.rows.front()[2].AsInteger(), 0);
  ASSERT_TRUE(WaitUntil(
      [management, target_node_id]() {
        auto infos = management->ManagedGraphRaftNodeInfos(kGraphName);
        auto iter = infos.nodes().find(target_node_id);
        return iter != infos.nodes().end() && iter->second.is_learner();
      },
      std::chrono::seconds(15)));

  ASSERT_TRUE(WaitUntil(
      [management, target_node_id]() {
        auto raft_status = management->ManagedGraphRaftStatus(kGraphName);
        auto iter = std::ranges::find(raft_status.nodes, target_node_id,
                                      &rg::ManagedRaftNodeStatus::node_id);
        return iter != raft_status.nodes.end() &&
               iter->match_index >= raft_status.commit_index;
      },
      std::chrono::seconds(15)));
  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      management,
      "CALL dbms.graph.promoteRaftLearnerNode($graph_name, $node_id)",
      {{"graph_name", Value(kGraphName)},
       {"node_id", Value(static_cast<std::int64_t>(target_node_id))}}));
  ASSERT_TRUE(WaitUntil(
      [management, target_node_id]() {
        auto infos = management->ManagedGraphRaftNodeInfos(kGraphName);
        auto iter = infos.nodes().find(target_node_id);
        return iter != infos.nodes().end() && !iter->second.is_learner();
      },
      std::chrono::seconds(15)));

  auto target_info =
      cluster.NodeInfosForGraph(kGraphName).nodes().at(target_node_id);
  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      management, "CALL dbms.graph.updateRaftNode($graph_name, $member)",
      {{"graph_name", Value(kGraphName)},
       {"member", RaftMemberValue(target_info)}}));

  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      management, "CALL dbms.graph.transferRaftLeader($graph_name, $node_id)",
      {{"graph_name", Value(kGraphName)},
       {"node_id", Value(static_cast<std::int64_t>(target_node_id))}}));
  auto transferred_leader =
      cluster.WaitForLeaderIndex(std::chrono::seconds(20));
  ASSERT_TRUE(transferred_leader.has_value()) << cluster.StatusSummary();
  ASSERT_EQ(*transferred_leader, target_index);

  auto* new_management = cluster.server(target_index)->graph_manager();
  const uint64_t removed_node_id = cluster.node_id(*leader_index);
  auto removed_info =
      cluster.NodeInfosForGraph(kGraphName).nodes().at(removed_node_id);
  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      new_management, "CALL dbms.graph.removeRaftNode($graph_name, $node_id)",
      {{"graph_name", Value(kGraphName)},
       {"node_id", Value(static_cast<std::int64_t>(removed_node_id))}}));
  ASSERT_TRUE(WaitUntil(
      [new_management, removed_node_id]() {
        return !new_management->ManagedGraphRaftNodeInfos(kGraphName)
                    .nodes()
                    .contains(removed_node_id);
      },
      std::chrono::seconds(15)));

  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      new_management,
      "CALL dbms.graph.addRaftLearnerNode($graph_name, $member)",
      {{"graph_name", Value(kGraphName)},
       {"member", RaftMemberValue(removed_info)}}));
  ASSERT_TRUE(WaitUntil(
      [new_management, removed_node_id]() {
        auto infos = new_management->ManagedGraphRaftNodeInfos(kGraphName);
        auto iter = infos.nodes().find(removed_node_id);
        return iter != infos.nodes().end() && iter->second.is_learner();
      },
      std::chrono::seconds(15)));

  auto direct_voter = removed_info;
  direct_voter.set_node_id(100);
  direct_voter.set_bolt_port(AllocateFreePort());
  direct_voter.set_raft_poft(AllocateFreePort());
  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      new_management, "CALL dbms.graph.addRaftNode($graph_name, $member)",
      {{"graph_name", Value(kGraphName)},
       {"member", RaftMemberValue(direct_voter)}}));
  ASSERT_NO_THROW(ExecuteSystemCypherAndCommit(
      new_management, "CALL dbms.graph.removeRaftNode($graph_name, $node_id)",
      {{"graph_name", Value(kGraphName)}, {"node_id", Value(100)}}));
}

TEST(RaftCluster, clearRaftGraphUpdatesOnlyLocalServer) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto* graph_leader = cluster.server(*leader_index);
  ASSERT_NE(graph_leader, nullptr);
  auto leader_graph = graph_leader->graph_manager()->OpenGraph(kGraphName);

  auto txn = leader_graph->BeginTransaction();
  txn->CreateVertex({"person"}, {{"name", Value("before_clear")}});
  txn->Commit();
  const auto applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();
  ASSERT_TRUE(
      cluster.WaitForGraphVertexCount(kGraphName, 1, std::chrono::seconds(15)));

  ASSERT_NO_THROW(graph_leader->graph_manager()->ClearGraph(kGraphName));
  EXPECT_EQ(cluster.VertexCount(*leader_index), 0U);
  for (size_t i = 0; i < cluster.size(); ++i) {
    if (i == *leader_index) {
      continue;
    }
    EXPECT_EQ(cluster.VertexCount(i), 1U);
  }
}

TEST(RaftCluster, deleteRaftGraphStopsGroupAndRemovesLocalDataOnly) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* local_server = cluster.server(0);
  ASSERT_NE(local_server, nullptr);
  auto graph = local_server->graph_manager()->OpenGraph(kGraphName);
  ASSERT_TRUE(graph->db_meta().enable_raft());
  ASSERT_NE(graph->raft_driver(), nullptr);
  const auto local_graph_path = graph->path();
  graph.reset();

  ASSERT_NO_THROW(local_server->graph_manager()->DeleteGraph(kGraphName));
  EXPECT_THROW_CODE(local_server->graph_manager()->OpenGraph(kGraphName),
                    NoSuchGraph);
  ASSERT_TRUE(
      WaitUntil([&local_graph_path]() { return !fs::exists(local_graph_path); },
                std::chrono::seconds(15)));

  for (size_t i = 1; i < cluster.size(); ++i) {
    auto* server = cluster.server(i);
    ASSERT_NE(server, nullptr);
    ASSERT_NO_THROW(server->graph_manager()->OpenGraph(kGraphName));
  }
}

TEST(RaftCluster, removeLeaderFromGraphRaftElectsNewLeaderAndCommits) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto old_leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(old_leader_index.has_value()) << cluster.StatusSummary();
  auto old_leader_node_id = cluster.node_id(*old_leader_index);

  auto* old_leader = cluster.server(*old_leader_index);
  ASSERT_NE(old_leader, nullptr);
  auto old_leader_graph = old_leader->graph_manager()->OpenGraph(kGraphName);

  auto initial_node_infos = cluster.NodeInfosForGraph(kGraphName);
  ASSERT_TRUE(initial_node_infos.nodes().count(old_leader_node_id));
  auto old_leader_node_info = initial_node_infos.nodes().at(old_leader_node_id);

  raftpb::ConfChange remove_leader;
  remove_leader.set_type(raftpb::ConfChangeRemoveNode);
  remove_leader.set_node_id(old_leader_node_id);
  remove_leader.set_context(old_leader_node_info.SerializeAsString());

  auto remove_result = WaitForAppliedResult(
      old_leader_graph->raft_driver()->ProposeConfChange(remove_leader),
      std::chrono::seconds(15));
  ASSERT_EQ(remove_result.err, nullptr) << remove_result.err.String();
  ASSERT_GT(remove_result.index, 0U);
  old_leader_graph.reset();

  auto new_leader_index = WaitForLeaderIndexExcluding(
      cluster, {*old_leader_index}, std::chrono::seconds(20));
  ASSERT_TRUE(new_leader_index.has_value()) << cluster.StatusSummary();
  ASSERT_NE(*new_leader_index, *old_leader_index);

  ASSERT_TRUE(WaitUntil(
      [&cluster, old_leader_index, old_leader_node_id]() {
        for (size_t i = 0; i < cluster.size(); ++i) {
          if (i == *old_leader_index) {
            continue;
          }
          auto* server = cluster.server(i);
          if (server == nullptr) {
            continue;
          }
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          auto node_infos = graph->raft_driver()->GetNodeInfosWithLeader();
          if (node_infos.nodes().count(old_leader_node_id)) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(15)))
      << cluster.StatusSummary();

  auto* new_leader = cluster.server(*new_leader_index);
  ASSERT_NE(new_leader, nullptr);
  auto new_leader_graph = new_leader->graph_manager()->OpenGraph(kGraphName);

  int64_t vertex_id = 0;
  uint64_t applied_index = 0;
  {
    auto txn = new_leader_graph->BeginTransaction();
    auto vertex = txn->CreateVertex({"remove_leader_label"}, kProperties);
    vertex_id = vertex.GetId();
    txn->Commit();
    applied_index = new_leader_graph->GetRaftApplyIndex();
  }

  ASSERT_TRUE(WaitUntil(
      [&cluster, old_leader_index, applied_index]() {
        for (size_t i = 0; i < cluster.size(); ++i) {
          if (i == *old_leader_index) {
            continue;
          }
          auto* server = cluster.server(i);
          if (server == nullptr) {
            continue;
          }
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          if (graph->GetRaftApplyIndex() < applied_index) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(15)))
      << cluster.StatusSummary();

  for (size_t i = 0; i < cluster.size(); ++i) {
    if (i == *old_leader_index) {
      continue;
    }
    auto* server = cluster.server(i);
    ASSERT_NE(server, nullptr);
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    auto read_txn = graph->BeginTransaction();
    auto persisted_vertex = read_txn->GetVertexById(vertex_id);
    EXPECT_EQ(persisted_vertex.GetAllProperty(), kProperties);
    read_txn->Commit();
  }
}

TEST(RaftCluster, removeFollowerFromGraphRaftKeepsMajorityWritable) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(leader_index.has_value()) << cluster.StatusSummary();
  auto removed_index = FirstFollowerIndex(cluster, *leader_index);
  auto removed_node_id = cluster.node_id(removed_index);

  auto* leader = cluster.server(*leader_index);
  ASSERT_NE(leader, nullptr);
  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);

  auto initial_node_infos = cluster.NodeInfosForGraph(kGraphName);
  ASSERT_TRUE(initial_node_infos.nodes().count(removed_node_id));
  auto removed_node_info = initial_node_infos.nodes().at(removed_node_id);

  raftpb::ConfChange remove_follower;
  remove_follower.set_type(raftpb::ConfChangeRemoveNode);
  remove_follower.set_node_id(removed_node_id);
  remove_follower.set_context(removed_node_info.SerializeAsString());

  auto remove_result = WaitForAppliedResult(
      leader_graph->raft_driver()->ProposeConfChange(remove_follower),
      std::chrono::seconds(15));
  ASSERT_EQ(remove_result.err, nullptr) << remove_result.err.String();
  ASSERT_GT(remove_result.index, 0U);

  ASSERT_TRUE(WaitUntil(
      [&cluster, removed_index, removed_node_id]() {
        for (size_t i = 0; i < cluster.size(); ++i) {
          if (i == removed_index) {
            continue;
          }
          auto* server = cluster.server(i);
          if (server == nullptr) {
            continue;
          }
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          auto node_infos = graph->raft_driver()->GetNodeInfosWithLeader();
          if (node_infos.nodes().count(removed_node_id)) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(15)))
      << cluster.StatusSummary();

  int64_t vertex_id = 0;
  uint64_t applied_index = 0;
  {
    auto txn = leader_graph->BeginTransaction();
    auto vertex = txn->CreateVertex({"remove_follower_label"}, kProperties);
    vertex_id = vertex.GetId();
    txn->Commit();
    applied_index = leader_graph->GetRaftApplyIndex();
  }

  ASSERT_TRUE(WaitUntil(
      [&cluster, removed_index, applied_index]() {
        for (size_t i = 0; i < cluster.size(); ++i) {
          if (i == removed_index) {
            continue;
          }
          auto* server = cluster.server(i);
          if (server == nullptr) {
            continue;
          }
          auto graph = server->graph_manager()->OpenGraph(kGraphName);
          if (graph->GetRaftApplyIndex() < applied_index) {
            return false;
          }
        }
        return true;
      },
      std::chrono::seconds(15)))
      << cluster.StatusSummary();

  for (size_t i = 0; i < cluster.size(); ++i) {
    if (i == removed_index) {
      continue;
    }
    auto* server = cluster.server(i);
    ASSERT_NE(server, nullptr);
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    auto read_txn = graph->BeginTransaction();
    auto persisted_vertex = read_txn->GetVertexById(vertex_id);
    EXPECT_EQ(persisted_vertex.GetAllProperty(), kProperties);
    read_txn->Commit();
  }
}

TEST(RaftCluster, allServersRestartAndRecoverCommittedData) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto* leader = cluster.WaitForLeader(std::chrono::seconds(15));
  ASSERT_NE(leader, nullptr) << cluster.StatusSummary();

  int64_t vertex_id = 0;
  uint64_t applied_index = 0;
  {
    auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);
    auto txn = leader_graph->BeginTransaction();
    auto vertex = txn->CreateVertex({"restart_cluster_label"}, kProperties);
    vertex_id = vertex.GetId();
    txn->Commit();

    applied_index = leader_graph->GetRaftApplyIndex();
    ASSERT_TRUE(
        cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
        << cluster.StatusSummary();
  }

  for (size_t i = 0; i < cluster.size(); ++i) {
    ASSERT_NO_THROW(cluster.StopServer(i));
  }
  for (size_t i = 0; i < cluster.size(); ++i) {
    ASSERT_NO_THROW(cluster.StartServer(i));
  }

  ASSERT_TRUE(cluster.WaitForGraphCreated(std::chrono::seconds(20)))
      << cluster.StatusSummary();
  ASSERT_NE(cluster.WaitForLeader(std::chrono::seconds(20)), nullptr)
      << cluster.StatusSummary();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(30)))
      << cluster.StatusSummary();

  for (auto* server : cluster.servers()) {
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    auto read_txn = graph->BeginTransaction();
    auto persisted_vertex = read_txn->GetVertexById(vertex_id);
    EXPECT_EQ(persisted_vertex.GetAllProperty(), kProperties);
    read_txn->Commit();
  }
}

TEST(RaftCluster, singleServerElectsLeaderAndCommitsTransaction) {
  TestServerCluster cluster("testdb_raft_cluster", 1);
  ASSERT_NO_THROW(cluster.Start());

  auto* leader = cluster.WaitForLeader(std::chrono::seconds(15));
  ASSERT_NE(leader, nullptr) << cluster.StatusSummary();

  auto leader_graph = leader->graph_manager()->OpenGraph(kGraphName);
  auto txn = leader_graph->BeginTransaction();
  auto vertex = txn->CreateVertex({"single_node_label"}, kProperties);
  txn->Commit();

  const auto applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_GT(applied_index, 0U);
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(10)))
      << cluster.StatusSummary();

  auto read_txn = leader_graph->BeginTransaction();
  auto persisted_vertex = read_txn->GetVertexById(vertex.GetId());
  EXPECT_EQ(persisted_vertex.GetAllProperty(), kProperties);
  read_txn->Commit();
}

TEST(RaftCluster, stoppedLeaderFailsOverAndCatchesUpAfterRestart) {
  TestServerCluster cluster("testdb_raft_cluster");
  ASSERT_NO_THROW(cluster.Start());

  auto old_leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(old_leader_index.has_value()) << cluster.StatusSummary();
  ASSERT_NO_THROW(cluster.StopServer(*old_leader_index));

  auto new_leader_index = cluster.WaitForLeaderIndex(std::chrono::seconds(20));
  ASSERT_TRUE(new_leader_index.has_value()) << cluster.StatusSummary();
  ASSERT_NE(*new_leader_index, *old_leader_index);
  auto* new_leader = cluster.server(*new_leader_index);
  ASSERT_NE(new_leader, nullptr);

  auto leader_graph = new_leader->graph_manager()->OpenGraph(kGraphName);
  auto txn = leader_graph->BeginTransaction();
  auto vertex = txn->CreateVertex({"failover_label"}, kProperties);
  txn->Commit();

  const auto applied_index = leader_graph->GetRaftApplyIndex();
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(15)))
      << cluster.StatusSummary();

  ASSERT_NO_THROW(cluster.StartServer(*old_leader_index));
  ASSERT_TRUE(
      cluster.WaitForApplyIndex(applied_index, std::chrono::seconds(30)))
      << cluster.StatusSummary();

  auto final_leader_index =
      cluster.WaitForLeaderIndex(std::chrono::seconds(15));
  ASSERT_TRUE(final_leader_index.has_value()) << cluster.StatusSummary();
  for (auto* server : cluster.servers()) {
    auto graph = server->graph_manager()->OpenGraph(kGraphName);
    auto read_txn = graph->BeginTransaction();
    auto persisted_vertex = read_txn->GetVertexById(vertex.GetId());
    EXPECT_EQ(persisted_vertex.GetAllProperty(), kProperties);
    read_txn->Commit();
  }
}

TEST(RaftLogStorage, persistsHardStateAndEntriesAcrossReopen) {
  const std::string raft_path = "testdb_raft_log_storage_persistence";
  fs::remove_all(raft_path);

  {
    auto storage = OpenRaftLogStorage(raft_path);
    raftpb::ConfState conf_state;
    conf_state.add_voters(1);
    conf_state.add_voters(2);
    storage->SetInitialConfState(conf_state);
    ASSERT_FALSE(storage->Init());

    auto initial_state = storage->InitialState();
    EXPECT_EQ(std::get<2>(initial_state), nullptr);
    EXPECT_EQ(std::get<1>(initial_state).SerializeAsString(),
              conf_state.SerializeAsString());

    auto first_index = storage->FirstIndex();
    EXPECT_EQ(first_index.second, nullptr);
    EXPECT_EQ(first_index.first, 1U);
    auto last_index = storage->LastIndex();
    EXPECT_EQ(last_index.second, nullptr);
    EXPECT_EQ(last_index.first, 0U);
    raftpb::HardState hard_state;
    hard_state.set_term(3);
    hard_state.set_vote(1);
    hard_state.set_commit(2);

    rocksdb::WriteBatch wb;
    EXPECT_EQ(storage->SetHardState(hard_state, wb), nullptr);
    EXPECT_EQ(storage->Append(
                  {MakeLogEntry(1, 1, "one"), MakeLogEntry(2, 1, "two")}, wb),
              nullptr);
    storage->WriteBatch(wb);
    storage.Close();
  }

  {
    auto storage = OpenRaftLogStorage(raft_path);
    ASSERT_TRUE(storage->Init());

    auto [hard_state, conf_state, err] = storage->InitialState();
    EXPECT_EQ(err, nullptr);
    EXPECT_EQ(hard_state.term(), 3U);
    EXPECT_EQ(hard_state.vote(), 1U);
    EXPECT_EQ(hard_state.commit(), 2U);
    EXPECT_TRUE(conf_state.voters().empty());

    auto first_index = storage->FirstIndex();
    EXPECT_EQ(first_index.second, nullptr);
    EXPECT_EQ(first_index.first, 1U);
    auto last_index = storage->LastIndex();
    EXPECT_EQ(last_index.second, nullptr);
    EXPECT_EQ(last_index.first, 2U);

    auto term = storage->Term(2);
    EXPECT_EQ(term.second, nullptr);
    EXPECT_EQ(term.first, 1U);

    auto entries = storage->Entries(1, 3, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(entries.second, nullptr);
    ASSERT_EQ(entries.first.size(), 2U);
    EXPECT_EQ(entries.first[0].data(), "one");
    EXPECT_EQ(entries.first[1].data(), "two");

    auto snapshot = storage->Snapshot();
    EXPECT_EQ(snapshot.second, eraft::ErrSnapshotTemporarilyUnavailable);
  }

  fs::remove_all(raft_path);
}

TEST(RaftLogStorage, appendOverwritesConflictingSuffix) {
  const std::string raft_path = "testdb_raft_log_storage_append";
  fs::remove_all(raft_path);

  auto storage = OpenRaftLogStorage(raft_path);
  ASSERT_FALSE(storage->Init());

  rocksdb::WriteBatch first_wb;
  ASSERT_EQ(
      storage->Append({MakeLogEntry(1, 1, "one"), MakeLogEntry(2, 1, "two"),
                       MakeLogEntry(3, 1, "stale")},
                      first_wb),
      nullptr);
  storage->WriteBatch(first_wb);

  rocksdb::WriteBatch overwrite_wb;
  ASSERT_EQ(
      storage->Append({MakeLogEntry(2, 2, "two-overwritten")}, overwrite_wb),
      nullptr);
  storage->WriteBatch(overwrite_wb);

  auto last_index = storage->LastIndex();
  EXPECT_EQ(last_index.second, nullptr);
  EXPECT_EQ(last_index.first, 2U);

  auto term = storage->Term(2);
  EXPECT_EQ(term.second, nullptr);
  EXPECT_EQ(term.first, 2U);
  EXPECT_EQ(storage->Term(3).second, eraft::ErrUnavailable);

  auto entries = storage->Entries(1, 3, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(entries.second, nullptr);
  ASSERT_EQ(entries.first.size(), 2U);
  EXPECT_EQ(entries.first[0].data(), "one");
  EXPECT_EQ(entries.first[1].data(), "two-overwritten");

  auto limited_entries = storage->Entries(1, 3, 1);
  EXPECT_EQ(limited_entries.second, nullptr);
  ASSERT_EQ(limited_entries.first.size(), 1U);
  EXPECT_EQ(limited_entries.first[0].index(), 1U);

  storage.Close();
  fs::remove_all(raft_path);
}

TEST(RaftLogStorage, compactMovesFirstIndexAndRejectsCompactedEntries) {
  const std::string raft_path = "testdb_raft_log_storage_compact";
  fs::remove_all(raft_path);

  auto storage = OpenRaftLogStorage(raft_path);
  ASSERT_FALSE(storage->Init());

  rocksdb::WriteBatch wb;
  ASSERT_EQ(
      storage->Append({MakeLogEntry(1, 1, "one"), MakeLogEntry(2, 1, "two"),
                       MakeLogEntry(3, 2, "three"), MakeLogEntry(4, 2, "four"),
                       MakeLogEntry(5, 2, "five")},
                      wb),
      nullptr);
  storage->WriteBatch(wb);

  storage->Compact(3);

  auto first_index = storage->FirstIndex();
  EXPECT_EQ(first_index.second, nullptr);
  EXPECT_EQ(first_index.first, 4U);
  EXPECT_EQ(storage->Term(2).second, eraft::ErrCompacted);
  auto compacted_entries = storage->Entries(3, 5, 1024);
  EXPECT_EQ(compacted_entries.second, eraft::ErrCompacted);

  auto term = storage->Term(3);
  EXPECT_EQ(term.second, nullptr);
  EXPECT_EQ(term.first, 2U);

  auto entries = storage->Entries(4, 6, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(entries.second, nullptr);
  ASSERT_EQ(entries.first.size(), 2U);
  EXPECT_EQ(entries.first[0].data(), "four");
  EXPECT_EQ(entries.first[1].data(), "five");

  storage.Close();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, startupCompactsLogWhenAppliedIndexExceedsRetention) {
  const std::string raft_path = "testdb_raft_driver_compaction";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("driver_compaction_graph");
  constexpr uint64_t kAppliedIndex = 100001;
  constexpr uint64_t kLastIndex = kAppliedIndex + 1;
  raftpb::ConfState conf_state;
  conf_state.add_voters(1);
  meta::RaftNodeInfos node_infos;
  (*node_infos.mutable_nodes())[1] = MakeNodeInfo(local_node, 1);
  {
    auto storage = OpenRaftLogStorage(raft_path);
    ASSERT_FALSE(storage->Init());

    raftpb::HardState hard_state;
    hard_state.set_term(2);
    hard_state.set_vote(1);
    hard_state.set_commit(kAppliedIndex);

    std::vector<raftpb::Entry> entries;
    entries.reserve(kLastIndex);
    for (uint64_t i = 1; i <= kLastIndex; ++i) {
      entries.emplace_back(MakeLogEntry(i, 2));
    }

    rocksdb::WriteBatch wb;
    ASSERT_EQ(storage->SetHardState(hard_state, wb), nullptr);
    ASSERT_EQ(storage->Append(std::move(entries), wb), nullptr);
    storage->WriteBatch(wb);
    storage.Close();
  }

  auto store_config = MakeRaftLogStoreConfig(raft_path);
  store_config.keep_logs = 0;
  auto raft_config = MakeRaftConfig();

  TestRaftStateMachine state_machine;
  state_machine.SetAppliedIndex(kAppliedIndex);
  state_machine.ApplyConfChange(kAppliedIndex, conf_state, node_infos);
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(),
                          state_machine.AppliedIndex(),
                          state_machine.ConfState(), state_machine.NodeInfos(),
                          local_node, {}, store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }

  ASSERT_TRUE(WaitUntil(
      [&driver]() {
        auto status = driver.GetRaftStatus();
        return status.first_log >= kAppliedIndex;
      },
      std::chrono::seconds(5)));

  auto status = driver.GetRaftStatus();
  EXPECT_EQ(status.first_log, kAppliedIndex);
  EXPECT_GE(status.last_log, kLastIndex);

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, proposeWriteBatchTimesOutWhenApplyStalls) {
  const std::string raft_path = "testdb_raft_proposal_timeout";
  fs::remove_all(raft_path);

  raft::LocalNodeConfig local_node;
  local_node.graph = "proposal_timeout_graph";
  local_node.ip = "127.0.0.1";
  local_node.bolt_port = AllocateFreePort();
  local_node.raft_poft = AllocateFreePort();

  raft::RaftLogStoreConfig store_config;
  store_config.path = raft_path;
  store_config.shared_block_cache = rocksdb::NewLRUCache(64 * 1024 * 1024L);
  store_config.total_threads = 2;
  store_config.keep_logs = 100000;
  store_config.gc_interval = 1;

  raft::RaftConfig raft_config;
  raft_config.tick_interval = 100;
  raft_config.election_tick = 10;
  raft_config.heartbeat_tick = 1;
  raft_config.proposal_timeout = 50;

  meta::RaftNodeInfo node_info;
  node_info.set_node_id(1);
  node_info.set_graph(local_node.graph);
  node_info.set_ip(local_node.ip);
  node_info.set_bolt_port(local_node.bolt_port);
  node_info.set_raft_poft(local_node.raft_poft);

  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = 1;
  peer.context_ = node_info.SerializeAsString();
  init_peers.emplace_back(std::move(peer));

  std::atomic<uint64_t> applied_index{0};
  TestRaftStateMachine state_machine;
  raft::RaftDriver driver(
      [&applied_index](uint64_t index, const meta::RaftRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        applied_index.store(index);
      },
      state_machine.ConfChangeCallback(), 0, std::nullopt, std::nullopt,
      std::move(local_node), std::move(init_peers), store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!WaitUntil(
          [&driver]() {
            auto status = driver.GetRaftStatus();
            return status.s.basicStatus_.softState_.lead_ == 1 &&
                   status.s.basicStatus_.softState_.raftState_ ==
                       eraft::StateLeader;
          },
          std::chrono::seconds(5))) {
    auto status = driver.GetRaftStatus();
    driver.Stop();
    FAIL() << "raft driver did not become leader, lead="
           << status.s.basicStatus_.softState_.lead_;
  }

  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("k", "v").ok());
  auto result = driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
  EXPECT_NE(result.err, nullptr);
  EXPECT_NE(result.err.String().find("timed out"), std::string::npos);
  EXPECT_TRUE(WaitUntil([&applied_index]() { return applied_index.load() > 0; },
                        std::chrono::seconds(2)));

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, raftThreadRemainsResponsiveWhileApplyStalls) {
  const std::string raft_path = "testdb_raft_async_apply";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("async_apply_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();
  raft_config.proposal_timeout = 5000;

  meta::RaftNodeInfo node_info = MakeNodeInfo(local_node, 1);
  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = 1;
  peer.context_ = node_info.SerializeAsString();
  init_peers.emplace_back(std::move(peer));

  std::promise<void> apply_started;
  auto apply_started_future = apply_started.get_future();
  std::promise<void> release_apply;
  auto release_apply_future = release_apply.get_future().share();
  std::atomic<bool> first_graph_write{true};
  std::atomic<uint64_t> applied_index{0};
  TestRaftStateMachine state_machine;
  raft::RaftDriver driver(
      [&apply_started, &release_apply_future, &first_graph_write,
       &applied_index](uint64_t index, const meta::RaftRequest& request) {
        if (request.wb_kind() == meta::WriteBatchKind::GRAPH_WRITE &&
            first_graph_write.exchange(false)) {
          apply_started.set_value();
          release_apply_future.wait();
        }
        applied_index.store(index);
      },
      state_machine.ConfChangeCallback(), 0, std::nullopt, std::nullopt,
      std::move(local_node), std::move(init_peers), store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!testutil::WaitUntilRaftLeader(&driver)) {
    driver.Stop();
    FAIL() << "raft driver did not become leader";
  }

  rocksdb::WriteBatch first_wb;
  ASSERT_TRUE(first_wb.Put("first", "value").ok());
  auto first_result_future =
      std::async(std::launch::async, [&driver, &first_wb] {
        return driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE,
                                        first_wb);
      });

  const bool started = apply_started_future.wait_for(std::chrono::seconds(5)) ==
                       std::future_status::ready;
  std::future<raft::RaftStatus> status_future;
  bool status_ready = false;
  std::shared_ptr<raft::PromiseContext> second_context;
  std::future<raft::PromiseContext::CommitResult> second_committed_future;
  std::future<raft::PromiseContext::ApplyResult> second_applied_future;
  bool second_committed_before_release = false;
  if (started) {
    status_future = std::async(std::launch::async,
                               [&driver] { return driver.GetRaftStatus(); });
    status_ready = status_future.wait_for(std::chrono::milliseconds(500)) ==
                   std::future_status::ready;

    rocksdb::WriteBatch second_wb;
    EXPECT_TRUE(second_wb.Put("second", "value").ok());
    meta::RaftRequest second_request;
    second_request.set_wb_kind(meta::WriteBatchKind::GRAPH_WRITE);
    second_request.set_wb_data(second_wb.Data());
    second_context = driver.ProposeRaftRequest(std::move(second_request));
    second_committed_future = second_context->commited.get_future();
    second_applied_future = second_context->applied.get_future();
    second_committed_before_release =
        second_committed_future.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready;
  }
  release_apply.set_value();

  EXPECT_TRUE(started);
  EXPECT_TRUE(status_ready)
      << "Raft scheduler was blocked by the state-machine apply callback";
  EXPECT_TRUE(second_committed_before_release)
      << "Raft did not Advance after handing the first Ready to apply";
  if (status_future.valid()) {
    status_future.wait();
  }
  auto first_result = first_result_future.get();
  ASSERT_EQ(first_result.err, nullptr);
  if (second_context) {
    auto second_committed = second_committed_future.get();
    EXPECT_EQ(second_committed.err, nullptr);
    auto second_applied = second_applied_future.get();
    EXPECT_EQ(second_applied.err, nullptr);
    EXPECT_GT(second_applied.index, first_result.index);
    EXPECT_EQ(applied_index.load(), second_applied.index);
  }

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, startupRecoversMembershipWithoutWaitingForApply) {
  struct StartupState {
    bool has_logs;
    bool partially_applied;
  };
  const StartupState startup_states[] = {
      {false, false}, {true, false}, {true, true}};
  for (const auto& startup_state : startup_states) {
    const std::string raft_path =
        "testdb_raft_bootstrap_recovery_" +
        std::to_string(startup_state.has_logs) + "_" +
        std::to_string(startup_state.partially_applied);
    SCOPED_TRACE(raft_path);
    fs::remove_all(raft_path);

    auto local_node = MakeLocalNodeConfig("bootstrap_recovery_graph");
    local_node.node_id = 2;
    auto remote_node = MakeLocalNodeConfig(local_node.graph);
    auto peers = MakeInitPeers(remote_node, 1);
    peers.push_back(MakeInitPeers(local_node, 2).front());
    if (startup_state.has_logs) {
      auto storage = OpenRaftLogStorage(raft_path);
      ASSERT_FALSE(storage->Init());

      std::vector<raftpb::Entry> entries;
      for (const auto& peer : peers) {
        raftpb::ConfChange conf_change;
        conf_change.set_type(raftpb::ConfChangeAddNode);
        conf_change.set_node_id(peer.id_);
        conf_change.set_context(peer.context_);
        auto entry = MakeLogEntry(entries.size() + 1, 1,
                                  conf_change.SerializeAsString());
        entry.set_type(raftpb::EntryConfChange);
        entries.push_back(std::move(entry));
      }

      raftpb::HardState hard_state;
      hard_state.set_term(1);
      hard_state.set_commit(entries.size());
      rocksdb::WriteBatch batch;
      ASSERT_EQ(storage->Append(std::move(entries), batch), nullptr);
      ASSERT_EQ(storage->SetHardState(hard_state, batch), nullptr);
      storage->WriteBatch(batch);
      storage.Close();
    }

    TestRaftStateMachine state_machine;
    if (startup_state.partially_applied) {
      raftpb::ConfState conf_state;
      conf_state.add_voters(1);
      conf_state.add_voters(2);
      meta::RaftNodeInfos node_infos;
      (*node_infos.mutable_nodes())[1] = MakeNodeInfo(remote_node, 1);
      state_machine.ApplyConfChange(1, conf_state, node_infos);
    }
    std::promise<void> apply_started;
    auto apply_started_future = apply_started.get_future();
    std::promise<void> release_apply;
    auto release_apply_future = release_apply.get_future().share();
    std::atomic<bool> first_conf_change{true};
    raft::RaftDriver driver(
        [&state_machine](uint64_t index, const meta::RaftRequest&) {
          state_machine.SetAppliedIndex(index);
        },
        [&state_machine, &apply_started, &release_apply_future,
         &first_conf_change](uint64_t index,
                             const raftpb::ConfState& conf_state,
                             const meta::RaftNodeInfos& node_infos) {
          if (first_conf_change.exchange(false)) {
            apply_started.set_value();
            release_apply_future.wait();
          }
          state_machine.ApplyConfChange(index, conf_state, node_infos);
        },
        state_machine.AppliedIndex(),
        state_machine.ConfState(), state_machine.NodeInfos(), local_node,
        startup_state.has_logs ? std::vector<eraft::Peer>{} : peers,
        MakeRaftLogStoreConfig(raft_path), MakeRaftConfig());

    auto run_future =
        std::async(std::launch::async, [&driver] { return driver.Run(); });
    const bool started = apply_started_future.wait_for(std::chrono::seconds(5)) ==
                         std::future_status::ready;
    const bool run_returned = run_future.wait_for(std::chrono::seconds(5)) ==
                              std::future_status::ready;
    auto applied_before_release = state_machine.AppliedIndex();
    release_apply.set_value();
    auto err = run_future.get();
    if (err != nullptr) {
      driver.Stop();
      FAIL() << err.String();
    }
    auto status = driver.GetRaftStatus();
    auto recovered_infos = driver.GetNodeInfosWithLeader();
    driver.Stop();
    auto persisted_infos = state_machine.NodeInfos();
    auto recovered_conf_state = state_machine.ConfState();
    auto applied_index = state_machine.AppliedIndex();

    EXPECT_TRUE(started);
    EXPECT_TRUE(run_returned) << "startup waited for the apply callback";
    EXPECT_EQ(applied_before_release,
              startup_state.partially_applied ? 1U : 0U);
    EXPECT_EQ(status.s.basicStatus_.id_, 2U);
    EXPECT_EQ(applied_index, 2U);
    EXPECT_EQ(recovered_infos.nodes_size(), 2);
    EXPECT_EQ(recovered_infos.nodes().at(2).graph(), local_node.graph);
    ASSERT_TRUE(persisted_infos.has_value());
    EXPECT_EQ(persisted_infos->nodes_size(), 2);
    EXPECT_EQ(persisted_infos->nodes().at(2).graph(), local_node.graph);
    ASSERT_TRUE(recovered_conf_state.has_value());
    EXPECT_EQ(recovered_conf_state->voters_size(), 2);
    fs::remove_all(raft_path);
  }
}

TEST(RaftDriver, restartRecoversNodeInfosAndContinuesApplying) {
  const std::string raft_path = "testdb_raft_restart_recovery";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("restart_recovery_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();
  TestRaftStateMachine state_machine;

  uint64_t first_applied_index = 0;
  {
    std::atomic<uint64_t> applied_index{0};
    raft::RaftDriver driver(
        [&applied_index, &state_machine](uint64_t index,
                                         const meta::RaftRequest&) {
          applied_index.store(index);
          state_machine.SetAppliedIndex(index);
        },
        state_machine.ConfChangeCallback(), state_machine.AppliedIndex(),
        state_machine.ConfState(), state_machine.NodeInfos(), local_node,
        MakeInitPeers(local_node), store_config, raft_config);

    auto err = driver.Run();
    if (err != nullptr) {
      driver.Stop();
      FAIL() << err.String();
    }
    if (!WaitForRaftDriverLeader(&driver)) {
      auto status = driver.GetRaftStatus();
      driver.Stop();
      FAIL() << "raft driver did not become leader, lead="
             << status.s.basicStatus_.softState_.lead_;
    }

    rocksdb::WriteBatch wb;
    ASSERT_TRUE(wb.Put("k1", "v1").ok());
    auto result =
        driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
    if (result.err != nullptr) {
      driver.Stop();
      FAIL() << result.err.String();
    }
    if (result.index == 0) {
      driver.Stop();
      FAIL() << "raft proposal applied at invalid index 0";
    }
    first_applied_index = result.index;
    EXPECT_EQ(applied_index.load(), first_applied_index);

    driver.Stop();
  }

  {
    std::atomic<uint64_t> applied_index{first_applied_index};
    raft::RaftDriver driver(
        [&applied_index, &state_machine](uint64_t index,
                                         const meta::RaftRequest&) {
          applied_index.store(index);
          state_machine.SetAppliedIndex(index);
        },
        state_machine.ConfChangeCallback(), state_machine.AppliedIndex(),
        state_machine.ConfState(), state_machine.NodeInfos(), local_node,
        {}, store_config, raft_config);

    auto err = driver.Run();
    if (err != nullptr) {
      driver.Stop();
      FAIL() << err.String();
    }
    if (!WaitForRaftDriverLeader(&driver)) {
      auto status = driver.GetRaftStatus();
      driver.Stop();
      FAIL() << "raft driver did not become leader after restart, lead="
             << status.s.basicStatus_.softState_.lead_;
    }

    auto node_infos = driver.GetNodeInfosWithLeader();
    if (node_infos.nodes_size() != 1 || !node_infos.nodes().count(1)) {
      driver.Stop();
      FAIL() << "unexpected node infos after restart: "
             << node_infos.ShortDebugString();
    }
    EXPECT_TRUE(node_infos.nodes().at(1).is_leader());
    EXPECT_EQ(node_infos.nodes().at(1).graph(), local_node.graph);

    rocksdb::WriteBatch wb;
    ASSERT_TRUE(wb.Put("k2", "v2").ok());
    auto result =
        driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
    if (result.err != nullptr) {
      driver.Stop();
      FAIL() << result.err.String();
    }
    EXPECT_GT(result.index, first_applied_index);
    EXPECT_EQ(applied_index.load(), result.index);

    driver.Stop();
  }

  fs::remove_all(raft_path);
}

TEST(RaftDriver, freshStartWithoutInitialPeersSkipsBootstrap) {
  const std::string raft_path = "testdb_raft_missing_init_peers";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("missing_init_peers_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, local_node, {}, store_config,
                          raft_config);

  auto err = driver.Run();
  EXPECT_EQ(err, nullptr);
  if (err == nullptr) {
    auto status = driver.GetRaftStatus();
    EXPECT_EQ(status.s.basicStatus_.id_, local_node.node_id);
    EXPECT_EQ(status.last_log, 0U);
    EXPECT_EQ(status.s.basicStatus_.hardState_.commit(), 0U);
    EXPECT_TRUE(status.s.config_.voters_.IDs().empty());
    EXPECT_EQ(driver.GetNodeInfosWithLeader().nodes_size(), 0);
  }

  driver.Stop();
  EXPECT_EQ(state_machine.AppliedIndex(), 0U);
  EXPECT_FALSE(state_machine.ConfState().has_value());
  fs::remove_all(raft_path);
}

TEST(RaftDriver, freshStartWithoutInitialPeersUsesProvidedConfState) {
  const std::string raft_path = "testdb_raft_no_logs_with_node_infos";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("no_logs_with_node_infos_graph");
  raftpb::ConfState conf_state;
  conf_state.add_voters(local_node.node_id);
  meta::RaftNodeInfos node_infos;
  (*node_infos.mutable_nodes())[local_node.node_id] =
      MakeNodeInfo(local_node, local_node.node_id);
  TestRaftStateMachine state_machine;
  raft::RaftDriver driver(
      [](uint64_t, const meta::RaftRequest&) {},
      state_machine.ConfChangeCallback(), 0, conf_state, node_infos, local_node,
      {}, MakeRaftLogStoreConfig(raft_path), MakeRaftConfig());

  auto err = driver.Run();
  EXPECT_EQ(err, nullptr);
  if (err == nullptr) {
    auto status = driver.GetRaftStatus();
    EXPECT_EQ(status.last_log, 0U);
    EXPECT_EQ(status.s.config_.voters_.IDs().count(local_node.node_id), 1U);
    auto recovered_infos = driver.GetNodeInfosWithLeader();
    EXPECT_EQ(recovered_infos.nodes_size(), 1);
    EXPECT_EQ(recovered_infos.nodes().at(local_node.node_id).graph(),
              local_node.graph);
  }
  driver.Stop();
  EXPECT_FALSE(state_machine.ConfState().has_value());
  fs::remove_all(raft_path);
}

TEST(RaftDriver, configValidationRejectsInvalidValues) {
  auto raft_config = MakeRaftConfig();
  EXPECT_TRUE(raft_config.Check());

  auto invalid_raft_config = raft_config;
  invalid_raft_config.tick_interval = 99;
  EXPECT_FALSE(invalid_raft_config.Check());
  invalid_raft_config = raft_config;
  invalid_raft_config.heartbeat_tick = 0;
  EXPECT_FALSE(invalid_raft_config.Check());
  invalid_raft_config = raft_config;
  invalid_raft_config.election_tick = 9;
  EXPECT_FALSE(invalid_raft_config.Check());
  invalid_raft_config = raft_config;
  invalid_raft_config.proposal_timeout = 0;
  EXPECT_FALSE(invalid_raft_config.Check());
  invalid_raft_config = raft_config;
  invalid_raft_config.max_proposal_bytes = 0;
  EXPECT_FALSE(invalid_raft_config.Check());
  invalid_raft_config = raft_config;
  invalid_raft_config.max_pending_proposals = 0;
  EXPECT_FALSE(invalid_raft_config.Check());
  invalid_raft_config = raft_config;
  invalid_raft_config.max_pending_proposal_bytes =
      invalid_raft_config.max_proposal_bytes - 1;
  EXPECT_FALSE(invalid_raft_config.Check());

  auto store_config = MakeRaftLogStoreConfig("testdb_raft_config_validation");
  EXPECT_TRUE(store_config.Check());

  auto invalid_store_config = store_config;
  invalid_store_config.path.clear();
  EXPECT_FALSE(invalid_store_config.Check());
  invalid_store_config = store_config;
  invalid_store_config.shared_block_cache.reset();
  EXPECT_FALSE(invalid_store_config.Check());
  invalid_store_config = store_config;
  invalid_store_config.total_threads = 1;
  EXPECT_FALSE(invalid_store_config.Check());
  invalid_store_config = store_config;
  invalid_store_config.keep_logs = 99999;
  EXPECT_FALSE(invalid_store_config.Check());
  invalid_store_config = store_config;
  invalid_store_config.gc_interval = 0;
  EXPECT_FALSE(invalid_store_config.Check());

  auto local_node = MakeLocalNodeConfig("config_validation_graph");
  EXPECT_TRUE(local_node.Check());

  auto invalid_local_node = local_node;
  invalid_local_node.node_id = 0;
  EXPECT_FALSE(invalid_local_node.Check());
  invalid_local_node = local_node;
  invalid_local_node.graph.clear();
  EXPECT_FALSE(invalid_local_node.Check());
  invalid_local_node = local_node;
  invalid_local_node.ip.clear();
  EXPECT_FALSE(invalid_local_node.Check());
  invalid_local_node = local_node;
  invalid_local_node.bolt_port = 0;
  EXPECT_FALSE(invalid_local_node.Check());
  invalid_local_node = local_node;
  invalid_local_node.raft_poft = 0;
  EXPECT_FALSE(invalid_local_node.Check());
}

TEST(RaftDriver, freshBootstrapRejectsInvalidInitialPeerContext) {
  const std::string raft_path = "testdb_raft_invalid_peer_context";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("invalid_peer_context_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = 1;
  peer.context_ = "not a serialized raft node info";
  init_peers.emplace_back(std::move(peer));

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, local_node, std::move(init_peers),
                          store_config, raft_config);

  auto err = driver.Run();
  EXPECT_NE(err, nullptr);
  EXPECT_NE(err.String().find("failed to parse initial peer context"),
            std::string::npos);

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, freshBootstrapRejectsMissingLocalInitialPeer) {
  const std::string raft_path = "testdb_raft_missing_local_peer";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("missing_local_peer_graph");
  auto other_node = MakeLocalNodeConfig(local_node.graph);
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, local_node,
                          MakeInitPeers(other_node, 2), store_config,
                          raft_config);

  auto err = driver.Run();
  EXPECT_NE(err, nullptr);
  EXPECT_NE(err.String().find("not in initial peers"),
            std::string::npos);

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, freshBootstrapRejectsDuplicateLocalInitialPeers) {
  const std::string raft_path = "testdb_raft_duplicate_local_peers";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("duplicate_local_peers_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  auto init_peers = MakeInitPeers(local_node, 1);
  eraft::Peer duplicate_peer;
  duplicate_peer.id_ = 1;
  duplicate_peer.context_ = MakeNodeInfo(local_node, 1).SerializeAsString();
  init_peers.emplace_back(std::move(duplicate_peer));

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, local_node, std::move(init_peers),
                          store_config, raft_config);

  auto err = driver.Run();
  EXPECT_NE(err, nullptr);
  EXPECT_NE(err.String().find("duplicate initial peer id 1"),
            std::string::npos);

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, restartUsesConfiguredIdWithoutLocalMembership) {
  const std::string raft_path = "testdb_raft_restart_identity_mismatch";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("restart_identity_mismatch_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();
  TestRaftStateMachine state_machine;

  {
    raft::RaftDriver driver(
        [&state_machine](uint64_t index, const meta::RaftRequest&) {
          state_machine.SetAppliedIndex(index);
        },
        state_machine.ConfChangeCallback(), state_machine.AppliedIndex(),
        state_machine.ConfState(), state_machine.NodeInfos(), local_node,
        MakeInitPeers(local_node), store_config, raft_config);

    auto err = driver.Run();
    if (err != nullptr) {
      driver.Stop();
      FAIL() << err.String();
    }
    if (!WaitForRaftDriverLeader(&driver)) {
      auto status = driver.GetRaftStatus();
      driver.Stop();
      FAIL() << "raft driver did not become leader, lead="
             << status.s.basicStatus_.softState_.lead_;
    }
    driver.Stop();
  }

  auto mismatched_node = local_node;
  mismatched_node.node_id = 2;
  raft::RaftDriver driver(
      [&state_machine](uint64_t index, const meta::RaftRequest&) {
        state_machine.SetAppliedIndex(index);
      },
      state_machine.ConfChangeCallback(), state_machine.AppliedIndex(),
      state_machine.ConfState(), state_machine.NodeInfos(), mismatched_node,
      {}, store_config, raft_config);

  auto err = driver.Run();
  EXPECT_EQ(err, nullptr);
  if (err == nullptr) {
    EXPECT_EQ(driver.GetRaftStatus().s.basicStatus_.id_, mismatched_node.node_id);
  }

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, concurrentLeaderProposalsAllApply) {
  const std::string raft_path = "testdb_raft_concurrent_proposals";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("concurrent_proposals_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  std::atomic<uint64_t> applied_count{0};
  TestRaftStateMachine state_machine;
  raft::RaftDriver driver(
      [&applied_count, &state_machine](uint64_t index,
                                       const meta::RaftRequest& request) {
        state_machine.SetAppliedIndex(index);
        if (request.wb_kind() == meta::WriteBatchKind::GRAPH_WRITE) {
          applied_count.fetch_add(1);
        }
      },
      state_machine.ConfChangeCallback(), 0, std::nullopt, std::nullopt,
      local_node, MakeInitPeers(local_node), store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!WaitForRaftDriverLeader(&driver)) {
    auto status = driver.GetRaftStatus();
    driver.Stop();
    FAIL() << "raft driver did not become leader, lead="
           << status.s.basicStatus_.softState_.lead_;
  }

  constexpr int kProposalCount = 8;
  std::vector<std::future<raft::PromiseContext::ApplyResult>> futures;
  futures.reserve(kProposalCount);
  for (int i = 0; i < kProposalCount; ++i) {
    futures.emplace_back(std::async(std::launch::async, [&driver, i]() {
      rocksdb::WriteBatch wb;
      auto status =
          wb.Put("concurrent_key_" + std::to_string(i), "concurrent_value");
      if (!status.ok()) {
        throw std::runtime_error(status.ToString());
      }
      return driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
    }));
  }

  std::unordered_set<uint64_t> applied_indexes;
  for (auto& future : futures) {
    auto result = future.get();
    EXPECT_EQ(result.err, nullptr);
    EXPECT_GT(result.index, 0U);
    EXPECT_TRUE(applied_indexes.insert(result.index).second);
  }
  EXPECT_TRUE(WaitUntil(
      [&applied_count]() {
        return applied_count.load() == static_cast<uint64_t>(kProposalCount);
      },
      std::chrono::seconds(5)));

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, rejectUnknownWriteBatchKind) {
  auto local_node = MakeLocalNodeConfig("unknown_wb_kind_graph");
  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, std::move(local_node), {}, {},
                          MakeRaftConfig());

  meta::RaftRequest request;
  EXPECT_THROW(driver.ProposeRaftRequest(std::move(request)), std::exception);
}

TEST(RaftDriver, rejectWriteBatchAfterStop) {
  const std::string raft_path = "testdb_raft_proposal_after_stop";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("proposal_after_stop_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, local_node, MakeInitPeers(local_node),
                          store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!WaitForRaftDriverLeader(&driver)) {
    auto status = driver.GetRaftStatus();
    driver.Stop();
    FAIL() << "raft driver did not become leader, lead="
           << status.s.basicStatus_.softState_.lead_;
  }
  driver.Stop();

  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("after_stop_key", "value").ok());
  auto result = driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
  EXPECT_NE(result.err, nullptr);
  EXPECT_NE(result.err.String().find("raft driver stopped"), std::string::npos);
  EXPECT_EQ(result.index, 0U);

  fs::remove_all(raft_path);
}

TEST(RaftDriver, learnerConfChangeUpdatesNodeInfos) {
  const std::string raft_path = "testdb_raft_learner_confchange";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("learner_confchange_graph");
  auto learner_node = MakeLocalNodeConfig(local_node.graph);
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, local_node, MakeInitPeers(local_node),
                          store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!WaitForRaftDriverLeader(&driver)) {
    auto status = driver.GetRaftStatus();
    driver.Stop();
    FAIL() << "raft driver did not become leader, lead="
           << status.s.basicStatus_.softState_.lead_;
  }

  auto learner_info = MakeNodeInfo(learner_node, 2);
  raftpb::ConfChange add_learner;
  add_learner.set_type(raftpb::ConfChangeAddLearnerNode);
  add_learner.set_node_id(learner_info.node_id());
  add_learner.set_context(learner_info.SerializeAsString());

  auto add_result = WaitForAppliedResult(driver.ProposeConfChange(add_learner));
  if (add_result.err != nullptr) {
    driver.Stop();
    FAIL() << add_result.err.String();
  }
  EXPECT_GT(add_result.index, 0U);

  auto node_infos = driver.GetNodeInfosWithLeader();
  ASSERT_EQ(node_infos.nodes_size(), 2);
  ASSERT_TRUE(node_infos.nodes().count(1));
  ASSERT_TRUE(node_infos.nodes().count(2));
  EXPECT_TRUE(node_infos.nodes().at(1).is_leader());
  EXPECT_FALSE(node_infos.nodes().at(2).is_leader());
  EXPECT_EQ(node_infos.nodes().at(2).graph(), local_node.graph);

  raftpb::ConfChange remove_learner;
  remove_learner.set_type(raftpb::ConfChangeRemoveNode);
  remove_learner.set_node_id(learner_info.node_id());
  remove_learner.set_context(learner_info.SerializeAsString());

  auto remove_result =
      WaitForAppliedResult(driver.ProposeConfChange(remove_learner));
  if (remove_result.err != nullptr) {
    driver.Stop();
    FAIL() << remove_result.err.String();
  }
  EXPECT_GT(remove_result.index, add_result.index);

  node_infos = driver.GetNodeInfosWithLeader();
  ASSERT_EQ(node_infos.nodes_size(), 1);
  ASSERT_TRUE(node_infos.nodes().count(1));
  EXPECT_FALSE(node_infos.nodes().count(2));
  EXPECT_TRUE(node_infos.nodes().at(1).is_leader());

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, restartUsesConfiguredIdWithDuplicateAddresses) {
  const std::string raft_path = "testdb_raft_restart_duplicate_node_infos";
  fs::remove_all(raft_path);

  auto local_node = MakeLocalNodeConfig("restart_duplicate_node_infos_graph");
  auto store_config = MakeRaftLogStoreConfig(raft_path);
  auto raft_config = MakeRaftConfig();
  TestRaftStateMachine state_machine;

  {
    raft::RaftDriver driver(
        [&state_machine](uint64_t index, const meta::RaftRequest&) {
          state_machine.SetAppliedIndex(index);
        },
        state_machine.ConfChangeCallback(), state_machine.AppliedIndex(),
        state_machine.ConfState(), state_machine.NodeInfos(), local_node,
        MakeInitPeers(local_node), store_config, raft_config);

    auto err = driver.Run();
    if (err != nullptr) {
      driver.Stop();
      FAIL() << err.String();
    }
    if (!WaitForRaftDriverLeader(&driver)) {
      auto status = driver.GetRaftStatus();
      driver.Stop();
      FAIL() << "raft driver did not become leader, lead="
             << status.s.basicStatus_.softState_.lead_;
    }

    auto duplicate_info = MakeNodeInfo(local_node, 2);
    raftpb::ConfChange add_duplicate_learner;
    add_duplicate_learner.set_type(raftpb::ConfChangeAddLearnerNode);
    add_duplicate_learner.set_node_id(duplicate_info.node_id());
    add_duplicate_learner.set_context(duplicate_info.SerializeAsString());

    auto result =
        WaitForAppliedResult(driver.ProposeConfChange(add_duplicate_learner));
    if (result.err != nullptr) {
      driver.Stop();
      FAIL() << result.err.String();
    }
    driver.Stop();
  }

  raft::RaftDriver driver(
      [&state_machine](uint64_t index, const meta::RaftRequest&) {
        state_machine.SetAppliedIndex(index);
      },
      state_machine.ConfChangeCallback(), state_machine.AppliedIndex(),
      state_machine.ConfState(), state_machine.NodeInfos(), local_node,
      {}, store_config, raft_config);

  auto err = driver.Run();
  EXPECT_EQ(err, nullptr);
  if (err == nullptr) {
    EXPECT_EQ(driver.GetRaftStatus().s.basicStatus_.id_, local_node.node_id);
  }

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, rejectOversizedWriteBatch) {
  raft::LocalNodeConfig local_node;
  local_node.graph = "oversized_proposal_graph";
  local_node.ip = "127.0.0.1";
  local_node.bolt_port = 1;
  local_node.raft_poft = 1;

  raft::RaftConfig raft_config;
  raft_config.max_proposal_bytes = 32;

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver([](uint64_t, const meta::RaftRequest&) {},
                          state_machine.ConfChangeCallback(), 0, std::nullopt,
                          std::nullopt, std::move(local_node), {}, {},
                          raft_config);

  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("k", std::string(1024, 'v')).ok());
  auto result = driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
  EXPECT_NE(result.err, nullptr);
  EXPECT_NE(result.err.String().find("too large"), std::string::npos);
}

TEST(RaftDriver, rejectWriteBatchWhenPendingQueueIsFull) {
  const std::string raft_path = "testdb_raft_proposal_backpressure";
  fs::remove_all(raft_path);

  raft::LocalNodeConfig local_node;
  local_node.graph = "proposal_backpressure_graph";
  local_node.ip = "127.0.0.1";
  local_node.bolt_port = AllocateFreePort();
  local_node.raft_poft = AllocateFreePort();

  raft::RaftLogStoreConfig store_config;
  store_config.path = raft_path;
  store_config.shared_block_cache = rocksdb::NewLRUCache(64 * 1024 * 1024L);
  store_config.total_threads = 2;
  store_config.keep_logs = 100000;
  store_config.gc_interval = 1;

  raft::RaftConfig raft_config;
  raft_config.tick_interval = 100;
  raft_config.election_tick = 10;
  raft_config.heartbeat_tick = 1;
  raft_config.max_pending_proposals = 1;

  meta::RaftNodeInfo node_info;
  node_info.set_node_id(1);
  node_info.set_graph(local_node.graph);
  node_info.set_ip(local_node.ip);
  node_info.set_bolt_port(local_node.bolt_port);
  node_info.set_raft_poft(local_node.raft_poft);

  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = 1;
  peer.context_ = node_info.SerializeAsString();
  init_peers.emplace_back(std::move(peer));

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver(
      [](uint64_t, const meta::RaftRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      },
      state_machine.ConfChangeCallback(), 0, std::nullopt, std::nullopt,
      std::move(local_node), std::move(init_peers), store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!WaitUntil(
          [&driver]() {
            auto status = driver.GetRaftStatus();
            return status.s.basicStatus_.softState_.lead_ == 1 &&
                   status.s.basicStatus_.softState_.raftState_ ==
                       eraft::StateLeader;
          },
          std::chrono::seconds(5))) {
    auto status = driver.GetRaftStatus();
    driver.Stop();
    FAIL() << "raft driver did not become leader, lead="
           << status.s.basicStatus_.softState_.lead_;
  }

  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("k", "v").ok());
  meta::RaftRequest first_request;
  first_request.set_wb_kind(meta::WriteBatchKind::GRAPH_WRITE);
  first_request.set_wb_data(wb.Data());
  auto first_context = driver.ProposeRaftRequest(std::move(first_request));

  auto second_result =
      driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
  EXPECT_NE(second_result.err, nullptr);
  EXPECT_NE(second_result.err.String().find("backpressure"), std::string::npos);

  auto first_future = first_context->applied.get_future();
  ASSERT_EQ(first_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(first_future.get().err, nullptr);

  driver.Stop();
  fs::remove_all(raft_path);
}

TEST(RaftDriver, rejectWriteBatchWhenPendingBytesAreFull) {
  const std::string raft_path = "testdb_raft_proposal_bytes_backpressure";
  fs::remove_all(raft_path);

  raft::LocalNodeConfig local_node;
  local_node.graph = "proposal_bytes_backpressure_graph";
  local_node.ip = "127.0.0.1";
  local_node.bolt_port = AllocateFreePort();
  local_node.raft_poft = AllocateFreePort();

  raft::RaftLogStoreConfig store_config;
  store_config.path = raft_path;
  store_config.shared_block_cache = rocksdb::NewLRUCache(64 * 1024 * 1024L);
  store_config.total_threads = 2;
  store_config.keep_logs = 100000;
  store_config.gc_interval = 1;

  raft::RaftConfig raft_config;
  raft_config.tick_interval = 100;
  raft_config.election_tick = 10;
  raft_config.heartbeat_tick = 1;
  raft_config.max_proposal_bytes = 700 * 1024;
  raft_config.max_pending_proposal_bytes = 800 * 1024;

  meta::RaftNodeInfo node_info;
  node_info.set_node_id(1);
  node_info.set_graph(local_node.graph);
  node_info.set_ip(local_node.ip);
  node_info.set_bolt_port(local_node.bolt_port);
  node_info.set_raft_poft(local_node.raft_poft);

  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = 1;
  peer.context_ = node_info.SerializeAsString();
  init_peers.emplace_back(std::move(peer));

  TestRaftStateMachine state_machine;
  raft::RaftDriver driver(
      [](uint64_t, const meta::RaftRequest&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      },
      state_machine.ConfChangeCallback(), 0, std::nullopt, std::nullopt,
      std::move(local_node), std::move(init_peers), store_config, raft_config);

  auto err = driver.Run();
  if (err != nullptr) {
    driver.Stop();
    FAIL() << err.String();
  }
  if (!WaitUntil(
          [&driver]() {
            auto status = driver.GetRaftStatus();
            return status.s.basicStatus_.softState_.lead_ == 1 &&
                   status.s.basicStatus_.softState_.raftState_ ==
                       eraft::StateLeader;
          },
          std::chrono::seconds(5))) {
    auto status = driver.GetRaftStatus();
    driver.Stop();
    FAIL() << "raft driver did not become leader, lead="
           << status.s.basicStatus_.softState_.lead_;
  }

  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("k", std::string(512 * 1024, 'v')).ok());
  meta::RaftRequest first_request;
  first_request.set_wb_kind(meta::WriteBatchKind::GRAPH_WRITE);
  first_request.set_wb_data(wb.Data());
  auto first_context = driver.ProposeRaftRequest(std::move(first_request));

  auto second_result =
      driver.ProposeWriteBatch(meta::WriteBatchKind::GRAPH_WRITE, wb);
  EXPECT_NE(second_result.err, nullptr);
  EXPECT_NE(second_result.err.String().find("pending proposal bytes"),
            std::string::npos);

  auto first_future = first_context->applied.get_future();
  ASSERT_EQ(first_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(first_future.get().err, nullptr);

  driver.Stop();
  fs::remove_all(raft_path);
}

}  // namespace
