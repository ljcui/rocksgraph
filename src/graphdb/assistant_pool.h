#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <thread>
#include <vector>

#include "common/type_traits.h"

namespace graphdb {

class AssistantPool {
 public:
  explicit AssistantPool(size_t thread_num);
  ~AssistantPool();

  DISABLE_COPY(AssistantPool);
  DISABLE_MOVE(AssistantPool);

  boost::asio::io_service& Service() { return service_; }
  boost::asio::io_service::strand NewStrand() {
    return boost::asio::io_service::strand(service_);
  }

 private:
  boost::asio::io_service service_;
  std::unique_ptr<boost::asio::io_service::work> work_;
  std::vector<std::thread> threads_;
};

}  // namespace graphdb
