#include "server/graph_manager.h"

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <filesystem>
#include <utility>

#include "common/exception.h"
#include "common/logger.h"
#include "runtime/plan_cache.h"
using namespace graphdb;
using namespace boost::endian;
namespace server {
namespace {

constexpr std::string_view kSystemGraphName = "system";

std::string BuildGraphMetaKey(uint64_t graph_id) {
  std::string key;
  key.append(1, static_cast<char>(GraphManagerMetadataType::GraphDB));
  native_to_big_inplace(graph_id);
  key.append((const char *)&graph_id, sizeof(graph_id));
  return key;
}

std::string BuildGraphManagerMetaKey(GraphManagerMetadataType type) {
  return std::string(1, static_cast<char>(type));
}

void ValidateRaftNodeInfo(const meta::RaftNodeInfo &node_info,
                          std::string_view graph_name) {
  if (node_info.node_id() == 0) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node_id should be greater than 0");
  }
  if (node_info.ip().empty()) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node [{}] ip should not be empty", node_info.node_id());
  }
  if (node_info.bolt_port() <= 0) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node [{}] bolt_port should be greater than 0",
             node_info.node_id());
  }
  if (node_info.raft_poft() <= 0) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node [{}] raft_port should be greater than 0",
             node_info.node_id());
  }
  if (node_info.graph().empty()) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node [{}] graph should not be empty", node_info.node_id());
  }
  if (!graph_name.empty() && node_info.graph() != graph_name) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node [{}] graph [{}] does not match graph [{}]",
             node_info.node_id(), node_info.graph(), graph_name);
  }
}

void ValidateRaftNodeInfos(const meta::RaftNodeInfos &node_infos,
                           std::string_view graph_name) {
  if (node_infos.nodes().empty()) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "raft node infos should not be empty");
  }
  for (const auto &[node_id, node_info] : node_infos.nodes()) {
    if (node_info.node_id() != node_id) {
      RG_THROW(common::ErrorCode::InvalidParameter,
               "raft node info key [{}] does not match node_id [{}]", node_id,
               node_info.node_id());
    }
    ValidateRaftNodeInfo(node_info, graph_name);
  }
}

void ValidateGraphNameForCreate(std::string_view name) {
  if (name.empty()) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "graph name should not be empty");
  }
  if (name == kSystemGraphName) {
    RG_THROW(common::ErrorCode::InvalidParameter, "graph name [{}] is reserved",
             name);
  }
}

void ApplyRaftRequest(GraphDB *graph_db, uint64_t index,
                      const meta::RaftRequest &request) {
  graph_db->ApplyRaftRequest(index, request);
}

raft_driver::LocalNodeConfig BuildLocalNodeConfig(
    const std::string &graph_name, const LocalNodeOptions &local_node_options) {
  raft_driver::LocalNodeConfig local_node;
  local_node.node_id = local_node_options.raft_node_id;
  local_node.graph = graph_name;
  local_node.ip = local_node_options.host;
  local_node.bolt_port = static_cast<int32_t>(local_node_options.bolt_port);
  local_node.raft_poft = static_cast<int32_t>(local_node_options.raft_port);
  local_node.http_port = local_node_options.http_port;
  return local_node;
}

raft_driver::RaftLogStoreConfig BuildRaftLogStoreConfig(
    const std::string &path,
    std::shared_ptr<rocksdb::Cache> shared_block_cache) {
  raft_driver::RaftLogStoreConfig store_config;
  store_config.path = path;
  store_config.shared_block_cache = std::move(shared_block_cache);
  store_config.total_threads = 2;
  store_config.keep_logs = 100000;
  store_config.gc_interval = 1;
  return store_config;
}

raft_driver::RaftConfig BuildRaftConfig() {
  raft_driver::RaftConfig raft_config;
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

std::uint64_t ProposeRaftConfChange(raft_driver::RaftDriver *driver,
                                    raftpb::ConfChange conf_change) {
  const auto result = driver->ProposeConfChangeAndWait(std::move(conf_change));
  if (result.err != nullptr) {
    RG_THROW(result.error_code, "raft configuration change failed: {}",
             result.err.String());
  }
  return result.index;
}

}  // namespace

GraphManager::~GraphManager() {
  graphs_.clear();
  auto s = meta_db_->Close();
  if (!s.ok()) {
    LOG_WARN("meta db close error : {}", s.ToString());
  }
  delete meta_db_;
  meta_db_ = nullptr;
  LOG_INFO("Close graph manager");
}

std::unique_ptr<GraphManager> GraphManager::Open(
    const std::string &path, const GraphManagerOptions &graph_manager_options,
    LocalNodeOptions local_node_options) {
  RG_CHECK(local_node_options.raft_node_id != 0,
           common::ErrorCode::InvalidParameter,
           "raft_node_id should be greater than 0");
  LOG_INFO("Open graph manager: {}", path);
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;
  rocksdb::TransactionDBOptions txn_db_options;

  std::string meta_path = path + "/meta";
  std::filesystem::create_directories(meta_path);
  rocksdb::TransactionDB *db = nullptr;
  auto s =
      rocksdb::TransactionDB::Open(options, txn_db_options, meta_path, &db);
  if (!s.ok()) RG_THROW(common::ErrorCode::StorageEngineError, s.ToString());
  auto graph_manager = std::make_unique<GraphManager>();
  graph_manager->path_ = path;
  raft_driver::RaftManager::Configure(
      graph_manager_options.raft_scheduler_shards);
  graph_manager->block_cache_ =
      rocksdb::NewLRUCache(graph_manager_options.block_cache_size);
  graph_manager->options_ = graph_manager_options;
  graph_manager->local_node_options_ = std::move(local_node_options);
  graph_manager->meta_db_ = db;
  graph_manager->raft_log_block_cache_ =
      rocksdb::NewLRUCache(graph_manager_options.raft_log_block_cache_size);
  graph_manager->assistant_pool_ = std::make_shared<graphdb::AssistantPool>(
      graph_manager_options.assistant_thread_num);
  graph_manager->plan_cache_ = std::make_unique<runtime::PlanCache>(
      graph_manager_options.plan_cache_capacity);

  rocksdb::ReadOptions ro;
  {
    std::string next_graph_id_key(
        1, static_cast<char>(GraphManagerMetadataType::NextGraphID));
    std::string val;
    s = graph_manager->meta_db_->Get(ro, next_graph_id_key, &val);
    if (s.ok()) {
      graph_manager->next_graph_id_ = *(uint64_t *)val.data();
    } else if (s.IsNotFound()) {
      graph_manager->next_graph_id_ = 1;
    } else {
      RG_THROW(common::ErrorCode::StorageEngineError, s.ToString());
    }
  }
  std::unique_ptr<rocksdb::Iterator> iter(
      graph_manager->meta_db_->NewIterator(ro));
  std::string prefix;
  prefix.append(1, static_cast<char>(GraphManagerMetadataType::GraphDB));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto val = iter->value();
    meta::GraphDBMetaInfo meta;
    bool f = meta.ParseFromString(val.ToString());
    assert(f);
    LOG_INFO("Load GraphDB: [{}]", meta.ShortDebugString());
    std::string graph_path =
        graph_manager->path_ + "/graph" + std::to_string(meta.graph_id());
    auto graph_db = GraphDB::Open(
        graph_path,
        {.block_cache = graph_manager->block_cache_,
         .assistant_pool = graph_manager->assistant_pool_,
         .ft_apply_interval_ = graph_manager->options_.ft_apply_interval,
         .ft_writer_threads_ = graph_manager->options_.ft_writer_threads,
         .ft_writer_memory_budget_ =
             graph_manager->options_.ft_writer_memory_budget,
         .vt_apply_interval_ = graph_manager->options_.vt_apply_interval});
    graph_db->db_meta() = meta;
    if (meta.enable_raft()) {
      graph_manager->StartGraphRaft(graph_db.get(), nullptr);
    }
    graph_manager->graphs_.emplace(meta.graph_name(), std::move(graph_db));
  }
  if (graph_manager->graphs_.empty()) {
    graph_manager->CreateGraph("default");
  }
  return graph_manager;
}

std::shared_ptr<GraphDB> GraphManager::OpenGraph(const std::string &name) {
  std::shared_lock<std::shared_mutex> read_lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  if (iter != graphs_.end()) {
    return iter->second;
  } else {
    RG_THROW(common::ErrorCode::NoSuchGraph, "No such graph: {}", name);
  }
}

runtime::PlanCache &GraphManager::GetPlanCache() noexcept {
  return *plan_cache_;
}

GraphDB *GraphManager::CreateGraph(const std::string &name) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  return CreateGraphInternal(name, nullptr);
}

GraphDB *GraphManager::CreateGraphWithRaft(
    const std::string &name, const meta::RaftNodeInfos &node_infos) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  ValidateGraphNameForCreate(name);
  ValidateRaftNodeInfos(node_infos, name);
  return CreateGraphInternal(name, &node_infos);
}

GraphDB *GraphManager::CreateGraphInternal(
    const std::string &name, const meta::RaftNodeInfos *node_infos) {
  ValidateGraphNameForCreate(name);
  meta::GraphDBMetaInfo meta;
  uint64_t graph_id = next_graph_id_.load();
  meta.set_graph_id(graph_id);
  meta.set_graph_name(name);
  meta.set_enable_raft(node_infos != nullptr);
  return CreateGraphWithId(meta, node_infos);
}

GraphDB *GraphManager::CreateGraphWithId(
    const meta::GraphDBMetaInfo &meta, const meta::RaftNodeInfos *node_infos) {
  std::unique_lock<std::shared_mutex> write_lock(graphs_mutex_);
  auto iter = graphs_.find(meta.graph_name());
  if (iter != graphs_.end()) {
    RG_THROW(common::ErrorCode::GraphAlreadyExists,
             "The graph already exists: {}", meta.graph_name());
  }

  if (node_infos != nullptr) {
    ValidateRaftNodeInfos(*node_infos, meta.graph_name());
    RG_CHECK(node_infos->nodes().contains(local_node_options_.raft_node_id),
             common::ErrorCode::InvalidParameter,
             "configured raft node id {} is not in graph [{}] members",
             local_node_options_.raft_node_id, meta.graph_name());
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
  wb.Put(BuildGraphManagerMetaKey(GraphManagerMetadataType::NextGraphID),
         std::string((const char *)&next, sizeof(next)));
  auto s = meta_db_->Write({}, {}, &wb);
  if (!s.ok()) RG_THROW(common::ErrorCode::StorageEngineError, s.ToString());
  uint64_t current_next = next_graph_id_.load();
  while (current_next < next &&
         !next_graph_id_.compare_exchange_weak(current_next, next)) {
  }
  LOG_INFO("Create graph:{}, path:{}", meta.graph_name(), graph_path);
  graphs_.emplace(meta.graph_name(), std::move(graph_db));
  return graphs_[meta.graph_name()].get();
}

void GraphManager::StartGraphRaft(
    GraphDB *graph_db, const meta::RaftNodeInfos *bootstrap_node_infos) {
  auto local_node = BuildLocalNodeConfig(graph_db->db_meta().graph_name(),
                                         local_node_options_);
  auto store_config = BuildRaftLogStoreConfig(graph_db->path() + "/raft",
                                              raft_log_block_cache_);
  auto raft_config = BuildRaftConfig();
  auto *graph_db_ptr = graph_db;
  auto apply_id = graph_db->GetRaftApplyIndex();
  auto conf_state = graph_db->GetRaftConfState();
  auto persisted_node_infos = graph_db->GetRaftNodeInfos();

  auto apply_request = [graph_db_ptr](uint64_t index,
                                      const meta::RaftRequest &request) {
    ApplyRaftRequest(graph_db_ptr, index, request);
  };
  auto apply_conf_change = [graph_db_ptr](uint64_t index,
                                          const raftpb::ConfState &state,
                                          const meta::RaftNodeInfos &infos) {
    graph_db_ptr->ApplyRaftConfChange(index, state, infos);
  };

  std::vector<eraft::Peer> init_peers;
  if (bootstrap_node_infos != nullptr) {
    init_peers = BuildInitPeers(*bootstrap_node_infos);
  }
  auto raft_driver = std::make_unique<raft_driver::RaftDriver>(
      std::move(apply_request), std::move(apply_conf_change), apply_id,
      std::move(conf_state), std::move(persisted_node_infos),
      std::move(local_node), std::move(init_peers), store_config, raft_config);

  auto err = raft_driver->Run();
  if (err != nullptr) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to run raft driver for graph [{}]: {}",
             graph_db->db_meta().graph_name(), err.String());
  }
  graph_db->SetRaftDriver(std::move(raft_driver));
}

GraphDB *GraphManager::ClearGraph(const std::string &name) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  std::unique_lock<std::shared_mutex> write_lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  if (iter == graphs_.end()) {
    RG_THROW(common::ErrorCode::NoSuchGraph, "No such graph: {}", name);
  }
  LOG_INFO("Clear graph:{}, path:{}", name, iter->second->path());
  if (iter->second->db_meta().enable_raft()) {
    iter->second->ClearDataInternal();
  } else {
    iter->second->ClearData();
  }
  return iter->second.get();
}

void GraphManager::DeleteGraph(const std::string &name) {
  std::lock_guard<std::mutex> guard(create_graph_mutex_);
  std::unique_lock<std::shared_mutex> write_lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  if (iter == graphs_.end()) {
    RG_THROW(common::ErrorCode::NoSuchGraph, "No such graph: {}", name);
  }

  if (iter->second->db_meta().enable_raft()) {
    iter->second->StopRaft();
  }

  rocksdb::WriteOptions wo;
  uint64_t graph_id = iter->second->db_meta().graph_id();
  auto s = meta_db_->Delete(wo, BuildGraphMetaKey(graph_id));
  if (!s.ok()) RG_THROW(common::ErrorCode::StorageEngineError, s.ToString());
  iter->second->drop_on_close() = true;
  std::string graph_path = iter->second->path();
  LOG_INFO("Erase graph:{}, path:{}", name, graph_path);
  graphs_.erase(iter);
}

void GraphManager::CreateManagedGraph(std::string_view name) {
  CreateGraph(std::string(name));
}

void GraphManager::CreateManagedRaftGraph(
    std::string_view name, const meta::RaftNodeInfos &node_infos) {
  CreateGraphWithRaft(std::string(name), node_infos);
}

void GraphManager::ClearManagedGraph(std::string_view name) {
  ClearGraph(std::string(name));
}

void GraphManager::DeleteManagedGraph(std::string_view name) {
  DeleteGraph(std::string(name));
}

std::vector<runtime::ManagedGraphInfo> GraphManager::ListManagedGraphs() const {
  std::shared_lock<std::shared_mutex> read_lock(graphs_mutex_);
  std::vector<runtime::ManagedGraphInfo> graphs;
  graphs.reserve(graphs_.size());
  for (const auto &[name, graph] : graphs_) {
    graphs.push_back({.id = graph->db_meta().graph_id(), .name = name});
  }
  std::ranges::sort(graphs, {}, &runtime::ManagedGraphInfo::id);
  return graphs;
}

meta::RaftNodeInfos GraphManager::ManagedGraphRaftNodeInfos(
    std::string_view name) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  return driver->GetNodeInfosWithLeader();
}

runtime::ManagedRaftChangeResult GraphManager::AddManagedRaftNode(
    std::string_view name, const meta::RaftNodeInfo &node_info, bool learner) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  ValidateRaftNodeInfo(node_info, name);
  auto node_infos = driver->GetNodeInfosWithLeader();
  RG_CHECK(!node_infos.nodes().contains(node_info.node_id()),
           common::ErrorCode::InvalidParameter, "raft node [{}] already exists",
           node_info.node_id());
  for (const auto &[existing_id, existing] : node_infos.nodes()) {
    RG_CHECK(existing.ip() != node_info.ip() ||
                 existing.raft_poft() != node_info.raft_poft(),
             common::ErrorCode::InvalidParameter,
             "raft endpoint [{}:{}] is already used by node [{}]",
             node_info.ip(), node_info.raft_poft(), existing_id);
  }

  auto conf_change = raftpb::ConfChange();
  conf_change.set_type(learner ? raftpb::ConfChangeAddLearnerNode
                               : raftpb::ConfChangeAddNode);
  conf_change.set_node_id(node_info.node_id());
  conf_change.set_context(node_info.SerializeAsString());
  return {.node_id = node_info.node_id(),
          .raft_index = ProposeRaftConfChange(driver, std::move(conf_change)),
          .is_learner = learner};
}

runtime::ManagedRaftChangeResult GraphManager::PromoteManagedRaftLearnerNode(
    std::string_view name, std::uint64_t node_id) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  auto node_infos = driver->GetNodeInfosWithLeader();
  auto iter = node_infos.nodes().find(node_id);
  RG_CHECK(iter != node_infos.nodes().end(),
           common::ErrorCode::InvalidParameter, "raft node [{}] does not exist",
           node_id);
  RG_CHECK(iter->second.is_learner(), common::ErrorCode::InvalidParameter,
           "raft node [{}] is not a learner", node_id);
  const auto status = driver->GetRaftStatus();
  RG_CHECK(status.s.basicStatus_.id_ == status.s.basicStatus_.softState_.lead_,
           common::ErrorCode::StorageEngineError,
           "raft configuration change must be executed on the leader");
  const auto progress = status.s.progress_.find(node_id);
  RG_CHECK(progress != status.s.progress_.end(),
           common::ErrorCode::InvalidParameter,
           "raft learner [{}] has no replication progress", node_id);
  RG_CHECK(progress->second.match_ >= status.s.basicStatus_.hardState_.commit(),
           common::ErrorCode::InvalidParameter,
           "raft learner [{}] has not caught up: match index {}, commit index "
           "{}",
           node_id, progress->second.match_,
           status.s.basicStatus_.hardState_.commit());

  auto node_info = iter->second;
  node_info.set_is_learner(false);
  node_info.set_is_leader(false);
  raftpb::ConfChange conf_change;
  conf_change.set_type(raftpb::ConfChangeAddNode);
  conf_change.set_node_id(node_id);
  conf_change.set_context(node_info.SerializeAsString());
  return {.node_id = node_id,
          .raft_index = ProposeRaftConfChange(driver, std::move(conf_change)),
          .is_learner = false};
}

runtime::ManagedRaftChangeResult GraphManager::RemoveManagedRaftNode(
    std::string_view name, std::uint64_t node_id) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  auto node_infos = driver->GetNodeInfosWithLeader();
  auto iter = node_infos.nodes().find(node_id);
  RG_CHECK(iter != node_infos.nodes().end(),
           common::ErrorCode::InvalidParameter, "raft node [{}] does not exist",
           node_id);
  RG_CHECK(!iter->second.is_leader(), common::ErrorCode::InvalidParameter,
           "cannot remove the current leader; transfer leadership first");
  if (!iter->second.is_learner()) {
    const auto voter_count = std::ranges::count_if(
        node_infos.nodes(),
        [](const auto &item) { return !item.second.is_learner(); });
    RG_CHECK(voter_count > 1, common::ErrorCode::InvalidParameter,
             "cannot remove the last raft voter");
  }

  const bool was_learner = iter->second.is_learner();
  raftpb::ConfChange conf_change;
  conf_change.set_type(raftpb::ConfChangeRemoveNode);
  conf_change.set_node_id(node_id);
  conf_change.set_context(iter->second.SerializeAsString());
  return {.node_id = node_id,
          .raft_index = ProposeRaftConfChange(driver, std::move(conf_change)),
          .is_learner = was_learner};
}

void GraphManager::TransferManagedRaftLeader(std::string_view name,
                                             std::uint64_t node_id) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  auto err = driver->TransferLeader(node_id);
  if (err != nullptr) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "raft leader transfer failed: {}", err.String());
  }
}

runtime::ManagedRaftChangeResult GraphManager::DemoteManagedRaftNode(
    std::string_view name, std::uint64_t node_id) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  auto node_infos = driver->GetNodeInfosWithLeader();
  auto iter = node_infos.nodes().find(node_id);
  RG_CHECK(iter != node_infos.nodes().end(),
           common::ErrorCode::InvalidParameter, "raft node [{}] does not exist",
           node_id);
  RG_CHECK(!iter->second.is_learner(), common::ErrorCode::InvalidParameter,
           "raft node [{}] is already a learner", node_id);
  RG_CHECK(!iter->second.is_leader(), common::ErrorCode::InvalidParameter,
           "cannot demote the current leader; transfer leadership first");
  const auto voter_count = std::ranges::count_if(
      node_infos.nodes(),
      [](const auto &item) { return !item.second.is_learner(); });
  RG_CHECK(voter_count > 1, common::ErrorCode::InvalidParameter,
           "cannot demote the last raft voter");

  auto node_info = iter->second;
  node_info.set_is_learner(true);
  node_info.set_is_leader(false);
  raftpb::ConfChange conf_change;
  conf_change.set_type(raftpb::ConfChangeAddLearnerNode);
  conf_change.set_node_id(node_id);
  conf_change.set_context(node_info.SerializeAsString());
  return {.node_id = node_id,
          .raft_index = ProposeRaftConfChange(driver, std::move(conf_change)),
          .is_learner = true};
}

runtime::ManagedRaftChangeResult GraphManager::UpdateManagedRaftNode(
    std::string_view name, const meta::RaftNodeInfo &node_info) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  ValidateRaftNodeInfo(node_info, name);
  auto node_infos = driver->GetNodeInfosWithLeader();
  auto iter = node_infos.nodes().find(node_info.node_id());
  RG_CHECK(iter != node_infos.nodes().end(),
           common::ErrorCode::InvalidParameter, "raft node [{}] does not exist",
           node_info.node_id());
  RG_CHECK(!iter->second.is_leader(), common::ErrorCode::InvalidParameter,
           "cannot update the current leader node");
  for (const auto &[existing_id, existing] : node_infos.nodes()) {
    if (existing_id == node_info.node_id()) {
      continue;
    }
    RG_CHECK(existing.ip() != node_info.ip() ||
                 existing.raft_poft() != node_info.raft_poft(),
             common::ErrorCode::InvalidParameter,
             "raft endpoint [{}:{}] is already used by node [{}]",
             node_info.ip(), node_info.raft_poft(), existing_id);
  }

  auto updated = node_info;
  updated.set_is_learner(iter->second.is_learner());
  updated.set_is_leader(false);
  raftpb::ConfChange conf_change;
  conf_change.set_type(raftpb::ConfChangeUpdateNode);
  conf_change.set_node_id(updated.node_id());
  conf_change.set_context(updated.SerializeAsString());
  return {.node_id = updated.node_id(),
          .raft_index = ProposeRaftConfChange(driver, std::move(conf_change)),
          .is_learner = updated.is_learner()};
}

runtime::ManagedRaftStatus GraphManager::ManagedGraphRaftStatus(
    std::string_view name) {
  auto graph = OpenGraph(std::string(name));
  auto *driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
           "graph [{}] does not enable raft", name);
  const auto status = driver->GetRaftStatus();
  runtime::ManagedRaftStatus result;
  result.local_node_id = status.s.basicStatus_.id_;
  result.leader_id = status.s.basicStatus_.softState_.lead_;
  result.term = status.s.basicStatus_.hardState_.term();
  result.commit_index = status.s.basicStatus_.hardState_.commit();
  result.applied_index = status.s.basicStatus_.applied_;
  result.first_log = status.first_log;
  result.last_log = status.last_log;
  result.raft_state =
      eraft::ToString(status.s.basicStatus_.softState_.raftState_);
  result.nodes.reserve(status.nodes.size());
  for (const auto &node : status.nodes) {
    result.nodes.push_back({.node_id = node.node_info.node_id(),
                            .ip = node.node_info.ip(),
                            .bolt_port = node.node_info.bolt_port(),
                            .raft_port = node.node_info.raft_poft(),
                            .is_leader = node.node_info.is_leader(),
                            .is_learner = node.node_info.is_learner(),
                            .reachable = node.reachable,
                            .match_index = node.match_index,
                            .next_index = node.next_index});
  }
  std::ranges::sort(
      result.nodes, {},
      [](const runtime::ManagedRaftNodeStatus &node) { return node.node_id; });
  return result;
}

}  // namespace server
