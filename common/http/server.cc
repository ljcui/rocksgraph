#include "common/http/server.h"

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <thread>
#include <utility>

#include "common/exception.h"

namespace common::http {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace wire = beast::http;
using tcp = asio::ip::tcp;
}  // namespace

class Server::Impl {
 public:
  Impl(const ServerOptions& options, Handler handler,
       ErrorHandler error_handler)
      : options_(options),
        handler_(std::move(handler)),
        error_handler_(std::move(error_handler)),
        acceptor_(context_),
        handlers_(options.handler_threads) {
    RG_CHECK(options_.port <= 65535 && options_.handler_threads > 0 &&
                 options_.timeout.count() > 0 && handler_,
             common::ErrorCode::InvalidParameter,
             "invalid HTTP server options or handler");
    tcp::endpoint endpoint(asio::ip::make_address(options_.host),
                           static_cast<uint16_t>(options_.port));
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
  }

  void Run() {
    if (started_.exchange(true)) return;
    Accept();
    thread_ = std::thread([this] { context_.run(); });
  }

  ~Impl() {
    context_.stop();
    if (thread_.joinable()) thread_.join();
    handlers_.join();
  }

  bool Started() const { return started_.load(); }
  uint16_t Port() const { return port_; }

 private:
  class Session : public std::enable_shared_from_this<Session> {
   public:
    Session(tcp::socket socket, Impl* server)
        : stream_(std::move(socket)), server_(server) {}
    void Read() {
      parser_.body_limit(server_->options_.request_body_limit);
      stream_.expires_after(server_->options_.timeout);
      wire::async_read(
          stream_, buffer_, parser_,
          [self = shared_from_this()](auto ec, auto) {
            if (ec) return;
            auto request = self->parser_.release();
            asio::post(self->server_->handlers_,
                       [self, request = std::move(request)] {
                         auto reply = self->server_->Handle(request);
                         asio::post(self->stream_.get_executor(),
                                    [self, reply = std::move(reply)]() mutable {
                                      self->Write(std::move(reply));
                                    });
                       });
          });
    }

   private:
    void Write(Reply reply) {
      stream_.expires_after(server_->options_.timeout);
      if (!reply.file.empty()) {
        auto response =
            std::make_shared<wire::response<wire::file_body>>(reply.status, 11);
        beast::error_code ec;
        response->body().open(reply.file.c_str(), beast::file_mode::scan, ec);
        if (ec) {
          Write(server_->Error(common::ErrorCode::IOError, ec.message()));
          return;
        }
        response->set(wire::field::content_type, reply.content_type);
        response->content_length(response->body().size());
        wire::async_write(stream_, *response,
                          [self = shared_from_this(), response,
                           file_owner = std::move(reply.file_owner)](
                              auto, auto) { self->Close(); });
      } else {
        auto response = std::make_shared<wire::response<wire::string_body>>(
            reply.status, 11);
        response->set(wire::field::content_type, reply.content_type);
        response->body() = std::move(reply.body);
        response->prepare_payload();
        wire::async_write(stream_, *response,
                          [self = shared_from_this(), response](auto, auto) {
                            self->Close();
                          });
      }
    }
    void Close() {
      beast::error_code ec;
      stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
      stream_.socket().close(ec);
    }
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    wire::request_parser<wire::string_body> parser_;
    Impl* server_;
  };

  void Accept() {
    acceptor_.async_accept([this](auto ec, auto socket) {
      if (!ec) std::make_shared<Session>(std::move(socket), this)->Read();
      if (started_.load()) Accept();
    });
  }

  Reply Handle(const Request& request) {
    try {
      return handler_(request);
    } catch (const common::Exception& e) {
      return Error(e.code(), e.message());
    } catch (const std::exception& e) {
      return Error(common::ErrorCode::InternalError, e.what());
    } catch (...) {
      return Error(common::ErrorCode::InternalError, "HTTP handler failed");
    }
  }

  Reply Error(common::ErrorCode code, const std::string& message) {
    if (error_handler_) {
      try {
        return error_handler_(code, message);
      } catch (...) {
        // A formatter failure must not escape an I/O callback.
      }
    }
    return {.status = wire::status::internal_server_error, .body = message};
  }

  ServerOptions options_;
  Handler handler_;
  ErrorHandler error_handler_;
  asio::io_context context_;
  tcp::acceptor acceptor_;
  asio::thread_pool handlers_;
  std::thread thread_;
  std::atomic<bool> started_{false};
  uint16_t port_ = 0;
};

Server::Server(ServerOptions options, Handler handler,
               ErrorHandler error_handler)
    : options_(std::move(options)),
      handler_(std::move(handler)),
      error_handler_(std::move(error_handler)),
      impl_(std::make_unique<Impl>(options_, handler_, error_handler_)) {}
Server::~Server() = default;
void Server::Start() {
  if (!impl_)
    impl_ = std::make_unique<Impl>(options_, handler_, error_handler_);
  impl_->Run();
}
void Server::Stop() { impl_.reset(); }
bool Server::Started() const { return impl_ && impl_->Started(); }
uint16_t Server::Port() const { return impl_ ? impl_->Port() : 0; }
}  // namespace common::http
