#pragma once

#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace common::http {

struct Address {
  std::string host;
  std::string port;
  std::string authority;
};

// Accept http://host:port, host:port, and bracketed IPv6 authorities.
Address ParseAddress(std::string address);

struct ClientOptions {
  std::chrono::milliseconds timeout = std::chrono::seconds(120);
  uint64_t response_body_limit = 4 * 1024 * 1024;
};

class Client final {
 public:
  using Response =
      boost::beast::http::response<boost::beast::http::string_body>;

  explicit Client(Address address, ClientOptions options = {});
  // Network failures throw IOError; HTTP status interpretation belongs to the
  // caller. Each call owns its connection and applies a timeout to every step.
  Response Send(boost::beast::http::verb method, const std::string& target,
                const std::string& body = {},
                const std::string& content_type = {}) const;
  // Stream to disk, requiring HTTP 200 and the specified Content-Length.
  void Download(const std::string& target, const std::filesystem::path& file,
                uint64_t expected_size) const;

 private:
  Address address_;
  ClientOptions options_;
};

}  // namespace common::http
