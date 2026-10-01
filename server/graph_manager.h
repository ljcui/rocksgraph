#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "graphdb/graph_db.h"
#include "runtime/graph_management.h"

namespace rg {
class PlanCache;
}

namespace server {
enum class GraphManagerMetadataType : char {
  GraphDB = 0,
  NextGraphID = 1,
};

struct GraphManagerOptions {
  size_t block_cache_size = 64 * 1024 * 1024L;
  size_t raft_log_block_cache_size = 256 * 1024 * 1024L;
  size_t raft_scheduler_shards = 4;
  size_t assistant_thread_num = 4;
  size_t ft_apply_interval = 1;
  size_t ft_writer_threads = 1;
  size_t ft_writer_memory_budget = 50 * 1000 * 1000;
  size_t vt_apply_interval = 1;
  size_t plan_cache_capacity = 1024;
};

struct LocalNodeOptions {
  std::string host = "127.0.0.1";
  uint32_t bolt_port = 0;
  uint32_t raft_port = 0;
  uint64_t raft_node_id = 1;
};

class GraphManager final : public rg::GraphManagement {
 public:
  GraphManager() = default;
  ~GraphManager();
  // No copying allowed
  GraphManager(const GraphManager&) = delete;
  void operator=(const GraphManager&) = delete;

  static std::unique_ptr<GraphManager> Open(
      const std::string& path, const GraphManagerOptions& graph_manager_options,
      LocalNodeOptions local_node_options = {});
  std::shared_ptr<graphdb::GraphDB> OpenGraph(const std::string& name);
  [[nodiscard]] rg::PlanCache& GetPlanCache() noexcept;
  graphdb::GraphDB* CreateGraph(const std::string& name);
  graphdb::GraphDB* CreateGraphWithRaft(const std::string& name,
                                        const meta::RaftNodeInfos& node_infos);
  graphdb::GraphDB* ClearGraph(const std::string& name);
  void DeleteGraph(const std::string& name);
  const std::unordered_map<std::string, std::shared_ptr<graphdb::GraphDB>>&
  Graphs() {
    return graphs_;
  }

  void CreateManagedGraph(std::string_view name) override;
  void CreateManagedRaftGraph(std::string_view name,
                              const meta::RaftNodeInfos& node_infos) override;
  void ClearManagedGraph(std::string_view name) override;
  void DeleteManagedGraph(std::string_view name) override;
  [[nodiscard]] std::vector<rg::ManagedGraphInfo> ListManagedGraphs()
      const override;
  [[nodiscard]] meta::RaftNodeInfos ManagedGraphRaftNodeInfos(
      std::string_view name) override;
  rg::ManagedRaftChangeResult AddManagedRaftNode(
      std::string_view name, const meta::RaftNodeInfo& node_info,
      bool learner) override;
  rg::ManagedRaftChangeResult PromoteManagedRaftLearnerNode(
      std::string_view name, std::uint64_t node_id) override;
  rg::ManagedRaftChangeResult RemoveManagedRaftNode(
      std::string_view name, std::uint64_t node_id) override;
  void TransferManagedRaftLeader(std::string_view name,
                                 std::uint64_t node_id) override;
  rg::ManagedRaftChangeResult DemoteManagedRaftNode(
      std::string_view name, std::uint64_t node_id) override;
  rg::ManagedRaftChangeResult UpdateManagedRaftNode(
      std::string_view name, const meta::RaftNodeInfo& node_info) override;
  [[nodiscard]] rg::ManagedRaftStatus ManagedGraphRaftStatus(
      std::string_view name) override;

 private:
  graphdb::GraphDB* CreateGraphInternal(const std::string& name,
                                        const meta::RaftNodeInfos* node_infos);
  graphdb::GraphDB* CreateGraphWithId(const meta::GraphDBMetaInfo& meta,
                                      const meta::RaftNodeInfos* node_infos);
  void StartGraphRaft(graphdb::GraphDB* graph_db,
                      const meta::RaftNodeInfos* node_infos);
  rocksdb::TransactionDB* meta_db_ = nullptr;
  std::unordered_map<std::string, std::shared_ptr<graphdb::GraphDB>> graphs_;
  std::shared_ptr<rocksdb::Cache> block_cache_;
  std::shared_ptr<rocksdb::Cache> raft_log_block_cache_;
  std::shared_ptr<graphdb::AssistantPool> assistant_pool_;
  std::unique_ptr<rg::PlanCache> plan_cache_;
  mutable std::shared_mutex graphs_mutex_;
  std::mutex create_graph_mutex_;
  std::atomic<uint64_t> next_graph_id_ = 1;
  std::string path_;
  GraphManagerOptions options_;
  LocalNodeOptions local_node_options_;
};
}  // namespace server
