#pragma once
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <unordered_map>

#include "common/io_service_pool.h"
#include "common/logger.h"

using boost::asio::ip::tcp;
namespace raft {

inline void socket_set_options(tcp::socket &socket) {
  socket.set_option(boost::asio::ip::tcp::no_delay(true));
  socket.set_option(boost::asio::socket_base::keep_alive(true));
  socket.set_option(boost::asio::ip::tcp::socket::reuse_address(true));
}

template <typename T, typename F>
class IOService : private boost::asio::noncopyable {
 public:
  ~IOService() { io_service_pool_.Stop(); }

  IOService(boost::asio::io_service &service, int port, int thread_num,
            F handler)
      : handler_(handler),
        acceptor_(service, tcp::endpoint(tcp::v4(), port),
                  /*reuse_addr*/ true),
        io_service_pool_(thread_num),
        interval_(10),
        timer_(service) {
    io_service_pool_.Run();
    invoke_async_accept();
    clean_closed_conn();
  }

 private:
  void invoke_async_accept() {
    conn_.reset(new T(io_service_pool_.GetIOService(), handler_));
    acceptor_.async_accept(
        conn_->socket(), [this](boost::system::error_code ec) {
          if (ec) {
            LOG_WARN("async accept error: {}", ec.message());
          } else {
            LOG_DEBUG("accept new raft connection {}",
                      boost::lexical_cast<std::string>(
                          conn_->socket().remote_endpoint()));
            socket_set_options(conn_->socket());
            conn_->conn_id() = next_conn_id_;
            connections_.emplace(next_conn_id_, conn_);
            next_conn_id_++;
            conn_->Start();
          }

          invoke_async_accept();
        });
  }

  void clean_closed_conn() {
    for (auto it = connections_.cbegin(); it != connections_.cend();) {
      if (it->second->has_closed()) {
        LOG_DEBUG("erase raft connection[id:{},use_count:{}] from pool",
                  it->second->conn_id(), it->second.use_count());
        it = connections_.erase(it);
      } else {
        ++it;
      }
    }
    timer_.expires_from_now(interval_);
    timer_.async_wait(std::bind(&IOService::clean_closed_conn, this));
  }

  std::shared_ptr<T> conn_;
  F handler_;
  std::unordered_map<int64_t, std::shared_ptr<T>> connections_;
  tcp::acceptor acceptor_;
  common::IOServicePool io_service_pool_;
  int next_conn_id_ = 0;
  boost::posix_time::seconds interval_;
  boost::asio::deadline_timer timer_;
};

}  // namespace raft
