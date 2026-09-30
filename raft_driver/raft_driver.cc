#include "raft_driver/raft_driver.h"

#include <gflags/gflags.h>
#include <pthread.h>

#include <array>
#include <boost/asio.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/lexical_cast.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <shared_mutex>
#include <string_view>

#include "common/exception.h"
#include "common/logger.h"

using boost::asio::async_write;
using boost::asio::ip::tcp;
using namespace std::chrono;

namespace raft {
namespace {
constexpr uint32_t kSnapshotFormatVersion = 1;
constexpr size_t kSnapshotChunkSize = 1024 * 1024;
constexpr std::array<uint8_t, 4> kRaftMagicCode = {0x17, 0xB0, 0x60, 0x60};

uint32_t SnapshotChecksum(std::string_view data) {
  uint32_t crc = 0xffffffffU;
  for (unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

std::pair<uint32_t, eraft::Error> SnapshotFileChecksum(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {0, eraft::Error("failed to open snapshot file " + path.string())};
  }
  uint32_t crc = 0xffffffffU;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = input.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      crc ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
      }
    }
  }
  if (!input.eof()) {
    return {0, eraft::Error("failed to read snapshot file " + path.string())};
  }
  return {~crc, nullptr};
}

bool IsSafeSnapshotFileName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  std::filesystem::path path(name);
  if (path.is_absolute()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

std::string SnapshotTransferKey(std::string_view snapshot_id,
                                uint64_t target_node_id) {
  return fmt::format("{}:{}", snapshot_id, target_node_id);
}

std::string EnvelopeToNetString(const meta::RaftMessage& envelope) {
  uint32_t msg_size = envelope.ByteSizeLong();
  std::string str;
  str.reserve(msg_size + sizeof(uint32_t));
  boost::endian::native_to_big_inplace(msg_size);
  str.append(reinterpret_cast<const char*>(&msg_size), sizeof(msg_size));
  str.append(envelope.SerializeAsString());
  return str;
}

eraft::Error WriteEnvelope(tcp::socket* socket,
                           const meta::RaftMessage& envelope) {
  auto data = EnvelopeToNetString(envelope);
  boost::system::error_code ec;
  boost::asio::write(*socket, boost::asio::buffer(data), ec);
  if (ec) {
    return eraft::Error("failed to send snapshot frame: " + ec.message());
  }
  return nullptr;
}

eraft::Error ConnectSnapshotSocket(const std::string& ip, int port,
                                   tcp::socket* socket) {
  boost::system::error_code ec;
  socket->connect(
      tcp::endpoint(boost::asio::ip::address::from_string(ip), port), ec);
  if (ec) {
    return eraft::Error("failed to connect snapshot transport: " +
                        ec.message());
  }
  boost::asio::write(*socket, boost::asio::buffer(kRaftMagicCode), ec);
  if (ec) {
    return eraft::Error("failed to send snapshot transport magic: " +
                        ec.message());
  }
  return nullptr;
}

eraft::Error SendSnapshotFiles(
    const meta::RaftNodeInfo& target, uint64_t source_node_id,
    const std::filesystem::path& root,
    const meta::GraphSnapshotDescriptor& descriptor) {
  boost::asio::io_service service;
  tcp::socket socket(service);
  auto err = ConnectSnapshotSocket(target.ip(), target.raft_poft(), &socket);
  if (err != nullptr) {
    return err;
  }

  bool include_descriptor = true;
  std::vector<char> buffer(kSnapshotChunkSize);
  for (const auto& file : descriptor.files()) {
    if (!IsSafeSnapshotFileName(file.name())) {
      return eraft::Error("unsafe snapshot file name " + file.name());
    }
    std::ifstream input(root / file.name(), std::ios::binary);
    if (!input) {
      return eraft::Error("failed to open snapshot file " + file.name());
    }
    uint64_t offset = 0;
    do {
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      if (count == 0 && !input.eof()) {
        return eraft::Error("failed to read snapshot file " + file.name());
      }
      meta::RaftMessage envelope;
      envelope.set_graph(descriptor.graph());
      auto* chunk = envelope.mutable_snapshot_chunk();
      if (include_descriptor) {
        *chunk->mutable_snapshot_descriptor() = descriptor;
        include_descriptor = false;
      }
      chunk->set_source_node_id(source_node_id);
      chunk->set_target_node_id(target.node_id());
      chunk->set_snapshot_id(descriptor.snapshot_id());
      chunk->set_file_name(file.name());
      chunk->set_offset(offset);
      chunk->set_data(buffer.data(), static_cast<size_t>(count));
      chunk->set_checksum(SnapshotChecksum(chunk->data()));
      chunk->set_file_done(input.eof());
      err = WriteEnvelope(&socket, envelope);
      if (err != nullptr) {
        return err;
      }
      offset += static_cast<uint64_t>(count);
    } while (!input.eof());
  }

  meta::RaftMessage envelope;
  envelope.set_graph(descriptor.graph());
  auto* chunk = envelope.mutable_snapshot_chunk();
  if (include_descriptor) {
    *chunk->mutable_snapshot_descriptor() = descriptor;
  }
  chunk->set_source_node_id(source_node_id);
  chunk->set_target_node_id(target.node_id());
  chunk->set_snapshot_id(descriptor.snapshot_id());
  chunk->set_snapshot_done(true);
  return WriteEnvelope(&socket, envelope);
}

eraft::Error SendSnapshotStatusFrame(const meta::RaftNodeInfo& target,
                                     const std::string& graph,
                                     const meta::GraphSnapshotStatus& status) {
  boost::asio::io_service service;
  tcp::socket socket(service);
  auto err = ConnectSnapshotSocket(target.ip(), target.raft_poft(), &socket);
  if (err != nullptr) {
    return err;
  }
  meta::RaftMessage envelope;
  envelope.set_graph(graph);
  *envelope.mutable_snapshot_status() = status;
  return WriteEnvelope(&socket, envelope);
}

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
                               const raftpb::Message& msg,
                               const meta::RaftNodeInfo& source_node) {
  meta::RaftMessage envelope;
  envelope.set_graph(graph);
  envelope.mutable_message()->CopyFrom(msg);
  envelope.mutable_source_node()->CopyFrom(source_node);
  return EnvelopeToNetString(envelope);
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

RaftDriver::RaftDriver(ApplyRequest apply, ApplyConfChange apply_conf_change,
                       uint64_t apply_id,
                       std::optional<raftpb::ConfState> conf_state,
                       std::optional<meta::RaftNodeInfos> node_infos,
                       LocalNodeConfig local_node,
                       const RaftLogStoreConfig& store_config,
                       const RaftConfig& config, CreateSnapshot create_snapshot,
                       ApplySnapshot apply_snapshot)
    : RaftDriver(std::move(apply), std::move(apply_conf_change), apply_id,
                 std::move(conf_state), std::move(node_infos),
                 std::move(local_node), {}, store_config, config,
                 std::move(create_snapshot), std::move(apply_snapshot), false,
                 std::nullopt) {}

RaftDriver::RaftDriver(ApplyRequest apply, ApplyConfChange apply_conf_change,
                       uint64_t apply_id,
                       std::optional<raftpb::ConfState> conf_state,
                       std::optional<meta::RaftNodeInfos> node_infos,
                       LocalNodeConfig local_node,
                       std::vector<eraft::Peer> init_peers,
                       const RaftLogStoreConfig& store_config,
                       const RaftConfig& config)
    : RaftDriver(std::move(apply), std::move(apply_conf_change), apply_id,
                 std::move(conf_state), std::move(node_infos),
                 std::move(local_node), std::move(init_peers), store_config,
                 config, {}, {}, false, std::nullopt) {}

RaftDriver::RaftDriver(ApplyRequest apply, ApplyConfChange apply_conf_change,
                       uint64_t apply_id,
                       std::optional<raftpb::ConfState> conf_state,
                       std::optional<meta::RaftNodeInfos> node_infos,
                       LocalNodeConfig local_node,
                       std::vector<eraft::Peer> init_peers,
                       const RaftLogStoreConfig& store_config,
                       const RaftConfig& config, CreateSnapshot create_snapshot,
                       ApplySnapshot apply_snapshot, bool join_existing,
                       std::optional<meta::RaftNodeInfos> join_node_infos)
    : manager_(RaftManager::Instance()),
      callback_alive_(std::make_shared<std::atomic<bool>>(false)),
      apply_(std::move(apply)),
      apply_conf_change_(std::move(apply_conf_change)),
      create_snapshot_(std::move(create_snapshot)),
      apply_snapshot_(std::move(apply_snapshot)),
      apply_id_(apply_id),
      initial_conf_state_(std::move(conf_state)),
      local_node_(std::move(local_node)),
      shard_id_(manager_->PickShard(local_node_.graph)),
      node_id_(0),
      init_peers_(std::move(init_peers)),
      join_existing_(join_existing),
      tick_interval_(config.tick_interval),
      tick_timer_(manager_->timer_service(shard_id_), tick_interval_),
      compact_interval_(store_config.gc_interval * 60 * 1000),
      compact_timer_(manager_->timer_service(shard_id_), compact_interval_),
      store_config_(store_config),
      raft_config_(config) {
  if (node_infos.has_value()) {
    node_infos_ = std::move(*node_infos);
  } else if (join_node_infos.has_value()) {
    node_infos_ = std::move(*join_node_infos);
  }
}

eraft::Error RaftDriver::Run() {
  stopped_.store(false);
  callback_alive_->store(true);
  if (!local_node_.Check()) {
    return eraft::Error("invalid local node config");
  }
  if (store_config_.shared_block_cache == nullptr) {
    return eraft::Error("raft log store shared_block_cache is required");
  }
  if (!apply_ || !apply_conf_change_) {
    return eraft::Error("raft state machine apply callbacks are required");
  }
  if (store_config_.snapshot_path.empty()) {
    store_config_.snapshot_path =
        (std::filesystem::path(store_config_.path).parent_path() / "snapshots")
            .string();
  }
  std::filesystem::create_directories(
      std::filesystem::path(store_config_.snapshot_path) / "outgoing");
  std::filesystem::create_directories(
      std::filesystem::path(store_config_.snapshot_path) / "incoming");
  if (apply_id_.load() > 0 &&
      (!initial_conf_state_.has_value() || node_infos_.nodes().empty())) {
    return eraft::Error(
        "persisted raft applied index requires ConfState and node infos");
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
  auto applied = apply_id_.load();
  if (initial_conf_state_.has_value()) {
    storage_->SetInitialConfState(*initial_conf_state_);
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
    if (!join_existing_) {
      err = rn_->Bootstrap(init_peers_);
      if (err != nullptr) {
        return err;
      }
      // Persist and apply bootstrap ConfChange entries before Run returns, so
      // a crash after graph metadata is visible can still recover local node
      // infos.
      CheckReady();
      manager_->WaitForApplyService(shard_id_);
      manager_->WaitForRaftService(shard_id_);
    } else {
      LOG_INFO("raft node {} is waiting to join graph [{}]", node_id_,
               local_node_.graph);
    }
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
  manager_->WaitForRaftService(shard_id_);
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    pending_snapshot_messages_.clear();
    incoming_snapshots_.clear();
  }
  snapshot_threads_.clear();
  manager_->WaitForRaftService(shard_id_);
  node_clients_.clear();
  if (storage_) {
    storage_->Close();
  }
  LOG_INFO("bolt raft driver stopped");
}

void RaftDriver::Step(raftpb::Message msg,
                      std::optional<meta::RaftNodeInfo> source_node) {
  if (stopped_.load()) {
    return;
  }
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive, msg = std::move(msg),
                                          source_node = std::move(
                                              source_node)]() mutable {
    if (!alive->load() || stopped_.load()) {
      return;
    }
    if (source_node.has_value() && source_node->node_id() == msg.from() &&
        source_node->node_id() != 0 &&
        source_node->graph() == local_node_.graph &&
        !source_node->ip().empty() && source_node->raft_poft() > 0) {
      std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
      if (!node_clients_.count(source_node->node_id())) {
        node_clients_.emplace(source_node->node_id(),
                              manager_->AcquireClient(
                                  source_node->ip(), source_node->raft_poft()));
      }
    }
    auto err = rn_->Step(std::move(msg));
    if (err != nullptr) {
      LOG_WARN("failed to step message, err: {}", err.String());
      return;
    }
    CheckReady();
  });
}

void RaftDriver::ReceiveSnapshotChunk(meta::GraphSnapshotChunk chunk) {
  if (stopped_.load() || chunk.target_node_id() != node_id_) {
    return;
  }

  meta::GraphSnapshotStatus status;
  status.set_snapshot_id(chunk.snapshot_id());
  status.set_source_node_id(chunk.source_node_id());
  status.set_target_node_id(node_id_);
  status.set_success(false);
  std::optional<meta::RaftNodeInfo> source;
  bool send_status = false;

  try {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (chunk.has_snapshot_descriptor()) {
      const auto& descriptor = chunk.snapshot_descriptor();
      status.set_snapshot_id(descriptor.snapshot_id());
      RG_CHECK(descriptor.format_version() == kSnapshotFormatVersion,
               common::ErrorCode::InvalidParameter,
               "unsupported graph snapshot format {}",
               descriptor.format_version());
      RG_CHECK(descriptor.graph() == local_node_.graph,
               common::ErrorCode::InvalidParameter,
               "snapshot graph [{}] does not match local graph [{}]",
               descriptor.graph(), local_node_.graph);
      RG_CHECK(!descriptor.snapshot_id().empty(),
               common::ErrorCode::InvalidParameter,
               "snapshot id should not be empty");
      auto source_iter =
          descriptor.node_infos().nodes().find(chunk.source_node_id());
      RG_CHECK(source_iter != descriptor.node_infos().nodes().end(),
               common::ErrorCode::InvalidParameter,
               "snapshot source node {} is missing from descriptor",
               chunk.source_node_id());
      source = source_iter->second;

      auto incoming_root = std::filesystem::path(store_config_.snapshot_path) /
                           "incoming" / descriptor.snapshot_id();
      std::error_code ec;
      std::filesystem::remove_all(incoming_root, ec);
      if (ec) {
        RG_THROW(common::ErrorCode::StorageEngineError,
                 "failed to reset incoming snapshot directory: {}",
                 ec.message());
      }
      std::filesystem::create_directories(incoming_root / "data");
      incoming_snapshots_[descriptor.snapshot_id()] =
          IncomingSnapshot{.descriptor = descriptor};
    }

    auto incoming = incoming_snapshots_.find(status.snapshot_id());
    RG_CHECK(incoming != incoming_snapshots_.end(),
             common::ErrorCode::InvalidParameter,
             "snapshot chunk arrived before its descriptor");
    const auto& descriptor = incoming->second.descriptor;
    RG_CHECK(chunk.snapshot_id() == descriptor.snapshot_id(),
             common::ErrorCode::InvalidParameter,
             "snapshot chunk id [{}] does not match descriptor [{}]",
             chunk.snapshot_id(), descriptor.snapshot_id());
    if (!source.has_value()) {
      auto source_iter =
          descriptor.node_infos().nodes().find(chunk.source_node_id());
      RG_CHECK(source_iter != descriptor.node_infos().nodes().end(),
               common::ErrorCode::InvalidParameter,
               "snapshot source node {} is missing from descriptor",
               chunk.source_node_id());
      source = source_iter->second;
    }

    if (!chunk.file_name().empty()) {
      RG_CHECK(IsSafeSnapshotFileName(chunk.file_name()),
               common::ErrorCode::InvalidParameter,
               "unsafe snapshot file name [{}]", chunk.file_name());
      const meta::GraphSnapshotFile* expected = nullptr;
      for (const auto& file : descriptor.files()) {
        if (file.name() == chunk.file_name()) {
          expected = &file;
          break;
        }
      }
      RG_CHECK(expected != nullptr, common::ErrorCode::InvalidParameter,
               "snapshot file [{}] is not present in the manifest",
               chunk.file_name());
      RG_CHECK(!incoming->second.completed_files.contains(chunk.file_name()),
               common::ErrorCode::InvalidParameter,
               "snapshot file [{}] was already completed", chunk.file_name());
      RG_CHECK(chunk.offset() <= expected->size() &&
                   chunk.data().size() <= expected->size() - chunk.offset(),
               common::ErrorCode::StorageEngineError,
               "snapshot chunk for file [{}] exceeds its manifest size",
               chunk.file_name());
      RG_CHECK(SnapshotChecksum(chunk.data()) == chunk.checksum(),
               common::ErrorCode::StorageEngineError,
               "snapshot chunk checksum mismatch for file [{}] at offset {}",
               chunk.file_name(), chunk.offset());

      auto file_path = std::filesystem::path(store_config_.snapshot_path) /
                       "incoming" / descriptor.snapshot_id() / "data" /
                       chunk.file_name();
      std::filesystem::create_directories(file_path.parent_path());
      if (chunk.offset() == 0) {
        std::ofstream output(file_path, std::ios::binary | std::ios::trunc);
        RG_CHECK(output.good(), common::ErrorCode::StorageEngineError,
                 "failed to create incoming snapshot file [{}]",
                 file_path.string());
        output.write(chunk.data().data(), chunk.data().size());
        RG_CHECK(output.good(), common::ErrorCode::StorageEngineError,
                 "failed to write incoming snapshot file [{}]",
                 file_path.string());
      } else {
        std::error_code ec;
        const auto current_size = std::filesystem::file_size(file_path, ec);
        RG_CHECK(!ec && current_size == chunk.offset(),
                 common::ErrorCode::StorageEngineError,
                 "snapshot file [{}] expected offset {}, actual {}",
                 chunk.file_name(), chunk.offset(), ec ? 0 : current_size);
        std::ofstream output(file_path, std::ios::binary | std::ios::app);
        RG_CHECK(output.good(), common::ErrorCode::StorageEngineError,
                 "failed to append incoming snapshot file [{}]",
                 file_path.string());
        output.write(chunk.data().data(), chunk.data().size());
        RG_CHECK(output.good(), common::ErrorCode::StorageEngineError,
                 "failed to append incoming snapshot file [{}]",
                 file_path.string());
      }

      if (chunk.file_done()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file_path, ec);
        RG_CHECK(!ec && size == expected->size(),
                 common::ErrorCode::StorageEngineError,
                 "snapshot file [{}] size mismatch", chunk.file_name());
        auto [checksum, checksum_err] = SnapshotFileChecksum(file_path);
        RG_CHECK(checksum_err == nullptr && checksum == expected->checksum(),
                 common::ErrorCode::StorageEngineError,
                 "snapshot file [{}] checksum mismatch", chunk.file_name());
        incoming->second.completed_files.insert(chunk.file_name());
      }
    }

    if (chunk.snapshot_done()) {
      RG_CHECK(incoming->second.completed_files.size() ==
                   static_cast<size_t>(descriptor.files_size()),
               common::ErrorCode::StorageEngineError,
               "snapshot [{}] is incomplete", descriptor.snapshot_id());
      status.set_success(true);
      send_status = true;
      incoming_snapshots_.erase(incoming);
    }
  } catch (const std::exception& e) {
    status.set_error(e.what());
    send_status = true;
  }

  if (!send_status || !source.has_value()) {
    return;
  }
  auto graph = local_node_.graph;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_threads_.emplace_back([source = std::move(*source),
                                    graph = std::move(graph),
                                    status = std::move(status)]() {
      auto err = SendSnapshotStatusFrame(source, graph, status);
      if (err != nullptr) {
        LOG_WARN("failed to send graph snapshot status: {}", err.String());
      }
    });
  }
}

void RaftDriver::ReceiveSnapshotStatus(meta::GraphSnapshotStatus status) {
  if (stopped_.load() || status.source_node_id() != node_id_) {
    return;
  }
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post(
      [this, alive, status = std::move(status)]() mutable {
        if (!alive->load() || stopped_.load()) {
          return;
        }
        HandleSnapshotStatus(std::move(status));
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

PromiseContext::ApplyResult RaftDriver::ProposeConfChangeAndWait(
    raftpb::ConfChange cc) {
  std::lock_guard<std::mutex> guard(confchange_mutex_);
  auto context = ProposeConfChange(cc);
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
    RG_THROW(common::ErrorCode::InvalidParameter,
             "write batch kind must be specified");
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

eraft::Error RaftDriver::TransferLeader(uint64_t node_id) {
  std::promise<eraft::Error> promise;
  auto future = promise.get_future();
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive, node_id, &promise]() {
    if (!alive->load() || stopped_.load()) {
      promise.set_value(eraft::Error("raft driver stopped"));
      return;
    }
    auto status = rn_->GetStatus();
    if (status.basicStatus_.softState_.lead_ != node_id_) {
      promise.set_value(eraft::Error("not leader"));
      return;
    }
    if (node_id == node_id_) {
      promise.set_value(eraft::Error("target node is already leader"));
      return;
    }
    auto progress = status.progress_.find(node_id);
    if (progress == status.progress_.end()) {
      promise.set_value(eraft::Error("target node is not a raft member"));
      return;
    }
    if (progress->second.isLearner_) {
      promise.set_value(
          eraft::Error("target node is a learner and cannot become leader"));
      return;
    }
    rn_->TransferLeader(node_id);
    CheckReady();
    promise.set_value(nullptr);
  });
  return future.get();
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
    const auto leader = rs.s.basicStatus_.softState_.lead_;
    std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
    rs.nodes.reserve(node_infos_.nodes_size());
    for (const auto& [node_id, node_info] : node_infos_.nodes()) {
      RaftStatus::NodeStatus node_status;
      node_status.node_info = node_info;
      node_status.node_info.set_is_leader(node_id == leader);
      node_status.reachable = node_id == node_id_;
      auto client = node_clients_.find(node_id);
      if (client != node_clients_.end() && client->second != nullptr) {
        node_status.reachable =
            node_status.reachable || client->second->connected();
      }
      auto progress = rs.s.progress_.find(node_id);
      if (progress != rs.s.progress_.end()) {
        node_status.match_index = progress->second.match_;
        node_status.next_index = progress->second.next_;
      }
      rs.nodes.emplace_back(std::move(node_status));
    }
    promise.set_value(rs);
  });
  return future.get();
}

eraft::Error RaftDriver::CreateSnapshotAndCompact() {
  if (!create_snapshot_) {
    return eraft::Error("graph snapshot callbacks are not configured");
  }
  if (stopped_.load()) {
    return eraft::Error("raft driver stopped");
  }

  std::promise<SnapshotBuildResult> build_promise;
  auto build_future = build_promise.get_future();
  auto alive = callback_alive_;
  manager_->apply_service(shard_id_).post(
      [this, alive, &build_promise]() mutable {
        if (!alive->load() || stopped_.load()) {
          SnapshotBuildResult result;
          result.err = eraft::Error("raft driver stopped");
          build_promise.set_value(std::move(result));
          return;
        }
        try {
          build_promise.set_value(create_snapshot_());
        } catch (const std::exception& e) {
          SnapshotBuildResult result;
          result.err = eraft::Error(e.what());
          build_promise.set_value(std::move(result));
        }
      });
  auto result = build_future.get();
  if (result.err != nullptr) {
    return result.err;
  }

  std::promise<eraft::Error> persist_promise;
  auto persist_future = persist_promise.get_future();
  manager_->raft_service(shard_id_).post(
      [this, alive, result = std::move(result), &persist_promise]() mutable {
        if (!alive->load() || stopped_.load()) {
          persist_promise.set_value(eraft::Error("raft driver stopped"));
          return;
        }
        const auto index = result.descriptor.index();
        if (index == 0) {
          persist_promise.set_value(
              eraft::Error("cannot create a snapshot at raft index zero"));
          return;
        }
        auto [term, term_err] = storage_->Term(index);
        if (term_err != nullptr) {
          persist_promise.set_value(std::move(term_err));
          return;
        }
        result.descriptor.set_term(term);
        result.descriptor.set_format_version(kSnapshotFormatVersion);
        std::string data;
        if (!result.descriptor.SerializeToString(&data)) {
          persist_promise.set_value(
              eraft::Error("failed to serialize graph snapshot descriptor"));
          return;
        }
        auto err = storage_->CreateSnapshot(index, term, result.conf_state,
                                            std::move(data));
        if (err == nullptr || err == eraft::ErrSnapOutOfDate) {
          storage_->Compact(index);
          LOG_INFO("created graph snapshot [{}] at raft index {} term {}",
                   result.descriptor.snapshot_id(), index, term);
          persist_promise.set_value(nullptr);
          return;
        }
        persist_promise.set_value(std::move(err));
      });
  return persist_future.get();
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
        auto [snapshot, snapshot_err] = storage_->Snapshot();
        if (snapshot_err == nullptr &&
            snapshot.metadata().index() >= compacted) {
          storage_->Compact(compacted);
          LOG_INFO("compact raft log, compacted index:{}, applied index:{}",
                   compacted, applied);
        } else {
          LOG_WARN(
              "skip raft log compaction at {} because no covering graph "
              "snapshot is available",
              compacted);
        }
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

void RaftDriver::StartSnapshotTransfer(const raftpb::Message& message) {
  meta::GraphSnapshotDescriptor descriptor;
  if (!descriptor.ParseFromString(message.snapshot().data())) {
    LOG_ERROR("failed to parse graph snapshot descriptor for node {}",
              message.to());
    rn_->ReportSnapshot(message.to(), eraft::SnapshotFailure);
    return;
  }
  auto transfer_message = message;
  meta::RaftNodeInfo target_node;
  auto target = descriptor.node_infos().nodes().find(message.to());
  if (target == descriptor.node_infos().nodes().end()) {
    // A snapshot may have been created just before the membership entry that
    // introduced this learner was applied. The checkpoint is still valid;
    // use the current committed endpoint only to route the bulk transfer.
    std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
    target = node_infos_.nodes().find(message.to());
    if (target == node_infos_.nodes().end()) {
      LOG_ERROR(
          "snapshot target node {} is missing from descriptor and current "
          "node infos",
          message.to());
      rn_->ReportSnapshot(message.to(), eraft::SnapshotFailure);
      return;
    }
    target_node = target->second;
    (*descriptor.mutable_node_infos()->mutable_nodes())[message.to()] =
        target_node;
    if (!descriptor.SerializeToString(
            transfer_message.mutable_snapshot()->mutable_data())) {
      rn_->ReportSnapshot(message.to(), eraft::SnapshotFailure);
      return;
    }
  } else {
    target_node = target->second;
  }

  const auto key = SnapshotTransferKey(descriptor.snapshot_id(), message.to());
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    pending_snapshot_messages_[key] = std::move(transfer_message);
  }
  const auto root = std::filesystem::path(store_config_.snapshot_path) /
                    "outgoing" / descriptor.snapshot_id() / "data";
  auto alive = callback_alive_;
  std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
  snapshot_threads_.emplace_back([this, alive, target = std::move(target_node),
                                  descriptor = std::move(descriptor),
                                  root]() mutable {
    auto err = SendSnapshotFiles(target, node_id_, root, descriptor);
    if (err == nullptr) {
      return;
    }
    meta::GraphSnapshotStatus status;
    status.set_snapshot_id(descriptor.snapshot_id());
    status.set_source_node_id(node_id_);
    status.set_target_node_id(target.node_id());
    status.set_success(false);
    status.set_error(err.String());
    manager_->raft_service(shard_id_).post(
        [this, alive, status = std::move(status)]() mutable {
          if (!alive->load() || stopped_.load()) {
            return;
          }
          HandleSnapshotStatus(std::move(status));
        });
  });
}

void RaftDriver::HandleSnapshotStatus(meta::GraphSnapshotStatus status) {
  const auto key =
      SnapshotTransferKey(status.snapshot_id(), status.target_node_id());
  raftpb::Message message;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    auto pending = pending_snapshot_messages_.find(key);
    if (pending == pending_snapshot_messages_.end()) {
      LOG_WARN("ignore status for unknown snapshot transfer [{}] to node {}",
               status.snapshot_id(), status.target_node_id());
      return;
    }
    message = std::move(pending->second);
    pending_snapshot_messages_.erase(pending);
  }

  if (!status.success()) {
    LOG_WARN("snapshot [{}] transfer to node {} failed: {}",
             status.snapshot_id(), status.target_node_id(), status.error());
    rn_->ReportSnapshot(status.target_node_id(), eraft::SnapshotFailure);
    CheckReady();
    return;
  }

  std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
  auto client = node_clients_.find(message.to());
  if (client == node_clients_.end() || !client->second->connected()) {
    LOG_WARN(
        "snapshot [{}] data reached node {}, but its raft transport is "
        "not connected",
        status.snapshot_id(), message.to());
    rn_->ReportSnapshot(message.to(), eraft::SnapshotFailure);
    CheckReady();
    return;
  }
  client->second->Send(
      MessageToNetString(local_node_.graph, message, LocalNodeInfo()));
  rn_->ReportSnapshot(message.to(), eraft::SnapshotFinish);
  mark_unreachable_.erase(message.to());
  lock.unlock();
  CheckReady();
}

void RaftDriver::SendReadyMessages(
    const std::vector<raftpb::Message>& messages) {
  const auto source_node = LocalNodeInfo();
  std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
  for (const auto& msg : messages) {
    if (msg.type() == raftpb::MsgSnap) {
      lock.unlock();
      StartSnapshotTransfer(msg);
      lock.lock();
      continue;
    }
    auto iter = node_clients_.find(msg.to());
    if (iter != node_clients_.end()) {
      if (iter->second->connected()) {
        iter->second->Send(
            MessageToNetString(local_node_.graph, msg, source_node));
        mark_unreachable_.erase(msg.to());
      } else if (!mark_unreachable_.count(msg.to())) {
        LOG_WARN("report raft node {} is unreachable, {}", msg.to(),
                 msg.ShortDebugString());
        rn_->ReportUnreachable(msg.to());
        mark_unreachable_.insert(msg.to());
      }
    } else {
      LOG_WARN("send msg but peer client id {} not exists", msg.to());
    }
  }
}

meta::RaftNodeInfo RaftDriver::LocalNodeInfo() const {
  meta::RaftNodeInfo node;
  node.set_node_id(node_id_);
  node.set_ip(local_node_.ip);
  node.set_bolt_port(local_node_.bolt_port);
  node.set_raft_poft(local_node_.raft_poft);
  node.set_graph(local_node_.graph);
  return node;
}

void RaftDriver::UpdateNodeInfosFromSnapshot(
    const meta::GraphSnapshotDescriptor& descriptor) {
  std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
  node_clients_.clear();
  node_infos_ = descriptor.node_infos();
  for (const auto& [id, node] : node_infos_.nodes()) {
    if (id == node_id_) {
      continue;
    }
    node_clients_[id] = manager_->AcquireClient(node.ip(), node.raft_poft());
  }
}

void RaftDriver::ApplyReadySnapshot(eraft::Ready ready) {
  snapshot_ready_pending_ = true;
  meta::GraphSnapshotDescriptor descriptor;
  if (!descriptor.ParseFromString(ready.snapshot_.data())) {
    LOG_FATAL("failed to parse incoming graph snapshot descriptor");
  }
  auto alive = callback_alive_;
  manager_->apply_service(shard_id_).post(
      [this, alive, ready = std::move(ready),
       descriptor = std::move(descriptor)]() mutable {
        try {
          if (!alive->load() || stopped_.load()) {
            return;
          }
          if (!apply_snapshot_) {
            RG_THROW(common::ErrorCode::StorageEngineError,
                     "graph snapshot apply callback is not configured");
          }
          apply_snapshot_(descriptor);
        } catch (const std::exception& e) {
          LOG_FATAL("failed to install graph snapshot [{}]: {}",
                    descriptor.snapshot_id(), e.what());
        }

        manager_->raft_service(shard_id_).post(
            [this, alive, ready = std::move(ready),
             descriptor = std::move(descriptor)]() mutable {
              if (!alive->load() || stopped_.load()) {
                return;
              }
              rocksdb::WriteBatch batch;
              auto err = storage_->ApplySnapshot(ready.snapshot_, batch);
              if (err != nullptr && err != eraft::ErrSnapOutOfDate) {
                LOG_FATAL("failed to persist incoming raft snapshot: {}",
                          err.String());
              }
              if (!eraft::IsEmptyHardState(ready.hardState_)) {
                storage_->SetHardState(ready.hardState_, batch);
              }
              storage_->WriteBatch(batch);
              apply_id_.store(ready.snapshot_.metadata().index());
              UpdateNodeInfosFromSnapshot(descriptor);
              SendReadyMessages(ready.messages_);
              rn_->Advance({});
              snapshot_ready_pending_ = false;
              if (rn_->HasReady()) {
                CheckReady();
              }
            });
      });
}

void RaftDriver::CheckReady() {
  if (snapshot_ready_pending_) {
    return;
  }
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
  if (!eraft::IsEmptySnap(ready.snapshot_)) {
    ApplyReadySnapshot(std::move(ready));
    return;
  }
  rocksdb::WriteBatch batch;
  if (!ready.entries_.empty()) {
    storage_->Append(std::move(ready.entries_), batch);
  }
  if (!eraft::IsEmptyHardState(ready.hardState_)) {
    storage_->SetHardState(ready.hardState_, batch);
  }
  storage_->WriteBatch(batch);
  SendReadyMessages(ready.messages_);
  if (ready.committedEntries_.empty()) {
    rn_->Advance({});
    if (rn_->HasReady()) {
      CheckReady();
    }
    return;
  }

  // Prepare RawNode-owned state on the Raft thread and hand the state-machine
  // operations to the shard's FIFO apply worker. The handoff is the durability
  // boundary for Advance: GraphDB apply may still be running, while its durable
  // applied index and proposal completion continue to advance strictly in log
  // order on the apply worker.
  auto operations = PrepareApplyOperations(ready.committedEntries_);
  std::shared_ptr<std::promise<void>> raft_advanced;
  for (auto& operation : operations) {
    if (operation.type != ApplyOperation::Type::ConfChange) {
      continue;
    }
    if (!raft_advanced) {
      raft_advanced = std::make_shared<std::promise<void>>();
      auto advanced = raft_advanced->get_future().share();
      for (auto& candidate : operations) {
        if (candidate.type == ApplyOperation::Type::ConfChange) {
          candidate.raft_advanced = advanced;
        }
      }
    }
    break;
  }
  if (!operations.empty()) {
    manager_->apply_service(shard_id_).post(
        [this, operations = std::move(operations)]() mutable {
          Apply(operations);
        });
  }
  rn_->Advance({});
  if (raft_advanced) {
    raft_advanced->set_value();
  }
  if (rn_->HasReady()) {
    CheckReady();
  }
}

std::vector<RaftDriver::ApplyOperation> RaftDriver::PrepareApplyOperations(
    const std::vector<raftpb::Entry>& entries) {
  std::vector<ApplyOperation> operations;
  operations.reserve(entries.size());
  for (const auto& entry : entries) {
    switch (entry.type()) {
      case raftpb::EntryNormal: {
        ApplyOperation operation;
        operation.type = ApplyOperation::Type::Request;
        operation.index = entry.index();
        if (!entry.data().empty() &&
            !operation.request.ParseFromString(entry.data())) {
          LOG_FATAL("failed to parse raft request at index {}", entry.index());
        }
        if (!entry.data().empty()) {
          std::lock_guard<std::mutex> guard(promise_mutex_);
          auto iter = pending_promise_.find(operation.request.id());
          if (iter != pending_promise_.end()) {
            operation.context = iter->second;
            pending_promise_.erase(iter);
          }
        }
        if (operation.context) {
          operation.context->SetCommited(
              PromiseContext::CommitResult{nullptr, entry.index()});
        }
        operations.emplace_back(std::move(operation));
        break;
      }
      case raftpb::EntryConfChange: {
        raftpb::ConfChange cc;
        if (!cc.ParseFromString(entry.data())) {
          LOG_FATAL("failed to parse ConfChange data");
        }
        auto confstate = rn_->ApplyConfChange(raftpb::ConfChangeWrap(cc));
        meta::RaftNodeInfo node_info;
        if (!node_info.ParseFromString(cc.context())) {
          LOG_FATAL("failed to parse raft node info from ConfChange");
        }
        switch (cc.type()) {
          case raftpb::ConfChangeType::ConfChangeAddLearnerNode:
          case raftpb::ConfChangeType::ConfChangeAddNode: {
            if (cc.type() == raftpb::ConfChangeType::ConfChangeAddNode) {
              LOG_INFO("add node: {}", node_info.ShortDebugString());
            } else {
              LOG_INFO("add learner: {}", node_info.ShortDebugString());
            }
            std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
            auto* nodes = node_infos_.mutable_nodes();
            auto existing = nodes->find(node_info.node_id());
            const bool learner =
                cc.type() == raftpb::ConfChangeType::ConfChangeAddLearnerNode;
            node_info.set_is_learner(learner);
            node_info.set_is_leader(false);
            if (existing == nodes->end()) {
              nodes->insert({node_info.node_id(), node_info});
              auto client = manager_->AcquireClient(node_info.ip(),
                                                    node_info.raft_poft());
              node_clients_.emplace(node_info.node_id(), std::move(client));
            } else if (existing->second.is_learner() != learner) {
              existing->second = node_info;
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
            std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
            auto* nodes = node_infos_.mutable_nodes();
            auto existing = nodes->find(node_info.node_id());
            if (existing == nodes->end()) {
              LOG_ERROR("no such node id {}", node_info.node_id());
              break;
            }
            node_info.set_is_learner(existing->second.is_learner());
            node_info.set_is_leader(false);
            auto client =
                manager_->AcquireClient(node_info.ip(), node_info.raft_poft());
            node_clients_[node_info.node_id()] = std::move(client);
            existing->second = node_info;
            break;
          }
          default: {
            break;
          }
        }

        ApplyOperation operation;
        operation.type = ApplyOperation::Type::ConfChange;
        operation.index = entry.index();
        operation.conf_state = *confstate;
        {
          std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
          operation.node_infos = node_infos_;
        }
        LOG_INFO("new conf state: {}", confstate->ShortDebugString());
        {
          std::lock_guard<std::mutex> guard(promise_mutex_);
          auto iter = pending_promise_.find(cc.id());
          if (iter != pending_promise_.end()) {
            operation.context = iter->second;
            pending_promise_.erase(iter);
          }
        }
        if (operation.context) {
          operation.context->SetCommited(
              PromiseContext::CommitResult{nullptr, entry.index()});
        }
        operations.emplace_back(std::move(operation));
        break;
      }
      default: {
        LOG_ERROR("unhandled entry : {}", entry.ShortDebugString());
      }
    }
  }
  return operations;
}

void RaftDriver::Apply(const std::vector<ApplyOperation>& operations) {
  for (const auto& operation : operations) {
    eraft::Error apply_err = nullptr;
    try {
      switch (operation.type) {
        case ApplyOperation::Type::Request:
          apply_(operation.index, operation.request);
          break;
        case ApplyOperation::Type::ConfChange:
          apply_conf_change_(operation.index, operation.conf_state,
                             operation.node_infos);
          break;
      }
    } catch (const std::exception& e) {
      apply_err = eraft::Error(e.what());
    } catch (...) {
      apply_err = eraft::Error("unknown error");
    }
    if (apply_err == nullptr) {
      apply_id_.store(operation.index);
    }
    if (operation.context) {
      if (operation.type == ApplyOperation::Type::ConfChange &&
          operation.raft_advanced.valid()) {
        operation.raft_advanced.wait();
      }
      operation.context->SetApplied(
          PromiseContext::ApplyResult{apply_err, operation.index});
      ReleaseProposalAccounting(operation.context);
    }
    if (apply_err != nullptr) {
      LOG_FATAL("failed to apply committed raft entry at index {}: {}",
                operation.index, apply_err.String());
    }
  }
}

}  // namespace raft
