#pragma once
#include <boost/asio.hpp>
#include <boost/endian/conversion.hpp>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "common/logger.h"
#include "etcd_raft/raftpb/raft.pb.h"
#include "proto/graph_replication.pb.h"
#include "raft_driver/snapshot.h"

namespace raft {

struct RaftHandlers {
  std::function<void(std::string, raftpb::Message)> message;
  std::function<void(std::string, meta::SnapshotFrame,
                     std::shared_ptr<SnapshotReceiver>,
                     std::function<void(std::string)>)>
      snapshot;
};

class Connection : private boost::asio::noncopyable {
 public:
  virtual ~Connection() = default;

  explicit Connection(boost::asio::io_service& io_service)
      : io_service_(io_service), socket_(io_service_), has_closed_(false) {}
  boost::asio::ip::tcp::socket& socket() { return socket_; }
  virtual void Close() {
    LOG_DEBUG("close conn[id:{}]", conn_id_);
    has_closed_ = true;
    boost::system::error_code error;
    socket_.close(error);
  }
  virtual bool has_closed() { return has_closed_; }
  int64_t& conn_id() { return conn_id_; }
  boost::asio::io_service& io_service() { return io_service_; }
  virtual void Start() = 0;

 private:
  boost::asio::io_service& io_service_;
  boost::asio::ip::tcp::socket socket_;
  int64_t conn_id_ = 0;
  std::atomic<bool> has_closed_;
};

class RaftConnection : public Connection,
                       public std::enable_shared_from_this<RaftConnection> {
 public:
  RaftConnection(boost::asio::io_service& io_service, RaftHandlers handler)
      : Connection(io_service),
        handler_(std::move(handler)),
        timer_(io_service) {}
  void Start() override;
  void Close() override {
    // Worker replies must stop posting before the listener destroys its pool.
    // External callbacks retain this gate, rather than the connection/socket.
    std::lock_guard lock(reply_gate_->mutex);
    if (reply_gate_->closed) return;
    reply_gate_->closed = true;
    boost::system::error_code error;
    timer_.cancel(error);
    Connection::Close();
  }

 private:
  struct ReplyGate {
    std::mutex mutex;
    bool closed = false;
  };
  void read_msg_size();
  void read_msg_size_done(const boost::system::error_code& ec);
  void read_magic();
  void read_magic_done(const boost::system::error_code& ec);
  void read_msg_body();
  void read_msg_body_done(const boost::system::error_code& ec);

  uint32_t msg_size_ = 0;
  std::vector<char> msg_body_;
  RaftHandlers handler_;
  std::shared_ptr<ReplyGate> reply_gate_ = std::make_shared<ReplyGate>();
  std::shared_ptr<SnapshotReceiver> receiver_ =
      std::make_shared<SnapshotReceiver>();
  std::string snapshot_graph_;
  boost::asio::deadline_timer timer_;
  const uint8_t magic_code_[4] = {0x17, 0xB0, 0x60, 0x60};
  uint8_t buffer4_[4] = {0};
};

inline void RaftConnection::Start() { read_magic(); }

inline void RaftConnection::read_msg_size() {
  async_read(
      socket(), boost::asio::buffer(&msg_size_, sizeof(msg_size_)),
      [this, self = shared_from_this()](const boost::system::error_code& ec,
                                        size_t) { read_msg_size_done(ec); });
}

inline void RaftConnection::read_magic() {
  async_read(
      socket(), boost::asio::buffer(buffer4_),
      [this, self = shared_from_this()](const boost::system::error_code& ec,
                                        size_t) { read_magic_done(ec); });
}

inline void RaftConnection::read_magic_done(
    const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("raft connection read_magic_done error {}", ec.message());
    Close();
    return;
  }
  if (memcmp(buffer4_, magic_code_, sizeof(magic_code_)) != 0) {
    LOG_WARN("receive wrong magic code");
    Close();
    return;
  }
  read_msg_size();
}

inline void RaftConnection::read_msg_size_done(
    const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("raft connection read_msg_size_done error {}", ec.message());
    Close();
    return;
  }

  boost::endian::big_to_native_inplace(msg_size_);
  if (!snapshot_graph_.empty() && msg_size_ > kSnapshotChunkSize + 64 * 1024) {
    Close();
    return;
  }
  if (msg_size_ > 1024 * 1024 * 1024) {
    LOG_WARN("receive raft message which is too big, size : {}", msg_size_);
    Close();
    return;
  }
  if (msg_size_ == 0) {
    LOG_WARN("receive raft message with error size, size : {}", msg_size_);
    Close();
    return;
  }
  msg_body_.resize(msg_size_);
  read_msg_body();
}

inline void RaftConnection::read_msg_body() {
  async_read(
      socket(), boost::asio::buffer(msg_body_),
      [this, self = shared_from_this()](const boost::system::error_code& ec,
                                        size_t) { read_msg_body_done(ec); });
}

inline void RaftConnection::read_msg_body_done(
    const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("raft connection read_msg_body_done error {}", ec.message());
    Close();
    return;
  }
  meta::RaftMessage envelope;
  if (!envelope.ParseFromArray(msg_body_.data(),
                               static_cast<int>(msg_body_.size()))) {
    LOG_WARN("failed to parse raft message envelope, close connection");
    Close();
    return;
  }
  if (envelope.graph().empty()) {
    LOG_WARN("receive raft message with empty graph");
    Close();
    return;
  }
  if (envelope.has_snapshot_frame()) {
    if (envelope.has_message() || !handler_.snapshot ||
        (!snapshot_graph_.empty() && snapshot_graph_ != envelope.graph())) {
      Close();
      return;
    }
    snapshot_graph_ = envelope.graph();
    timer_.expires_from_now(boost::posix_time::seconds(30));
    timer_.async_wait(
        [self = shared_from_this()](const boost::system::error_code& error) {
          if (!error) self->Close();
        });
    const auto id = envelope.snapshot_frame().transfer_id();
    handler_.snapshot(
        envelope.graph(), std::move(*envelope.mutable_snapshot_frame()),
        receiver_,
        [weak = weak_from_this(), gate = reply_gate_, id](std::string error) {
          std::lock_guard lock(gate->mutex);
          if (gate->closed) return;
          auto self = weak.lock();
          if (!self) return;
          self->io_service().post([self, id, error = std::move(error)]() {
            if (self->has_closed()) return;
            meta::SnapshotFrame response;
            response.set_kind(meta::SnapshotFrame::ACK);
            response.set_transfer_id(id);
            response.set_error(error.substr(0, 2048));
            auto body = response.SerializeAsString();
            uint32_t size = boost::endian::native_to_big(
                static_cast<uint32_t>(body.size()));
            auto wire = std::make_shared<std::string>(
                reinterpret_cast<char*>(&size), sizeof(size));
            wire->append(body);
            async_write(self->socket(), boost::asio::buffer(*wire),
                        [self, wire, failed = !error.empty()](
                            const boost::system::error_code& ec, size_t) {
                          if (ec || failed)
                            self->Close();
                          else
                            self->read_msg_size();
                        });
          });
        });
    return;
  }
  if (!snapshot_graph_.empty() || !envelope.has_message()) {
    LOG_WARN("receive raft message without raft payload");
    Close();
    return;
  }
  handler_.message(envelope.graph(), std::move(*envelope.mutable_message()));
  read_msg_size();
}

}  // namespace raft
