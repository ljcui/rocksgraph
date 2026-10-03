#pragma once

#include <boost/beast/http.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace common {
enum class ErrorCode;
}

namespace common::http {

using Request = boost::beast::http::request<boost::beast::http::string_body>;

struct Reply {
  boost::beast::http::status status = boost::beast::http::status::ok;
  std::string body;
  std::string content_type = "text/plain";
  std::filesystem::path file;
  // Keep the backing file alive until its asynchronous response finishes.
  std::shared_ptr<const void> file_owner;
};

struct ServerOptions {
  std::string host = "0.0.0.0";
  uint32_t port = 0;
  size_t handler_threads = 4;
  uint64_t request_body_limit = 64 * 1024;
  std::chrono::milliseconds timeout = std::chrono::seconds(120);
};

// Handlers run on a worker pool and return text or a streamed file response.
class Server final {
 public:
  using Handler = std::function<Reply(const Request&)>;
  using ErrorHandler =
      std::function<Reply(common::ErrorCode, const std::string&)>;

  // Bind immediately; port zero selects an available port. Start accepts work.
  Server(ServerOptions options, Handler handler,
         ErrorHandler error_handler = {});
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  void Start();
  // Wait for running handlers before releasing their captured state.
  void Stop();
  bool Started() const;
  uint16_t Port() const;

 private:
  class Impl;
  ServerOptions options_;
  Handler handler_;
  ErrorHandler error_handler_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace common::http
