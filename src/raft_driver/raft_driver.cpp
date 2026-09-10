/**
 * Copyright 2022 AntGroup CO., Ltd.
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

// written by botu.wzy

#include "raft_driver/raft_driver.h"

#include <gflags/gflags.h>
#include <pthread.h>

#include <boost/asio.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/lexical_cast.hpp>
#include <filesystem>
#include <future>
#include <shared_mutex>

#include "common/exceptions.h"
#include "common/logger.h"

using boost::asio::async_write;
using boost::asio::ip::tcp;
using namespace std::chrono;

namespace raft {
namespace {
size_t NormalizeRaftShardCount(size_t shard_count) {
  return shard_count == 0 ? 1 : shard_count;
}

std::atomic<size_t>& ConfiguredRaftShardCount() {
  static std::atomic<size_t> shard_count{4};
  return shard_count;
}

std::string LocalNodeIdentityString(const LocalNodeConfig& local_node) {
  return fmt::format("graph={}, ip={}, bolt_port={}, raft_poft={}",
                     local_node.graph, local_node.ip, local_node.bolt_port,
                     local_node.raft_poft);
}

bool MatchLocalNode(const LocalNodeConfig& local_node,
                    const meta::RaftNodeInfo& node_info) {
  return node_info.graph() == local_node.graph &&
         node_info.ip() == local_node.ip &&
         node_info.bolt_port() == local_node.bolt_port &&
         node_info.raft_poft() == local_node.raft_poft;
}

eraft::Error InferNodeIdFromNodeInfos(const LocalNodeConfig& local_node,
                                      const meta::RaftNodeInfos& node_infos,
                                      uint64_t* node_id) {
  std::optional<uint64_t> matched;
  for (const auto& [id, node_info] : node_infos.nodes()) {
    if (!MatchLocalNode(local_node, node_info)) {
      continue;
    }
    if (matched.has_value()) {
      return eraft::Error(
          fmt::format("multiple raft nodes match local identity [{}]",
                      LocalNodeIdentityString(local_node)));
    }
    matched = id;
  }
  if (!matched.has_value()) {
    return eraft::Error(
        fmt::format("no raft node matches local identity [{}] in node infos",
                    LocalNodeIdentityString(local_node)));
  }
  *node_id = matched.value();
  return nullptr;
}

eraft::Error InferNodeIdFromInitPeers(const LocalNodeConfig& local_node,
                                      const std::vector<eraft::Peer>& peers,
                                      uint64_t* node_id) {
  std::optional<uint64_t> matched;
  for (const auto& peer : peers) {
    meta::RaftNodeInfo node_info;
    if (!node_info.ParseFromString(peer.context_)) {
      return eraft::Error(fmt::format(
          "failed to parse initial peer context for peer {}", peer.id_));
    }
    if (!MatchLocalNode(local_node, node_info)) {
      continue;
    }
    if (matched.has_value()) {
      return eraft::Error(
          fmt::format("multiple initial peers match local identity [{}]",
                      LocalNodeIdentityString(local_node)));
    }
    matched = peer.id_;
  }
  if (!matched.has_value()) {
    return eraft::Error(
        fmt::format("no initial peer matches local identity [{}]",
                    LocalNodeIdentityString(local_node)));
  }
  *node_id = matched.value();
  return nullptr;
}
}  // namespace

void NodeClient::reconnect() {
  if (has_closed_) {
    return;
  }
  boost::system::error_code ec;
  socket_.close(ec);
  if (ec) {
    LOG_WARN("socket close error: {}", ec.message());
  }
  connected_ = false;
  send_buffers_.clear();
  msg_queue_.clear();
  timer_.expires_from_now(interval_);
  timer_.async_wait(
      [this, self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
          return;
        }
        Connect();
      });
}

void NodeClient::Close() {
  if (has_closed_.exchange(true)) {
    return;
  }
  connected_ = false;
  io_service_.post([this, self = shared_from_this()]() {
    boost::system::error_code ec;
    socket_.close(ec);
    timer_.cancel(ec);
    connected_ = false;
    msg_queue_.clear();
    send_buffers_.clear();
  });
}

void NodeClient::Send(std::string str) {
  if (has_closed_) {
    LOG_DEBUG("connection[{}:{}] is closed, drop this message",
              endpoint_.address().to_string(), endpoint_.port());
    return;
  }
  io_service_.post(
      [this, self = shared_from_this(), msg = std::move(str)]() mutable {
        if (!connected_) {
          LOG_DEBUG("connection[{}:{}] is not available, drop this message",
                    endpoint_.address().to_string(), endpoint_.port());
          return;
        }
        bool need_invoke = msg_queue_.empty();
        msg_queue_.emplace_back(std::move(msg));
        if (need_invoke) {
          do_send();
        }
      });
}

void NodeClient::send_magic_code() {
  async_write(socket_, boost::asio::buffer(magic_code),
              [this, self = shared_from_this()](
                  const boost::system::error_code& ec, size_t) {
                if (ec) {
                  LOG_WARN("async write error {}", ec.message());
                  if (ec == boost::asio::error::operation_aborted) {
                    return;
                  }
                  reconnect();
                  return;
                }
                connected_ = true;
                do_read_some();
              });
}

void NodeClient::do_send() {
  for (size_t i = 0; i < msg_queue_.size(); i++) {
    send_buffers_.emplace_back(boost::asio::buffer(msg_queue_[i]));
    if (send_buffers_.size() >= 5) {
      break;
    }
  }
  async_write(socket_, send_buffers_,
              [this, self = shared_from_this()](
                  const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                  LOG_WARN("async write error {}", ec.message());
                  if (ec == boost::asio::error::operation_aborted) {
                    return;
                  }
                  reconnect();
                  return;
                }
                assert(msg_queue_.size() >= send_buffers_.size());
                msg_queue_.erase(msg_queue_.begin(),
                                 msg_queue_.begin() + send_buffers_.size());
                send_buffers_.clear();
                if (!msg_queue_.empty()) {
                  do_send();
                }
              });
}

void NodeClient::do_read_some() {
  socket_.async_read_some(
      boost::asio::buffer(buffer4_),
      [this, self = shared_from_this()](const boost::system::error_code& ec,
                                        size_t) {
        if (ec) {
          LOG_WARN("async_read_some error: {}", ec.message());
          if (ec == boost::asio::error::operation_aborted) {
            return;
          }
          reconnect();
        } else {
          LOG_ERROR("unexpected read in node client do_read_some");
          reconnect();
        }
      });
}

void socket_set_options(tcp::socket& socket) {
  try {
    socket.set_option(tcp::no_delay(true));
    socket.set_option(boost::asio::socket_base::keep_alive(true));
    socket.set_option(tcp::socket::reuse_address(true));
  } catch (const boost::system::system_error& e) {
    LOG_ERROR("socket_set_options error: {}", e.what());
  }
}

void NodeClient::Connect() {
  if (has_closed_) {
    return;
  }
  boost::system::error_code ec;
  socket_.open(tcp::v4(), ec);
  if (ec) {
    LOG_WARN("socket open error: {}", ec.message());
  }
  socket_.async_connect(endpoint_, [this, self = shared_from_this()](
                                       const boost::system::error_code& ec) {
    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        return;
      }
      LOG_WARN("async connect {} error: {}",
               boost::lexical_cast<std::string>(endpoint_), ec.message());
      reconnect();
    } else {
      LOG_INFO("connect to {} successfully",
               boost::lexical_cast<std::string>(endpoint_));
      socket_set_options(socket_);
      send_magic_code();
    }
  });
}

TransportClient::~TransportClient() {
  if (transport_) {
    transport_->Release(key_);
  }
}

void TransportClient::Send(std::string str) {
  if (client_) {
    client_->Send(std::move(str));
  }
}

bool TransportClient::connected() const {
  return client_ != nullptr && client_->connected();
}

std::string RaftTransport::BuildKey(const std::string& ip, int port) {
  return ip + ":" + std::to_string(port);
}

std::shared_ptr<TransportClient> RaftTransport::Acquire(const std::string& ip,
                                                        int port) {
  auto key = BuildKey(ip, port);
  std::shared_ptr<NodeClient> client;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto& entry = clients_[key];
    if (!entry.client) {
      entry.client = std::make_shared<NodeClient>(client_service_, ip, port);
      entry.client->Connect();
    }
    ++entry.refs;
    client = entry.client;
  }
  return std::shared_ptr<TransportClient>(new TransportClient(
      shared_from_this(), std::move(key), std::move(client)));
}

void RaftTransport::Release(const std::string& key) {
  std::shared_ptr<NodeClient> client_to_close;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto iter = clients_.find(key);
    if (iter == clients_.end()) {
      return;
    }
    if (iter->second.refs > 0) {
      --iter->second.refs;
    }
    if (iter->second.refs == 0) {
      client_to_close = std::move(iter->second.client);
      clients_.erase(iter);
    }
  }
  if (client_to_close) {
    client_to_close->Close();
  }
}

void RaftTransport::CloseAll() {
  std::vector<std::shared_ptr<NodeClient>> clients;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    clients.reserve(clients_.size());
    for (auto& [_, entry] : clients_) {
      if (entry.client) {
        clients.emplace_back(std::move(entry.client));
      }
    }
    clients_.clear();
  }
  for (auto& client : clients) {
    client->Close();
  }
}

RaftManager::ServiceRunner::ServiceRunner(std::string thread_name,
                                          size_t thread_num)
    : thread_name(std::move(thread_name)),
      work(std::make_unique<boost::asio::io_service::work>(service)) {
  if (thread_num == 0) {
    thread_num = 1;
  }
  threads.reserve(thread_num);
  for (size_t i = 0; i < thread_num; ++i) {
    threads.emplace_back([this, i]() {
      {
        std::lock_guard<std::mutex> guard(thread_ids_mutex);
        thread_ids.insert(std::this_thread::get_id());
      }
      auto name = fmt::format("{}{}", this->thread_name, i);
      pthread_setname_np(pthread_self(), name.c_str());
      service.run();
      {
        std::lock_guard<std::mutex> guard(thread_ids_mutex);
        thread_ids.erase(std::this_thread::get_id());
      }
    });
  }
}

RaftManager::ServiceRunner::~ServiceRunner() { Stop(); }

void RaftManager::ServiceRunner::Stop() {
  if (stopped.exchange(true)) {
    return;
  }
  work.reset();
  service.stop();
  for (auto& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads.clear();
}

void RaftManager::ServiceRunner::WaitForIdle() {
  if (stopped.load() || IsServiceThread()) {
    return;
  }
  std::promise<void> promise;
  auto future = promise.get_future();
  service.post([&promise]() { promise.set_value(); });
  future.wait();
}

bool RaftManager::ServiceRunner::IsServiceThread() {
  std::lock_guard<std::mutex> guard(thread_ids_mutex);
  return thread_ids.count(std::this_thread::get_id()) > 0;
}

RaftManager::ServiceShard::ServiceShard(size_t shard_id)
    : raft_runner(fmt::format("raft-mgr{}-", shard_id), 1),
      timer_runner(fmt::format("raft-tmr{}-", shard_id), 1),
      apply_runner(fmt::format("raft-app{}-", shard_id), 1) {}

void RaftManager::Configure(size_t raft_shard_count) {
  ConfiguredRaftShardCount().store(NormalizeRaftShardCount(raft_shard_count));
}

std::shared_ptr<RaftManager> RaftManager::Instance() {
  static std::weak_ptr<RaftManager> weak_manager;
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  auto manager = weak_manager.lock();
  if (!manager) {
    manager = std::shared_ptr<RaftManager>(
        new RaftManager(ConfiguredRaftShardCount().load()));
    weak_manager = manager;
  }
  return manager;
}

RaftManager::RaftManager(size_t raft_shard_count)
    : raft_shard_count_(NormalizeRaftShardCount(raft_shard_count)),
      client_runner_("raft-cli-", 1) {
  shards_.reserve(raft_shard_count_);
  for (size_t i = 0; i < raft_shard_count_; ++i) {
    shards_.emplace_back(std::make_unique<ServiceShard>(i));
  }
  transport_ = std::make_shared<RaftTransport>(client_runner_.service);
  LOG_INFO("raft manager started with {} scheduler shard(s)",
           raft_shard_count_);
}

RaftManager::~RaftManager() {
  if (transport_) {
    transport_->CloseAll();
  }
}

size_t RaftManager::PickShard(const std::string& graph) const {
  return std::hash<std::string>{}(graph) % raft_shard_count_;
}

boost::asio::io_service& RaftManager::raft_service(size_t shard_id) {
  return Shard(shard_id).raft_runner.service;
}

boost::asio::io_service& RaftManager::timer_service(size_t shard_id) {
  return Shard(shard_id).timer_runner.service;
}

boost::asio::io_service& RaftManager::apply_service(size_t shard_id) {
  return Shard(shard_id).apply_runner.service;
}

RaftManager::ServiceShard& RaftManager::Shard(size_t shard_id) {
  return *shards_[shard_id % raft_shard_count_];
}

boost::asio::io_service& RaftManager::client_service() {
  return client_runner_.service;
}

const RaftManager::ServiceShard& RaftManager::Shard(size_t shard_id) const {
  return *shards_[shard_id % raft_shard_count_];
}

std::shared_ptr<TransportClient> RaftManager::AcquireClient(
    const std::string& ip, int port) {
  return transport_->Acquire(ip, port);
}

void RaftManager::WaitForRaftService(size_t shard_id) {
  Shard(shard_id).raft_runner.WaitForIdle();
}

void RaftManager::WaitForTimerService(size_t shard_id) {
  Shard(shard_id).timer_runner.WaitForIdle();
}

void RaftManager::WaitForApplyService(size_t shard_id) {
  Shard(shard_id).apply_runner.WaitForIdle();
}

std::string MessageToNetString(const std::string& graph,
                               const raftpb::Message& msg) {
  meta::RaftMessage envelope;
  envelope.set_graph(graph);
  envelope.mutable_message()->CopyFrom(msg);

  uint32_t msg_size = envelope.ByteSizeLong();
  std::string str;
  str.reserve(msg_size + sizeof(uint32_t));
  boost::endian::native_to_big_inplace(msg_size);
  str.append(reinterpret_cast<const char*>(&msg_size), sizeof(msg_size));
  str.append(envelope.SerializeAsString());
  return str;
}

bool RaftConfig::Check() {
  if (tick_interval < 100) {
    LOG_WARN("tick_interval should be greater than 100");
    return false;
  }
  if (heartbeat_tick < 1) {
    LOG_WARN("heartbeat_tick should be greater than 1");
    return false;
  }
  if (election_tick < 10) {
    LOG_WARN("election_tick should be greater than 10");
    return false;
  }
  if (proposal_timeout < 1) {
    LOG_WARN("proposal_timeout should be greater than 0");
    return false;
  }
  if (max_proposal_bytes == 0) {
    LOG_WARN("max_proposal_bytes should be greater than 0");
    return false;
  }
  if (max_pending_proposals == 0) {
    LOG_WARN("max_pending_proposals should be greater than 0");
    return false;
  }
  if (max_pending_proposal_bytes < max_proposal_bytes) {
    LOG_WARN(
        "max_pending_proposal_bytes should be greater than or equal to "
        "max_proposal_bytes");
    return false;
  }
  return true;
}

bool RaftLogStoreConfig::Check() {
  if (path.empty()) {
    LOG_WARN("raft logstore path is empty.");
    return false;
  }
  if (shared_block_cache == nullptr) {
    LOG_WARN("shared_block_cache should not be null");
    return false;
  }
  if (total_threads < 2) {
    LOG_WARN("total_threads should be greater than 2");
    return false;
  }
  if (keep_logs < 100000) {
    LOG_WARN("keep_logs should be greater than 100000");
    return false;
  }
  if (gc_interval < 1) {
    LOG_WARN("gc_interval should be greater than 1");
    return false;
  }
  return true;
}

bool LocalNodeConfig::Check() {
  if (graph.empty()) {
    LOG_WARN("local node graph is empty.");
    return false;
  }
  if (ip.empty()) {
    LOG_WARN("local node ip is empty.");
    return false;
  }
  if (bolt_port < 1) {
    LOG_WARN("local node bolt_port should be greater than 0");
    return false;
  }
  if (raft_poft < 1) {
    LOG_WARN("local node raft_poft should be greater than 0");
    return false;
  }
  return true;
}

RaftDriver::RaftDriver(
    std::function<void(uint64_t index, const meta::RaftRequest&)> apply,
    uint64_t apply_id, LocalNodeConfig local_node,
    const RaftLogStoreConfig& store_config, const RaftConfig& config)
    : manager_(RaftManager::Instance()),
      callback_alive_(std::make_shared<std::atomic<bool>>(false)),
      apply_(std::move(apply)),
      apply_id_(apply_id),
      local_node_(std::move(local_node)),
      shard_id_(manager_->PickShard(local_node_.graph)),
      node_id_(0),
      tick_interval_(config.tick_interval),
      tick_timer_(manager_->timer_service(shard_id_), tick_interval_),
      compact_interval_(store_config.gc_interval * 60 * 1000),
      compact_timer_(manager_->timer_service(shard_id_), compact_interval_),
      store_config_(store_config),
      raft_config_(config) {}

RaftDriver::RaftDriver(
    std::function<void(uint64_t index, const meta::RaftRequest&)> apply,
    uint64_t apply_id, LocalNodeConfig local_node,
    std::vector<eraft::Peer> init_peers, const RaftLogStoreConfig& store_config,
    const RaftConfig& config)
    : manager_(RaftManager::Instance()),
      callback_alive_(std::make_shared<std::atomic<bool>>(false)),
      apply_(std::move(apply)),
      apply_id_(apply_id),
      local_node_(std::move(local_node)),
      shard_id_(manager_->PickShard(local_node_.graph)),
      node_id_(0),
      init_peers_(std::move(init_peers)),
      tick_interval_(config.tick_interval),
      tick_timer_(manager_->timer_service(shard_id_), tick_interval_),
      compact_interval_(store_config.gc_interval * 60 * 1000),
      compact_timer_(manager_->timer_service(shard_id_), compact_interval_),
      store_config_(store_config),
      raft_config_(config) {}

eraft::Error RaftDriver::Run() {
  stopped_.store(false);
  callback_alive_->store(true);
  if (!local_node_.Check()) {
    return eraft::Error("invalid local node config");
  }
  if (store_config_.shared_block_cache == nullptr) {
    return eraft::Error("raft log store shared_block_cache is required");
  }
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;
  rocksdb::BlockBasedTableOptions table_options;
  table_options.cache_index_and_filter_blocks = true;
  table_options.block_cache = store_config_.shared_block_cache;
  table_options.data_block_index_type =
      rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
  table_options.partition_filters = true;
  table_options.index_type =
      rocksdb::BlockBasedTableOptions::IndexType::kTwoLevelIndexSearch;
  options.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));
  options.IncreaseParallelism(store_config_.total_threads);
  options.OptimizeLevelStyleCompaction();
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, options);
  cfs.emplace_back("meta", options);
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
  std::unique_ptr<rocksdb::DB> db;
  std::filesystem::create_directories(store_config_.path);
  auto s =
      rocksdb::DB::Open(options, store_config_.path, cfs, &cf_handles, &db);
  if (!s.ok()) {
    return eraft::Error("failed to open raft db, error: " + s.ToString());
  }
  storage_ = std::make_shared<RaftLogStorage>(db.release(), cf_handles[0],
                                               cf_handles[1]);
  auto applied = std::max(apply_id_.load(), storage_->GetApplyIndex());
  auto nodes = storage_->GetNodeInfos();
  if (nodes.has_value()) {
    node_infos_.ParseFromString(nodes.value());
  }
  for (auto& [id, node] : node_infos_.nodes()) {
    auto client = manager_->AcquireClient(node.ip(), node.raft_poft());
    node_clients_.emplace(node.node_id(), std::move(client));
  }
  LOG_INFO("raft nodes info: {}", node_infos_.ShortDebugString());
  bool exist = storage_->Init();
  eraft::Error err;
  if (!node_infos_.nodes().empty()) {
    err = InferNodeIdFromNodeInfos(local_node_, node_infos_, &node_id_);
    if (err != nullptr) {
      return err;
    }
  } else {
    if (init_peers_.empty()) {
      if (exist) {
        return eraft::Error(
            fmt::format("failed to infer local node id from empty node infos "
                        "for [{}]",
                        LocalNodeIdentityString(local_node_)));
      }
      return eraft::Error(
          "initial peers are required for first raft bootstrap");
    }
    err = InferNodeIdFromInitPeers(local_node_, init_peers_, &node_id_);
    if (err != nullptr) {
      return err;
    }
  }
  id_generator_.Reset(node_id_, duration_cast<milliseconds>(
                                    steady_clock::now().time_since_epoch())
                                    .count());
  eraft::Config config;
  config.id_ = node_id_;
  config.applied_ = applied;
  config.electionTick_ = raft_config_.election_tick;
  config.heartbeatTick_ = raft_config_.heartbeat_tick;
  config.storage_ = storage_;
  config.maxSizePerMsg_ = 1024 * 1024;
  config.maxInflightMsgs_ = 256;
  config.maxUncommittedEntriesSize_ = raft_config_.max_pending_proposal_bytes;
  config.stepDownOnRemoval_ = true;
  config.preVote_ = true;
  config.checkQuorum_ = true;
  std::tie(rn_, err) = eraft::NewRawNode(config);
  if (err != nullptr) {
    return err;
  }
  if (!exist) {
    err = rn_->Bootstrap(init_peers_);
    if (err != nullptr) {
      return err;
    }
    // Persist and apply bootstrap ConfChange entries before Run returns, so a
    // crash after graph metadata is visible can still recover local node infos.
    CheckReady();
  }
  Tick();
  CheckAndCompactLog();
  return {};
}

void RaftDriver::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  callback_alive_->store(false);
  RejectPendingPromises(eraft::Error("raft driver stopped"));
  manager_->timer_service(shard_id_).post([this]() {
    boost::system::error_code ec;
    tick_timer_.cancel(ec);
    compact_timer_.cancel(ec);
  });
  manager_->WaitForTimerService(shard_id_);
  manager_->WaitForRaftService(shard_id_);
  manager_->WaitForApplyService(shard_id_);
  node_clients_.clear();
  if (storage_) {
    storage_->Close();
  }
  LOG_INFO("bolt raft driver stopped");
}

void RaftDriver::Step(raftpb::Message msg) {
  if (stopped_.load()) {
    return;
  }
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post(
      [this, alive, msg = std::move(msg)]() mutable {
        if (!alive->load() || stopped_.load()) {
          return;
        }
        auto err = rn_->Step(std::move(msg));
        if (err != nullptr) {
          LOG_WARN("failed to step message, err: {}", err.String());
          return;
        }
        CheckReady();
      });
}

std::shared_ptr<PromiseContext> RaftDriver::Propose(uint64_t uuid,
                                                    raftpb::Message msg,
                                                    uint64_t proposal_bytes) {
  auto context = std::make_shared<PromiseContext>();
  context->id = uuid;
  context->proposal_bytes = proposal_bytes;
  {
    std::lock_guard<std::mutex> guard(promise_mutex_);
    if (stopped_.load()) {
      context->SetError(eraft::Error("raft driver stopped"));
      return context;
    }
    if (proposal_bytes > raft_config_.max_proposal_bytes) {
      context->SetError(eraft::Error(fmt::format(
          "raft proposal {} is too large: {} bytes exceeds limit {} bytes",
          uuid, proposal_bytes, raft_config_.max_proposal_bytes)));
      return context;
    }
    if (pending_proposals_ >= raft_config_.max_pending_proposals) {
      context->SetError(eraft::Error(fmt::format(
          "raft proposal backpressure: pending proposals {} reaches limit {}",
          pending_proposals_, raft_config_.max_pending_proposals)));
      return context;
    }
    if (pending_proposal_bytes_ > raft_config_.max_pending_proposal_bytes ||
        proposal_bytes >
            raft_config_.max_pending_proposal_bytes - pending_proposal_bytes_) {
      context->SetError(eraft::Error(fmt::format(
          "raft proposal backpressure: pending proposal bytes {} + {} exceeds "
          "limit {}",
          pending_proposal_bytes_, proposal_bytes,
          raft_config_.max_pending_proposal_bytes)));
      return context;
    }
    pending_promise_.emplace(uuid, context);
    ++pending_proposals_;
    pending_proposal_bytes_ += proposal_bytes;
    context->proposal_accounted = true;
  }
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post(
      [this, alive, uuid, context, msg = std::move(msg)]() mutable {
        eraft::Error err = nullptr;
        bool should_reject = false;
        {
          std::lock_guard<std::mutex> guard(promise_mutex_);
          auto iter = pending_promise_.find(uuid);
          if (iter == pending_promise_.end() || iter->second != context) {
            return;
          }
          if (!alive->load() || stopped_.load()) {
            err = eraft::Error("raft driver stopped");
            ReleaseProposalAccountingLocked(iter->second);
            pending_promise_.erase(iter);
            should_reject = true;
          } else if (rn_->raft_->id_ != rn_->raft_->lead_) {
            err = eraft::Error("not leader");
            ReleaseProposalAccountingLocked(iter->second);
            pending_promise_.erase(iter);
            should_reject = true;
          } else {
            msg.set_from(rn_->raft_->id_);
            err = rn_->raft_->Step(std::move(msg));
            if (err != nullptr) {
              ReleaseProposalAccountingLocked(iter->second);
              pending_promise_.erase(iter);
              should_reject = true;
            }
          }
        }
        if (should_reject) {
          if (err.String() != "not leader" &&
              err.String() != "raft driver stopped") {
            LOG_WARN("failed to step raft message, err: {}", err.String());
          }
          context->SetError(std::move(err));
          return;
        }
        CheckReady();
      });
  return context;
}

bool RaftDriver::RemovePendingPromise(
    uint64_t uuid, const std::shared_ptr<PromiseContext>& context) {
  std::lock_guard<std::mutex> guard(promise_mutex_);
  auto iter = pending_promise_.find(uuid);
  if (iter == pending_promise_.end() || iter->second != context) {
    return false;
  }
  ReleaseProposalAccountingLocked(iter->second);
  pending_promise_.erase(iter);
  return true;
}

void RaftDriver::ReleaseProposalAccounting(
    const std::shared_ptr<PromiseContext>& context) {
  std::lock_guard<std::mutex> guard(promise_mutex_);
  ReleaseProposalAccountingLocked(context);
}

void RaftDriver::ReleaseProposalAccountingLocked(
    const std::shared_ptr<PromiseContext>& context) {
  if (!context->proposal_accounted) {
    return;
  }
  context->proposal_accounted = false;
  if (pending_proposals_ > 0) {
    --pending_proposals_;
  }
  if (pending_proposal_bytes_ >= context->proposal_bytes) {
    pending_proposal_bytes_ -= context->proposal_bytes;
  } else {
    pending_proposal_bytes_ = 0;
  }
}

void RaftDriver::RejectPendingPromises(const eraft::Error& err) {
  std::vector<std::shared_ptr<PromiseContext>> contexts;
  {
    std::lock_guard<std::mutex> guard(promise_mutex_);
    contexts.reserve(pending_promise_.size());
    for (auto& [_, context] : pending_promise_) {
      ReleaseProposalAccountingLocked(context);
      contexts.emplace_back(std::move(context));
    }
    pending_promise_.clear();
  }
  for (auto& context : contexts) {
    context->SetError(err);
  }
}

void RaftDriver::Tick() {
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive]() mutable {
    if (!alive->load() || stopped_.load()) {
      return;
    }
    rn_->Tick();
    CheckReady();
  });
  tick_timer_.expires_at(tick_timer_.expires_at() + tick_interval_);
  tick_timer_.async_wait([this, alive](const boost::system::error_code& ec) {
    if (!alive->load() || stopped_.load()) {
      return;
    }
    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        LOG_WARN("tick_timer async_wait error: {}", ec.message());
      }
      return;
    }
    Tick();
  });
}

std::shared_ptr<PromiseContext> RaftDriver::ProposeConfChange(
    raftpb::ConfChange& cc) {
  cc.set_id(id_generator_.Next());
  raftpb::Message msg;
  auto entry = msg.add_entries();
  entry->set_type(raftpb::EntryType::EntryConfChange);
  entry->set_data(cc.SerializeAsString());
  msg.set_type(raftpb::MessageType::MsgProp);
  auto proposal_bytes = msg.ByteSizeLong();
  return Propose(cc.id(), std::move(msg), proposal_bytes);
}

PromiseContext::ApplyResult RaftDriver::ProposeWriteBatch(
    meta::WriteBatchKind kind, const rocksdb::WriteBatch& wb) {
  meta::RaftRequest request;
  request.set_wb_kind(kind);
  request.set_wb_data(wb.Data());
  return ProposeRaftRequestAndWait(std::move(request));
}

PromiseContext::ApplyResult RaftDriver::ProposeRaftRequestAndWait(
    meta::RaftRequest request) {
  auto context = ProposeRaftRequest(std::move(request));
  auto future = context->applied.get_future();
  auto timeout = std::chrono::milliseconds(raft_config_.proposal_timeout);
  if (future.wait_for(timeout) == std::future_status::ready) {
    return future.get();
  }
  if (future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::ready) {
    return future.get();
  }
  auto err =
      eraft::Error(fmt::format("proposal {} timed out after {} ms", context->id,
                               raft_config_.proposal_timeout));
  if (RemovePendingPromise(context->id, context)) {
    context->SetError(err);
  }
  return PromiseContext::ApplyResult{std::move(err), 0};
}

std::shared_ptr<PromiseContext> RaftDriver::ProposeRaftRequest(
    meta::RaftRequest request) {
  if (request.wb_kind() == meta::WriteBatchKind::UNKNOWN) {
    THROW_CODE(InvalidParameter, "write batch kind must be specified");
  }
  request.set_id(id_generator_.Next());
  raftpb::Message msg;
  auto entry = msg.add_entries();
  entry->set_type(raftpb::EntryType::EntryNormal);
  entry->set_data(request.SerializeAsString());
  msg.set_type(raftpb::MessageType::MsgProp);
  auto proposal_bytes = msg.ByteSizeLong();
  return Propose(request.id(), std::move(msg), proposal_bytes);
}

meta::RaftNodeInfos RaftDriver::GetNodeInfosWithLeader() {
  std::promise<uint64_t> promise;
  auto future = promise.get_future();
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive, &promise]() {
    if (!alive->load() || stopped_.load()) {
      promise.set_value(0);
      return;
    }
    promise.set_value(rn_->raft_->lead_);
  });
  auto leader = future.get();

  std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
  meta::RaftNodeInfos ret = node_infos_;
  if (ret.nodes().count(leader)) {
    ret.mutable_nodes()->at(leader).set_is_leader(true);
  }
  return ret;
}

RaftStatus RaftDriver::GetRaftStatus() {
  std::promise<RaftStatus> promise;
  auto future = promise.get_future();
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive, &promise]() {
    RaftStatus rs;
    if (!alive->load() || stopped_.load()) {
      promise.set_value(rs);
      return;
    }
    rs.s = rn_->GetStatus();
    rs.first_log = storage_->FirstIndex().first - 1;
    rs.last_log = storage_->LastIndex().first;
    promise.set_value(rs);
  });
  return future.get();
}

void RaftDriver::CheckAndCompactLog() {
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive]() mutable {
    if (!alive->load() || stopped_.load()) {
      return;
    }
    auto first = storage_->FirstIndex().first;
    auto applied = apply_id_.load();
    if (applied > first) {
      if (applied - first >= store_config_.keep_logs + 100000) {
        auto compacted = applied - store_config_.keep_logs;
        storage_->Compact(compacted);
        LOG_INFO("compact raft log, compacted index:{}, applied index:{}",
                 compacted, applied);
      }
    }
  });
  compact_timer_.expires_at(compact_timer_.expires_at() + compact_interval_);
  compact_timer_.async_wait([this, alive](const boost::system::error_code& ec) {
    if (!alive->load() || stopped_.load()) {
      return;
    }
    if (ec) {
      if (ec != boost::asio::error::operation_aborted) {
        LOG_WARN("compact_timer async_wait error: {}", ec.message());
      }
      return;
    }
    CheckAndCompactLog();
  });
}

void RaftDriver::CheckReady() {
  if (!rn_->HasReady()) {
    return;
  }
  auto ready = rn_->GetReady();
  if (ready.softState_) {
    LOG_INFO("soft state change, state:{}, lead:{}",
             eraft::ToString(ready.softState_->raftState_),
             ready.softState_->lead_);
    if (ready.softState_->raftState_ != eraft::StateLeader ||
        ready.softState_->lead_ != node_id_) {
      RejectPendingPromises(eraft::Error(fmt::format(
          "leadership changed, current leader {}", ready.softState_->lead_)));
    }
  }
  rocksdb::WriteBatch batch;
  if (!ready.entries_.empty()) {
    storage_->Append(std::move(ready.entries_), batch);
  }
  if (!eraft::IsEmptyHardState(ready.hardState_)) {
    storage_->SetHardState(ready.hardState_, batch);
  }
  if (!eraft::IsEmptySnap(ready.snapshot_)) {
    LOG_FATAL("snapshot should be empty");
  }
  storage_->WriteBatch(batch);
  {
    std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
    for (const auto& msg : ready.messages_) {
      auto iter = node_clients_.find(msg.to());
      if (iter != node_clients_.end()) {
        if (iter->second->connected()) {
          iter->second->Send(MessageToNetString(local_node_.graph, msg));
          if (mark_unreachable_.count(msg.to())) {
            mark_unreachable_.erase(msg.to());
          }
        } else {
          if (!mark_unreachable_.count(msg.to())) {
            LOG_WARN("report raft node {} is unreachable, {}", msg.to(),
                     msg.ShortDebugString());
            rn_->ReportUnreachable(msg.to());
            mark_unreachable_.insert(msg.to());
          }
        }
      } else {
        LOG_WARN("send msg but peer client id {} not exists", msg.to());
      }
    }
  }
  if (!eraft::IsEmptySnap(ready.snapshot_)) {
    LOG_FATAL("snapshot should be empty");
  }
  if (!ready.committedEntries_.empty()) {
    bool has_confchange = false;
    for (auto& entry : ready.committedEntries_) {
      if (entry.type() == raftpb::EntryConfChange) {
        has_confchange = true;
        break;
      }
    }
    if (!has_confchange) {
      auto alive = callback_alive_;
      manager_->apply_service(shard_id_).post(
          [this, alive,
           committedEntries = std::move(ready.committedEntries_)]() {
            if (!alive->load() || stopped_.load()) {
              return;
            }
            Apply(committedEntries);
          });
    } else {
      LOG_INFO("there is ConfChange in committed entries, entries size: {}",
               ready.committedEntries_.size());
      Apply(ready.committedEntries_);
    }
  }
  rn_->Advance({});
}

void RaftDriver::Apply(const std::vector<raftpb::Entry>& entries) {
  for (const auto& entry : entries) {
    switch (entry.type()) {
      case raftpb::EntryNormal: {
        if (entry.data().empty()) {
          continue;
        }
        meta::RaftRequest request;
        request.ParseFromString(entry.data());
        std::shared_ptr<raft::PromiseContext> context;
        {
          std::lock_guard<std::mutex> guard(promise_mutex_);
          auto iter = pending_promise_.find(request.id());
          if (iter != pending_promise_.end()) {
            context = iter->second;
            pending_promise_.erase(iter);
          }
        }
        if (context) {
          context->SetCommited(
              PromiseContext::CommitResult{nullptr, entry.index()});
        }
        eraft::Error apply_err = nullptr;
        try {
          apply_(entry.index(), request);
        } catch (const std::exception& e) {
          apply_err = eraft::Error(e.what());
        } catch (...) {
          apply_err = eraft::Error("unknown error");
        }
        if (apply_err == nullptr) {
          apply_id_.store(entry.index());
        }
        if (context) {
          context->SetApplied(
              PromiseContext::ApplyResult{apply_err, entry.index()});
          ReleaseProposalAccounting(context);
        }
        if (apply_err != nullptr) {
          LOG_FATAL("failed to apply committed raft request at index {}: {}",
                    entry.index(), apply_err.String());
        }
        break;
      }
      case raftpb::EntryConfChange: {
        raftpb::ConfChange cc;
        if (!cc.ParseFromString(entry.data())) {
          LOG_FATAL("failed to parse ConfChange data");
        }
        auto confstate = rn_->ApplyConfChange(raftpb::ConfChangeWrap(cc));
        meta::RaftNodeInfo node_info;
        node_info.ParseFromString(cc.context());
        switch (cc.type()) {
          case raftpb::ConfChangeType::ConfChangeAddLearnerNode:
          case raftpb::ConfChangeType::ConfChangeAddNode: {
            if (cc.type() == raftpb::ConfChangeType::ConfChangeAddNode) {
              LOG_INFO("add node: {}", node_info.ShortDebugString());
            } else {
              LOG_INFO("add learner: {}", node_info.ShortDebugString());
            }
            std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
            if (!node_infos_.nodes().count(node_info.node_id())) {
              node_infos_.mutable_nodes()->insert(
                  {node_info.node_id(), node_info});
              auto client = manager_->AcquireClient(node_info.ip(),
                                                    node_info.raft_poft());
              node_clients_.emplace(node_info.node_id(), std::move(client));
            } else {
              LOG_ERROR("node id {} has already existed", node_info.node_id());
            }
            break;
          }
          case raftpb::ConfChangeType::ConfChangeRemoveNode: {
            LOG_INFO("remove node: {}", node_info.node_id());
            std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
            if (node_infos_.nodes().count(node_info.node_id())) {
              node_clients_.erase(node_info.node_id());
              node_infos_.mutable_nodes()->erase(node_info.node_id());
            } else {
              LOG_ERROR("no such node id {}", node_info.node_id());
            }
            break;
          }
          case raftpb::ConfChangeType::ConfChangeUpdateNode: {
            LOG_INFO("update node: {}", cc.ShortDebugString());
            break;
          }
          default: {
            break;
          }
        }

        LOG_INFO("new conf state: {}", confstate->ShortDebugString());
        rocksdb::WriteBatch wb;
        storage_->SetConfState(*confstate, wb);
        storage_->SetNodeInfos(node_infos_.SerializeAsString(), wb);
        storage_->SetApplyIndex(entry.index(), wb);
        storage_->WriteBatch(wb);

        std::shared_ptr<raft::PromiseContext> context;
        {
          std::lock_guard<std::mutex> guard(promise_mutex_);
          auto iter = pending_promise_.find(cc.id());
          if (iter != pending_promise_.end()) {
            context = iter->second;
            pending_promise_.erase(iter);
          }
        }
        if (context) {
          context->SetCommited(
              PromiseContext::CommitResult{nullptr, entry.index()});
          context->SetApplied(
              PromiseContext::ApplyResult{nullptr, entry.index()});
          ReleaseProposalAccounting(context);
        }
        break;
      }
      default: {
        LOG_ERROR("unhandled entry : {}", entry.ShortDebugString());
      }
    }
  }
}

}  // namespace raft
