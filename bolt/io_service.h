#pragma once
#include <cstddef>
#include <unordered_map>

#include "boost/asio.hpp"
#include "boost/lexical_cast.hpp"
#include "common/io_service_pool.h"
#include "common/logger.h"

namespace bolt {

using boost::asio::ip::tcp;

template <typename T, typename F>
class IOService : private boost::asio::noncopyable {
 public:
  ~IOService() {
    boost::system::error_code ec;
    timer_.cancel(ec);
    acceptor_.close(ec);
    CloseAllConnections();
    io_service_pool_.Stop();
  }
  IOService(boost::asio::io_service& service, uint32_t port,
            uint32_t thread_num, size_t max_connections, F handler,
            size_t max_message_size = kDefaultMaxBoltMessageSize)
      : handler_(handler),
        acceptor_(service, tcp::endpoint(tcp::v4(), port),
                  /*reuse_addr*/ true),
        io_service_pool_(thread_num),
        max_connections_(max_connections),
        max_message_size_(max_message_size),
        interval_(10),
        timer_(service) {
    io_service_pool_.Run();
    invoke_async_accept();
    clean_closed_conn();
  }

 private:
  void CloseAllConnections() {
    if (conn_) {
      conn_->Close();
    }
    for (auto& pair : connections_) {
      pair.second->Close();
    }
  }

  void PruneClosedConnections() {
    for (auto it = connections_.cbegin(); it != connections_.cend();) {
      if (it->second->has_closed()) {
        LOG_DEBUG("erase connection[id:{},use_count:{}] from pool",
                  it->second->conn_id(), it->second.use_count());
        it = connections_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void invoke_async_accept() {
    conn_.reset(new T(io_service_pool_.GetIOService(), handler_));
    acceptor_.async_accept(conn_->socket(), [this](
                                                boost::system::error_code ec) {
      if (ec) {
        LOG_WARN("accept error: {}", ec.message());
      } else {
        PruneClosedConnections();
        LOG_DEBUG("accept new bolt connection {}",
                  boost::lexical_cast<std::string>(
                      conn_->socket().remote_endpoint()));
        if (max_connections_ != 0 && connections_.size() >= max_connections_) {
          LOG_WARN("reject bolt connection: connection limit {} reached",
                   max_connections_);
          conn_->Close();
          invoke_async_accept();
          return;
        }
        socket_set_options(conn_->socket());
        conn_->set_max_message_size(max_message_size_);
        conn_->conn_id() = next_conn_id_;
        connections_.emplace(next_conn_id_, conn_);
        next_conn_id_++;
        conn_->Start();
      }

      invoke_async_accept();
    });
  }
  void clean_closed_conn() {
    PruneClosedConnections();
    timer_.expires_from_now(interval_);
    timer_.async_wait([this](const boost::system::error_code& ec) {
      if (ec) {
        return;
      }
      clean_closed_conn();
    });
  }
  std::shared_ptr<T> conn_;
  F handler_;
  std::unordered_map<int64_t, std::shared_ptr<T>> connections_;
  tcp::acceptor acceptor_;
  common::IOServicePool io_service_pool_;
  int next_conn_id_ = 0;
  size_t max_connections_;
  size_t max_message_size_;
  boost::posix_time::seconds interval_;
  boost::asio::deadline_timer timer_;
};

}  // namespace bolt
