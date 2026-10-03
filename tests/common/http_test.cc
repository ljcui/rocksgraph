#include <gtest/gtest.h>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>

#include "common/exception.h"
#include "common/http/client.h"
#include "common/http/server.h"

namespace {
namespace wire = boost::beast::http;
namespace fs = std::filesystem;

common::http::Address LocalAddress(const common::http::Server& server) {
  return common::http::ParseAddress("127.0.0.1:" +
                                    std::to_string(server.Port()));
}

TEST(HttpTest, TextHandlersAndClientPreserveBodyStatusAndContentType) {
  common::http::Server server(
      {.host = "127.0.0.1", .handler_threads = 1},
      [](const common::http::Request& request) -> common::http::Reply {
        if (request.target() == "/echo" && request.method() == wire::verb::post)
          return {.status = wire::status::created,
                  .body = request.body(),
                  .content_type = "application/octet-stream"};
        if (request.target() == "/fail") throw std::runtime_error("failed");
        return {.status = wire::status::not_found, .body = "missing"};
      });
  server.Start();
  common::http::Client client(LocalAddress(server));
  std::string body{"a\0b", 3};
  auto response =
      client.Send(wire::verb::post, "/echo", body, "application/octet-stream");
  EXPECT_EQ(response.result(), wire::status::created);
  EXPECT_EQ(response.body(), body);
  EXPECT_EQ(response[wire::field::content_type], "application/octet-stream");
  EXPECT_EQ(client.Send(wire::verb::get, "/missing").result(),
            wire::status::not_found);
  EXPECT_EQ(client.Send(wire::verb::get, "/fail").result(),
            wire::status::internal_server_error);
  server.Stop();
  EXPECT_FALSE(server.Started());
  server.Start();
  common::http::Client restarted(LocalAddress(server));
  EXPECT_EQ(restarted.Send(wire::verb::post, "/echo", body).body(), body);
}

struct OwnedFile {
  fs::path path;
  ~OwnedFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

TEST(HttpTest, DownloadsStreamedFilesAndKeepsTheirOwnerAlive) {
  auto root = fs::temp_directory_path() /
              ("rocksgraph-http-" +
               boost::uuids::to_string(boost::uuids::random_generator{}()));
  fs::create_directory(root);
  std::string payload(256 * 1024, 'x');
  payload[1] = '\0';
  {
    common::http::Server server(
        {.host = "127.0.0.1", .handler_threads = 1},
        [&](const common::http::Request& request) -> common::http::Reply {
          if (request.target() == "/missing") return {.file = root / "missing"};
          auto owner = std::make_shared<OwnedFile>();
          owner->path = root / boost::uuids::to_string(
                                   boost::uuids::random_generator{}());
          std::ofstream output(owner->path, std::ios::binary);
          output.write(payload.data(), payload.size());
          output.close();
          return {.content_type = "application/octet-stream",
                  .file = owner->path,
                  .file_owner = owner};
        },
        [](common::ErrorCode code, const std::string&) -> common::http::Reply {
          return {.status = wire::status::internal_server_error,
                  .body = common::ErrorCodeToString(code),
                  .content_type = "text/x-http-error"};
        });
    server.Start();
    common::http::Client client(LocalAddress(server));
    auto missing = client.Send(wire::verb::get, "/missing");
    EXPECT_EQ(missing.result(), wire::status::internal_server_error);
    EXPECT_EQ(missing.body(), "IOError");
    EXPECT_EQ(missing[wire::field::content_type], "text/x-http-error");
    auto bad_file = root / "mismatch";
    EXPECT_THROW(client.Download("/file", bad_file, payload.size() + 1),
                 common::Exception);
    EXPECT_FALSE(fs::exists(bad_file));
    auto downloaded = root / "downloaded";
    client.Download("/file", downloaded, payload.size());
    std::ifstream input(downloaded, std::ios::binary);
    std::string actual{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(actual, payload);
  }
  EXPECT_EQ(
      std::distance(fs::directory_iterator(root), fs::directory_iterator()), 1);
  fs::remove_all(root);
}

TEST(HttpTest, ClientTimesOutWaitingForResponse) {
  std::promise<void> release;
  auto ready = release.get_future().share();
  common::http::Server server(
      {.host = "127.0.0.1", .handler_threads = 1},
      [ready](const common::http::Request&) -> common::http::Reply {
        ready.wait_for(std::chrono::seconds(2));
        return {.body = "done"};
      });
  server.Start();
  common::http::Client client(LocalAddress(server),
                              {.timeout = std::chrono::milliseconds(50)});
  auto started = std::chrono::steady_clock::now();
  try {
    client.Send(wire::verb::get, "/");
    ADD_FAILURE() << "expected HTTP timeout";
  } catch (const common::Exception& e) {
    EXPECT_EQ(e.code(), common::ErrorCode::IOError);
  }
  release.set_value();
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(2));
}
}  // namespace
