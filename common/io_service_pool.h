#pragma once

#include <pthread.h>

#include <boost/asio.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/exception.h"

namespace common {

class IOServicePool : private boost::asio::noncopyable {
 public:
  explicit IOServicePool(std::size_t pool_size) : next_io_service_(0) {
    RG_CHECK(pool_size != 0, ErrorCode::InvalidParameter,
             "io_service_pool size is 0");
    for (std::size_t i = 0; i < pool_size; ++i) {
      auto io_service = std::make_unique<boost::asio::io_service>(1);
      auto work = std::make_unique<boost::asio::io_service::work>(*io_service);
      io_services_.push_back(std::move(io_service));
      works_.push_back(std::move(work));
    }
  }

  ~IOServicePool() { Stop(); }

  void Run() {
    for (std::size_t i = 0; i < io_services_.size(); ++i) {
      boost::asio::io_service& service = *io_services_[i];
      threads_.emplace_back([i, &service]() {
        std::string name = "io-worker-" + std::to_string(i);
        pthread_setname_np(pthread_self(), name.c_str());
        service.run();
      });
    }
  }

  void Stop() {
    if (stopped_) {
      return;
    }
    for (const auto& io_service : io_services_) {
      io_service->stop();
    }
    for (auto& thread : threads_) {
      thread.join();
    }
    stopped_ = true;
  }

  boost::asio::io_service& GetIOService() {
    boost::asio::io_service& io_service = *io_services_[next_io_service_];
    ++next_io_service_;
    if (next_io_service_ == io_services_.size()) {
      next_io_service_ = 0;
    }
    return io_service;
  }

 private:
  std::vector<std::unique_ptr<boost::asio::io_service>> io_services_;
  std::vector<std::unique_ptr<boost::asio::io_service::work>> works_;
  std::size_t next_io_service_;
  std::vector<std::thread> threads_;
  bool stopped_ = false;
};

}  // namespace common
