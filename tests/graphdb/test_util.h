#pragma once

#include <rocksdb/cache.h>
#include <rocksdb/write_batch.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>

#include "graphdb/graph_db.h"
#include "proto/meta.pb.h"
#include "raft_driver/raft_driver.h"

#define EXPECT_THROW_CODE(statement, error_code)                \
  {                                                             \
    try {                                                       \
      statement;                                                \
      FAIL() << "Expecting exception, but nothing is thrown.";  \
    } catch (LgraphException & e) {                             \
      if (e.code() != ErrorCode::error_code) {                  \
        FAIL() << "Unexpected exception message: " << e.what(); \
      } else {                                                  \
        SUCCEED() << "Expected exception: " << e.what();        \
      }                                                         \
    } catch (std::exception & e) {                              \
      FAIL() << "Unexpected exception message: " << e.what();   \
    }                                                           \
  }

#define EXPECT_THROW_CODE_MSG(statement, error_code, msg)       \
  {                                                             \
    try {                                                       \
      statement;                                                \
      FAIL() << "Expecting exception, but nothing is thrown.";  \
    } catch (LgraphException & e) {                             \
      if (e.code() != ErrorCode::error_code) {                  \
        FAIL() << "Unexpected exception message: " << e.what(); \
      } else {                                                  \
        std::string what = e.what();                            \
        if (what.find(msg) != what.npos) {                      \
          SUCCEED() << "Expected exception: " << e.what();      \
        } else {                                                \
          FAIL() << "Unexpected exception message: " << what;   \
        }                                                       \
      }                                                         \
    } catch (std::exception & e) {                              \
      FAIL() << "Unexpected exception message: " << e.what();   \
    }                                                           \
  }

inline bool WaitUntilPropertyIndexReady(
    graphdb::GraphDB* graph_db, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (graph_db->meta_info().GetReadyVertexPropertyIndex(index_name)) {
      return true;
    }
    auto index = graph_db->meta_info().GetVertexPropertyIndex(index_name);
    if (index && index->state() == meta::IndexBuildState::FAILED) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

inline bool WaitUntilPropertyIndexFailed(
    graphdb::GraphDB* graph_db, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto index = graph_db->meta_info().GetVertexPropertyIndex(index_name);
    if (index && index->state() == meta::IndexBuildState::FAILED) {
      return true;
    }
    if (graph_db->meta_info().GetReadyVertexPropertyIndex(index_name)) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

namespace testutil {

inline constexpr std::array<std::string_view, 8> kTestDataDirectories = {
    "testdb",       "cypher_testdb", "temporal_db", "test_galaxy",
    "test_ftindex", "testkv",        "varlendb",    "testdb_raft_cluster"};

inline void CleanupTestDataDirectories() {
  std::error_code ec;
  for (const auto dir : kTestDataDirectories) {
    ec.clear();
    std::filesystem::remove_all(std::filesystem::path(dir), ec);
  }
}

inline graphdb::GraphDBOptions NewGraphDBOptions() {
  graphdb::GraphDBOptions options;
  options.assistant_pool = std::make_shared<graphdb::AssistantPool>(1);
  return options;
}

inline void ApplyRaftRequest(graphdb::GraphDB* graph_db, uint64_t index,
                             const meta::RaftRequest& request) {
  graph_db->ApplyRaftRequest(index, request);
}

inline std::unique_ptr<raft::RaftDriver> NewSingleNodeRaftDriver(
    graphdb::GraphDB* graph_db, const std::string& graph_name,
    const std::string& raft_path, int32_t bolt_port, int32_t raft_port) {
  raft::LocalNodeConfig local_node;
  local_node.graph = graph_name;
  local_node.ip = "127.0.0.1";
  local_node.bolt_port = bolt_port;
  local_node.raft_poft = raft_port;

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

  meta::RaftNodeInfo node_info;
  node_info.set_node_id(1);
  node_info.set_graph(graph_name);
  node_info.set_ip(local_node.ip);
  node_info.set_bolt_port(local_node.bolt_port);
  node_info.set_raft_poft(local_node.raft_poft);

  std::vector<eraft::Peer> init_peers;
  eraft::Peer peer;
  peer.id_ = 1;
  peer.context_ = node_info.SerializeAsString();
  init_peers.emplace_back(std::move(peer));

  return std::make_unique<raft::RaftDriver>(
      [graph_db](uint64_t index, const meta::RaftRequest& request) {
        ApplyRaftRequest(graph_db, index, request);
      },
      graph_db->GetRaftApplyIndex(), std::move(local_node),
      std::move(init_peers), store_config, raft_config);
}

inline bool WaitUntilRaftLeader(
    raft::RaftDriver* raft_driver,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = raft_driver->GetRaftStatus();
    if (status.s.basicStatus_.softState_.lead_ == 1) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace testutil
