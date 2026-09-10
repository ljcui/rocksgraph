#pragma once
#include <rocksdb/cache.h>
#include <rocksdb/write_batch.h>

#include <atomic>
#include <boost/asio.hpp>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/type_traits.h"
#include "proto/meta.pb.h"
#include "raft_driver/raft_log_store.h"

namespace raft {
using namespace boost::asio::ip;
class NodeClient : public std::enable_shared_from_this<NodeClient> {
 public:
  NodeClient(boost::asio::io_service& io_service, const std::string& ip,
             int port)
      : io_service_(io_service),
        socket_(io_service_),
        endpoint_(address::from_string(ip), port),
        interval_(1000),
        timer_(io_service_) {}
  void Connect();
  void Send(std::string str);
  void Close();
  std::string ip() { return endpoint_.address().to_string(); }
  int port() { return endpoint_.port(); }
  bool connected() { return connected_; }

 private:
  void do_send();
  void do_read_some();
  void reconnect();
  void send_magic_code();

  boost::asio::io_service& io_service_;
  tcp::socket socket_;
  tcp::endpoint endpoint_;
  std::deque<std::string> msg_queue_;
  std::vector<boost::asio::const_buffer> send_buffers_;
  boost::posix_time::millisec interval_;
  boost::asio::deadline_timer timer_;
  std::atomic<bool> has_closed_{false};
  std::atomic<bool> connected_ = false;
  const uint8_t magic_code[4] = {0x17, 0xB0, 0x60, 0x60};
  uint8_t buffer4_[4] = {0};
};

class RaftTransport;

class TransportClient {
 public:
  ~TransportClient();
  void Send(std::string str);
  bool connected() const;

 private:
  friend class RaftTransport;
  TransportClient(std::shared_ptr<RaftTransport> transport, std::string key,
                  std::shared_ptr<NodeClient> client)
      : transport_(std::move(transport)),
        key_(std::move(key)),
        client_(std::move(client)) {}

  std::shared_ptr<RaftTransport> transport_;
  std::string key_;
  std::shared_ptr<NodeClient> client_;
};

class RaftTransport : public std::enable_shared_from_this<RaftTransport> {
 public:
  explicit RaftTransport(boost::asio::io_service& client_service)
      : client_service_(client_service) {}

  std::shared_ptr<TransportClient> Acquire(const std::string& ip, int port);
  void CloseAll();

 private:
  friend class TransportClient;
  struct ClientEntry {
    std::shared_ptr<NodeClient> client;
    size_t refs = 0;
  };

  static std::string BuildKey(const std::string& ip, int port);
  void Release(const std::string& key);

  boost::asio::io_service& client_service_;
  std::mutex mutex_;
  std::unordered_map<std::string, ClientEntry> clients_;
};

class RaftManager : public std::enable_shared_from_this<RaftManager> {
 public:
  static void Configure(size_t raft_shard_count);
  static std::shared_ptr<RaftManager> Instance();
  ~RaftManager();
  DISABLE_COPY(RaftManager);
  DISABLE_MOVE(RaftManager);

  size_t PickShard(const std::string& graph) const;
  boost::asio::io_service& raft_service(size_t shard_id);
  boost::asio::io_service& timer_service(size_t shard_id);
  boost::asio::io_service& apply_service(size_t shard_id);
  boost::asio::io_service& client_service();
  std::shared_ptr<TransportClient> AcquireClient(const std::string& ip,
                                                 int port);
  void WaitForRaftService(size_t shard_id);
  void WaitForTimerService(size_t shard_id);
  void WaitForApplyService(size_t shard_id);

 private:
  struct ServiceRunner {
    ServiceRunner(std::string thread_name, size_t thread_num);
    ~ServiceRunner();
    void Stop();
    void WaitForIdle();
    bool IsServiceThread();

    std::string thread_name;
    boost::asio::io_service service;
    std::unique_ptr<boost::asio::io_service::work> work;
    std::vector<std::thread> threads;
    std::mutex thread_ids_mutex;
    std::unordered_set<std::thread::id> thread_ids;
    std::atomic<bool> stopped{false};
  };

  struct ServiceShard {
    explicit ServiceShard(size_t shard_id);

    ServiceRunner raft_runner;
    ServiceRunner timer_runner;
    ServiceRunner apply_runner;
  };

  explicit RaftManager(size_t raft_shard_count);
  ServiceShard& Shard(size_t shard_id);
  const ServiceShard& Shard(size_t shard_id) const;

  size_t raft_shard_count_ = 1;
  std::vector<std::unique_ptr<ServiceShard>> shards_;
  ServiceRunner client_runner_;
  std::shared_ptr<RaftTransport> transport_;
};

struct Generator {
  static const int tsLen = 5 * 8;
  static const int cntLen = 8;
  static const int suffixLen = tsLen + cntLen;

  Generator() = default;
  void Reset(uint64_t id, uint64_t time) {
    prefix = id << suffixLen;
    suffix.store(lowbit(time, tsLen) << cntLen);
  }
  uint64_t prefix = 0;
  std::atomic<uint64_t> suffix{0};
  static uint64_t lowbit(uint64_t x, int n) {
    return x & (std::numeric_limits<uint64_t>::max() >> (64 - n));
  }
  uint64_t Next() {
    auto suf = suffix.fetch_add(1) + 1;
    auto id = prefix | lowbit(suf, suffixLen);
    return id;
  }
};

struct PromiseContext {
  struct Result {
    eraft::Error err;
    uint64_t index = 0;
  };
  using CommitResult = Result;
  using ApplyResult = Result;

  uint64_t id = 0;
  uint64_t proposal_bytes = 0;
  bool proposal_accounted = false;
  std::promise<CommitResult> commited;
  std::promise<ApplyResult> applied;
  std::atomic<bool> commited_ready = false;
  std::atomic<bool> applied_ready = false;

  void SetCommited(CommitResult result) {
    if (!commited_ready.exchange(true)) {
      commited.set_value(std::move(result));
    }
  }

  void SetApplied(ApplyResult result) {
    if (!applied_ready.exchange(true)) {
      applied.set_value(std::move(result));
    }
  }

  void SetError(eraft::Error err, uint64_t index = 0) {
    SetCommited(CommitResult{err, index});
    SetApplied(ApplyResult{std::move(err), index});
  }
};

struct RaftStatus {
  eraft::Status s;
  uint64_t first_log = 0;
  uint64_t last_log = 0;
};

struct RaftConfig {
  int64_t tick_interval = 0;
  int64_t election_tick = 0;
  int64_t heartbeat_tick = 0;
  int64_t proposal_timeout = 10000;
  uint64_t max_proposal_bytes = 64 * 1024 * 1024;
  uint64_t max_pending_proposals = 1024;
  uint64_t max_pending_proposal_bytes = 256 * 1024 * 1024;
  bool Check();
};

struct RaftLogStoreConfig {
  std::string path;
  std::shared_ptr<rocksdb::Cache> shared_block_cache;
  uint64_t total_threads = 0;
  uint64_t keep_logs = 0;
  uint64_t gc_interval = 0;
  bool Check();
};

struct LocalNodeConfig {
  std::string graph;
  std::string ip;
  int32_t bolt_port = 0;
  int32_t raft_poft = 0;
  bool Check();
};

class RaftDriver {
 public:
  RaftDriver(
      std::function<void(uint64_t index, const meta::RaftRequest&)> apply,
      uint64_t apply_id, LocalNodeConfig local_node,
      const RaftLogStoreConfig& store_config, const RaftConfig& config);
  RaftDriver(
      std::function<void(uint64_t index, const meta::RaftRequest&)> apply,
      uint64_t apply_id, LocalNodeConfig local_node,
      std::vector<eraft::Peer> init_peers,
      const RaftLogStoreConfig& store_config, const RaftConfig& config);
  eraft::Error Run();
  void Stop();
  void Step(raftpb::Message msg);
  PromiseContext::ApplyResult ProposeWriteBatch(meta::WriteBatchKind kind,
                                                const rocksdb::WriteBatch& wb);
  PromiseContext::ApplyResult ProposeRaftRequestAndWait(
      meta::RaftRequest request);
  std::shared_ptr<PromiseContext> ProposeRaftRequest(meta::RaftRequest request);
  std::shared_ptr<PromiseContext> ProposeConfChange(raftpb::ConfChange& cc);
  meta::RaftNodeInfos GetNodeInfosWithLeader();
  RaftStatus GetRaftStatus();

 private:
  std::shared_ptr<PromiseContext> Propose(uint64_t uuid, raftpb::Message msg,
                                          uint64_t proposal_bytes);
  bool RemovePendingPromise(uint64_t uuid,
                            const std::shared_ptr<PromiseContext>& context);
  void ReleaseProposalAccounting(
      const std::shared_ptr<PromiseContext>& context);
  void ReleaseProposalAccountingLocked(
      const std::shared_ptr<PromiseContext>& context);
  void RejectPendingPromises(const eraft::Error& err);
  void Tick();
  void CheckAndCompactLog();
  void CheckReady();
  void Apply(const std::vector<raftpb::Entry>& entries);

  std::shared_ptr<RaftManager> manager_;
  std::shared_ptr<std::atomic<bool>> callback_alive_;
  std::function<void(uint64_t, const meta::RaftRequest&)> apply_;
  std::atomic<uint64_t> apply_id_;
  LocalNodeConfig local_node_;
  size_t shard_id_ = 0;
  uint64_t node_id_;
  std::vector<eraft::Peer> init_peers_;
  boost::posix_time::millisec tick_interval_;
  boost::asio::deadline_timer tick_timer_;
  boost::posix_time::millisec compact_interval_;
  boost::asio::deadline_timer compact_timer_;
  std::shared_ptr<eraft::RawNode> rn_;
  std::shared_ptr<RaftLogStorage> storage_;
  std::shared_mutex nodes_mutex_;
  meta::RaftNodeInfos node_infos_;
  std::unordered_map<uint64_t, std::shared_ptr<TransportClient>> node_clients_;
  Generator id_generator_;
  std::mutex promise_mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<PromiseContext>>
      pending_promise_;
  uint64_t pending_proposals_ = 0;
  uint64_t pending_proposal_bytes_ = 0;
  std::unordered_set<uint64_t> mark_unreachable_;
  RaftLogStoreConfig store_config_;
  RaftConfig raft_config_;
  std::atomic<bool> stopped_ = false;
};
}  // namespace raft
