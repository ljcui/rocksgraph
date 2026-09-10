#include "graphdb/assistant_pool.h"

#include <pthread.h>

#include "common/exceptions.h"

namespace graphdb {

AssistantPool::AssistantPool(size_t thread_num)
    : work_(std::make_unique<boost::asio::io_service::work>(service_)) {
  if (thread_num == 0) {
    THROW_CODE(InvalidParameter, "assistant thread num must be greater than 0");
  }
  threads_.reserve(thread_num);
  for (size_t i = 0; i < thread_num; ++i) {
    threads_.emplace_back([this]() {
      pthread_setname_np(pthread_self(), "assistant");
      service_.run();
    });
  }
}

AssistantPool::~AssistantPool() {
  work_.reset();
  service_.stop();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

}  // namespace graphdb
