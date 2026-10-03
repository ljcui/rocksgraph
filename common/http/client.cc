#include "common/http/client.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <charconv>
#include <utility>

#include "common/exception.h"

namespace common::http {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace wire = beast::http;
using tcp = asio::ip::tcp;

Address ParseAddress(std::string address) {
  if (address.starts_with("http://")) address.erase(0, 7);
  if (address.ends_with('/')) address.pop_back();
  RG_CHECK(!address.empty() &&
               address.find_first_of("/@?#\\\r\n\t ") == std::string::npos,
           common::ErrorCode::InvalidParameter,
           "address must be http://host:port");
  Address result;
  result.authority = address;
  if (address.front() == '[') {
    auto end = address.find(']');
    RG_CHECK(end != std::string::npos && end + 1 < address.size() &&
                 address[end + 1] == ':',
             common::ErrorCode::InvalidParameter, "invalid IPv6 address");
    result.host = address.substr(1, end - 1);
    result.port = address.substr(end + 2);
  } else {
    auto colon = address.find(':');
    RG_CHECK(colon != std::string::npos &&
                 address.find(':', colon + 1) == std::string::npos,
             common::ErrorCode::InvalidParameter,
             "address must include an HTTP port");
    result.host = address.substr(0, colon);
    result.port = address.substr(colon + 1);
  }
  unsigned port = 0;
  auto [end, ec] = std::from_chars(
      result.port.data(), result.port.data() + result.port.size(), port);
  RG_CHECK(!result.host.empty() && ec == std::errc{} &&
               end == result.port.data() + result.port.size() && port > 0 &&
               port <= 65535,
           common::ErrorCode::InvalidParameter, "invalid HTTP address or port");
  return result;
}

namespace {
// Run asynchronous Beast operations on a private context so timeout expiry also
// applies to DNS, connection establishment, and streaming large file bodies.
template <typename Start>
void Await(asio::io_context& context, Start start) {
  boost::system::error_code result;
  bool done = false;
  context.restart();
  start([&](boost::system::error_code ec, auto...) {
    result = ec;
    done = true;
  });
  context.run();
  RG_CHECK(done && !result, common::ErrorCode::IOError,
           "HTTP request failed: {}", result.message());
}

class Connection {
 public:
  Connection(const Address& address, const ClientOptions& options)
      : options_(options), stream_(context_) {
    tcp::resolver resolver(context_);
    tcp::resolver::results_type endpoints;
    asio::steady_timer timer(context_);
    timer.expires_after(options_.timeout);
    timer.async_wait([&](auto ec) {
      if (!ec) resolver.cancel();
    });
    Await(context_, [&](auto handler) {
      resolver.async_resolve(address.host, address.port,
                             [&, handler](auto ec, auto results) {
                               timer.cancel();
                               endpoints = std::move(results);
                               handler(ec);
                             });
    });
    stream_.expires_after(options_.timeout);
    Await(context_,
          [&](auto handler) { stream_.async_connect(endpoints, handler); });
  }

  void Write(const Address& address, wire::verb method,
             const std::string& target, const std::string& body,
             const std::string& content_type) {
    wire::request<wire::string_body> request{method, target, 11};
    request.set(wire::field::host, address.authority);
    if (!content_type.empty())
      request.set(wire::field::content_type, content_type);
    request.body() = body;
    request.prepare_payload();
    stream_.expires_after(options_.timeout);
    Await(context_,
          [&](auto handler) { wire::async_write(stream_, request, handler); });
  }

  Client::Response Read() {
    wire::response_parser<wire::string_body> parser;
    parser.body_limit(options_.response_body_limit);
    stream_.expires_after(options_.timeout);
    Await(context_, [&](auto handler) {
      wire::async_read(stream_, buffer_, parser, handler);
    });
    return parser.release();
  }

  void Download(const std::filesystem::path& file, uint64_t expected_size) {
    wire::response_parser<wire::file_body> parser;
    parser.body_limit(expected_size);
    stream_.expires_after(options_.timeout);
    Await(context_, [&](auto handler) {
      wire::async_read_header(stream_, buffer_, parser, handler);
    });
    RG_CHECK(parser.get().result() == wire::status::ok &&
                 parser.content_length() &&
                 *parser.content_length() == expected_size,
             common::ErrorCode::IOError,
             "HTTP download size or status does not match");
    beast::error_code ec;
    parser.get().body().open(file.c_str(), beast::file_mode::write, ec);
    RG_CHECK(!ec, common::ErrorCode::IOError,
             "cannot create downloaded file: {}", ec.message());
    stream_.expires_after(options_.timeout);
    Await(context_, [&](auto handler) {
      wire::async_read(stream_, buffer_, parser, handler);
    });
  }

 private:
  ClientOptions options_;
  asio::io_context context_;
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
};
}  // namespace

Client::Client(Address address, ClientOptions options)
    : address_(std::move(address)), options_(options) {
  RG_CHECK(options_.timeout.count() > 0, common::ErrorCode::InvalidParameter,
           "HTTP timeout must be positive");
}

Client::Response Client::Send(wire::verb method, const std::string& target,
                              const std::string& body,
                              const std::string& content_type) const {
  Connection connection(address_, options_);
  connection.Write(address_, method, target, body, content_type);
  return connection.Read();
}

void Client::Download(const std::string& target,
                      const std::filesystem::path& file,
                      uint64_t expected_size) const {
  Connection connection(address_, options_);
  connection.Write(address_, wire::verb::get, target, {}, {});
  connection.Download(file, expected_size);
}
}  // namespace common::http
