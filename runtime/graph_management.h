#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "proto/graph_replication.pb.h"

namespace rg {

struct ManagedGraphInfo {
  std::uint64_t id = 0;
  std::string name;
};

struct ManagedRaftNodeStatus {
  std::uint64_t node_id = 0;
  std::string ip;
  std::int32_t bolt_port = 0;
  std::int32_t raft_port = 0;
  bool is_leader = false;
  bool is_learner = false;
  bool reachable = false;
  std::uint64_t match_index = 0;
  std::uint64_t next_index = 0;
};

struct ManagedRaftStatus {
  std::uint64_t local_node_id = 0;
  std::uint64_t leader_id = 0;
  std::uint64_t term = 0;
  std::uint64_t commit_index = 0;
  std::uint64_t applied_index = 0;
  std::uint64_t first_log = 0;
  std::uint64_t last_log = 0;
  std::string raft_state;
  std::vector<ManagedRaftNodeStatus> nodes;
};

struct ManagedRaftChangeResult {
  std::uint64_t node_id = 0;
  std::uint64_t raft_index = 0;
  bool is_learner = false;
};

class GraphManagement {
 public:
  GraphManagement() = default;
  GraphManagement(const GraphManagement&) = delete;
  GraphManagement& operator=(const GraphManagement&) = delete;
  virtual ~GraphManagement() = default;

  virtual void CreateManagedGraph(std::string_view name) = 0;
  virtual void CreateManagedRaftGraph(
      std::string_view name, const meta::RaftNodeInfos& node_infos) = 0;
  virtual void ClearManagedGraph(std::string_view name) = 0;
  virtual void DeleteManagedGraph(std::string_view name) = 0;
  [[nodiscard]] virtual std::vector<ManagedGraphInfo> ListManagedGraphs()
      const = 0;
  [[nodiscard]] virtual meta::RaftNodeInfos ManagedGraphRaftNodeInfos(
      std::string_view name) = 0;
  virtual ManagedRaftChangeResult AddManagedRaftNode(
      std::string_view name, const meta::RaftNodeInfo& node_info,
      bool learner) = 0;
  virtual ManagedRaftChangeResult PromoteManagedRaftLearnerNode(
      std::string_view name, std::uint64_t node_id) = 0;
  virtual ManagedRaftChangeResult RemoveManagedRaftNode(
      std::string_view name, std::uint64_t node_id) = 0;
  virtual void TransferManagedRaftLeader(std::string_view name,
                                         std::uint64_t node_id) = 0;
  virtual ManagedRaftChangeResult DemoteManagedRaftNode(
      std::string_view name, std::uint64_t node_id) = 0;
  virtual ManagedRaftChangeResult UpdateManagedRaftNode(
      std::string_view name, const meta::RaftNodeInfo& node_info) = 0;
  [[nodiscard]] virtual ManagedRaftStatus ManagedGraphRaftStatus(
      std::string_view name) = 0;
};

}  // namespace rg
