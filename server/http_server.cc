#include "server/http_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/crc.hpp>
#include <boost/json.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "common/exception.h"
#include "common/http/client.h"
#include "common/http/server.h"
#include "common/logger.h"
#include "server/graph_manager.h"

namespace server {
namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace json = boost::json;
namespace fs = std::filesystem;
using Json = json::value;
constexpr size_t kMaxSnapshots = 32;

Json ParseJson(const std::string& text, common::ErrorCode code) {
  boost::system::error_code ec;
  auto value = json::parse(text, ec);
  RG_CHECK(!ec, code, "invalid JSON: {}", ec.message());
  return value;
}

bool IsUnsignedInteger(const Json& value) {
  return value.is_uint64() || (value.is_int64() && value.as_int64() >= 0);
}

std::string NewId() {
  return boost::uuids::to_string(boost::uuids::random_generator{}());
}

uint32_t FileChecksum(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  RG_CHECK(input.is_open(), common::ErrorCode::IOError,
           "cannot open snapshot file");
  boost::crc_32_type crc;
  std::array<char, 65536> buffer;
  while (input.read(buffer.data(), buffer.size()) || input.gcount())
    crc.process_bytes(buffer.data(), input.gcount());
  RG_CHECK(input.eof(), common::ErrorCode::IOError,
           "cannot read snapshot file");
  return crc.checksum();
}

Json Exchange(const common::http::Client& client, http::verb method,
              const std::string& target, const Json& body = json::object{}) {
  auto response =
      client.Send(method, target, json::serialize(body), "application/json");
  RG_CHECK(response.result_int() >= 200 && response.result_int() < 300,
           common::ErrorCode::IOError, "remote HTTP {}: {}",
           response.result_int(), response.body());
  return ParseJson(response.body(), common::ErrorCode::IOError);
}

std::string StringParameter(const Json& body, const char* name) {
  RG_CHECK(body.is_object() && body.as_object().contains(name) &&
               body.at(name).is_string() && !body.at(name).as_string().empty(),
           common::ErrorCode::InvalidParameter, "missing string parameter [{}]",
           name);
  return json::value_to<std::string>(body.at(name));
}

uint64_t UnsignedParameter(const Json& body, const char* name) {
  RG_CHECK(body.is_object() && body.as_object().contains(name) &&
               IsUnsignedInteger(body.at(name)) &&
               json::value_to<uint64_t>(body.at(name)) > 0,
           common::ErrorCode::InvalidParameter,
           "missing positive integer parameter [{}]", name);
  return json::value_to<uint64_t>(body.at(name));
}

struct Snapshot {
  fs::path directory;
  Json manifest;
  std::chrono::steady_clock::time_point created =
      std::chrono::steady_clock::now();
  ~Snapshot() {
    std::error_code ec;
    fs::remove_all(directory, ec);
  }
};

struct Reply {
  http::status status = http::status::ok;
  Json body = json::object{};
  fs::path file;
  std::shared_ptr<Snapshot> snapshot;
};

http::status ErrorStatus(common::ErrorCode code) {
  if (code == common::ErrorCode::InvalidParameter)
    return http::status::bad_request;
  if (code == common::ErrorCode::NoSuchGraph) return http::status::not_found;
  if (code == common::ErrorCode::GraphBusy) return http::status::conflict;
  return http::status::internal_server_error;
}
}  // namespace

class HttpServer::Impl {
 public:
  Impl(GraphManager* manager, fs::path root, const std::string& host,
       uint32_t port)
      : manager_(manager),
        root_(std::move(root)),
        http_(
            {.host = host, .port = port},
            [this](const auto& request) {
              auto reply = Handle(request);
              return common::http::Reply{
                  .status = reply.status,
                  .body = json::serialize(reply.body),
                  .content_type = reply.file.empty()
                                      ? "application/json"
                                      : "application/octet-stream",
                  .file = std::move(reply.file),
                  .file_owner = std::move(reply.snapshot)};
            },
            [](common::ErrorCode code, const std::string& message) {
              return common::http::Reply{
                  .status = ErrorStatus(code),
                  .body = json::serialize(
                      Json{{"error", common::ErrorCodeToString(code)},
                           {"message", message}}),
                  .content_type = "application/json"};
            }) {
    // This private directory contains only transient HTTP snapshots/downloads.
    fs::remove_all(root_);
    fs::create_directories(root_);
  }

  void Run() {
    started_.store(true);
    http_.Start();
  }

  ~Impl() { Stop(); }

  void Stop() {
    if (!started_.exchange(false)) return;
    // The graph manager stays alive while handlers and imports finish. Queued
    // imports observe started_ and cancel, and every network operation expires.
    http_.Stop();
    updates_.join();
    snapshots_.clear();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  bool Started() const { return started_.load(); }
  uint16_t Port() const { return http_.Port(); }

  void HandleSnapshotTrigger(const std::string& graph,
                             const raftpb::Message& message) {
    RG_CHECK(started_.load(), common::ErrorCode::QueryCancelled,
             "HTTP server is stopping");
    RG_CHECK(message.type() == raftpb::MsgSnap,
             common::ErrorCode::InvalidParameter, "expected snapshot trigger");
    auto context =
        ParseJson(message.context(), common::ErrorCode::InvalidParameter);
    auto address =
        common::http::ParseAddress(StringParameter(context, "address"));
    {
      // Do not retain the graph while the asynchronous import replaces it.
      auto database = manager_->OpenGraph(graph);
      auto* driver = database->raft_driver();
      RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
               "snapshot update requires a raft graph [{}]", graph);
      auto status = driver->GetRaftStatus();
      RG_CHECK(message.to() == status.s.basicStatus_.id_ &&
                   message.from() > 0 && message.term() > 0,
               common::ErrorCode::InvalidParameter,
               "invalid snapshot sender, recipient or term");
      if (message.term() < status.s.basicStatus_.hardState_.term()) return;
      // An offline follower can have missed the membership change that added
      // the current leader. Its old membership must not block recovery.
    }
    {
      std::lock_guard lock(mutex_);
      if (!updating_graphs_.insert(graph).second) return;
    }
    asio::post(updates_, [this, graph, address, node_id = message.to()] {
      DownloadAndInstallSnapshot(graph, address, node_id);
    });
  }

 private:
  Reply Handle(const common::http::Request& request) {
    try {
      RG_CHECK(started_.load(), common::ErrorCode::QueryCancelled,
               "HTTP server is stopping");
      const std::string target(request.target());
      if (target == "/snapshots" && request.method() == http::verb::post) {
        auto body =
            ParseJson(request.body(), common::ErrorCode::InvalidParameter);
        auto graph = StringParameter(body, "graph");
        auto id = NewId();
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->directory = root_ / ("snapshot-" + id);
        {
          std::lock_guard lock(mutex_);
          auto expiry =
              std::chrono::steady_clock::now() - std::chrono::hours(1);
          std::erase_if(snapshots_, [&](const auto& entry) {
            return !entry.second->manifest.is_null() &&
                   entry.second->created < expiry;
          });
          RG_CHECK(snapshots_.size() < kMaxSnapshots,
                   common::ErrorCode::GraphBusy,
                   "too many retained snapshots; delete unused snapshots");
          snapshots_.emplace(id, snapshot);
        }
        try {
          auto info =
              manager_->CreateSnapshot(graph, snapshot->directory.string());
          json::array files;
          for (const auto& entry :
               fs::directory_iterator(snapshot->directory / "data")) {
            RG_CHECK(entry.is_regular_file(), common::ErrorCode::IOError,
                     "invalid checkpoint file");
            files.push_back(
                {{"path", "data/" + entry.path().filename().string()},
                 {"size", entry.file_size()},
                 {"crc32", FileChecksum(entry.path())}});
          }
          Json manifest{{"snapshot_id", id}, {"version", 1},
                        {"graph", graph},    {"index", info.index},
                        {"term", info.term}, {"files", std::move(files)}};
          {
            std::lock_guard lock(mutex_);
            snapshot->manifest = manifest;
          }
          return {.body = std::move(manifest)};
        } catch (...) {
          std::lock_guard lock(mutex_);
          snapshots_.erase(id);
          throw;
        }
      }
      if (target.starts_with("/snapshots/")) {
        auto suffix = target.substr(11);
        auto slash = suffix.find('/');
        auto id = suffix.substr(0, slash);
        std::lock_guard lock(mutex_);
        auto iter = snapshots_.find(id);
        if (iter == snapshots_.end())
          return {
              .status = http::status::not_found,
              .body = {{"error", common::ErrorCodeToString(
                                     common::ErrorCode::SnapshotNotFound)}}};
        auto snapshot = iter->second;
        RG_CHECK(!snapshot->manifest.is_null(), common::ErrorCode::GraphBusy,
                 "snapshot is being created");
        if (slash == std::string::npos &&
            request.method() == http::verb::delete_) {
          snapshots_.erase(iter);
          return {.body = {{"deleted", id}}};
        }
        if (slash == std::string::npos && request.method() == http::verb::get)
          return {.body = snapshot->manifest};
        if (slash != std::string::npos &&
            suffix.substr(slash).starts_with("/files/") &&
            request.method() == http::verb::get) {
          auto number = suffix.substr(slash + 7);
          size_t index = 0;
          auto [end, ec] = std::from_chars(
              number.data(), number.data() + number.size(), index);
          RG_CHECK(ec == std::errc{} && end == number.data() + number.size() &&
                       index < snapshot->manifest.at("files").as_array().size(),
                   common::ErrorCode::InvalidParameter,
                   "invalid snapshot file number");
          return {
              .file = snapshot->directory /
                      json::value_to<std::string>(
                          snapshot->manifest.at("files").at(index).at("path")),
              .snapshot = std::move(snapshot)};
        }
      }
      if (target == "/snapshot-reports" &&
          request.method() == http::verb::post) {
        auto body =
            ParseJson(request.body(), common::ErrorCode::InvalidParameter);
        auto graph = StringParameter(body, "graph");
        auto node_id = UnsignedParameter(body, "node_id");
        RG_CHECK(body.as_object().contains("success") &&
                     body.at("success").is_bool(),
                 common::ErrorCode::InvalidParameter,
                 "missing boolean parameter [success]");
        auto database = manager_->OpenGraph(graph);
        auto* driver = database->raft_driver();
        RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
                 "snapshot report requires a raft graph [{}]", graph);
        driver->ReportSnapshot(node_id, body.at("success").as_bool());
        return {};
      }
      return {.status = http::status::not_found,
              .body = {{"error", "InvalidParameter"},
                       {"message", "unknown HTTP route"}}};
    } catch (const common::Exception& e) {
      return {.status = ErrorStatus(e.code()),
              .body = {{"error", common::ErrorCodeToString(e.code())},
                       {"message", e.message()}}};
    } catch (const std::exception& e) {
      return {.status = http::status::internal_server_error,
              .body = {{"error", "InternalError"}, {"message", e.what()}}};
    }
  }

  void DownloadAndInstallSnapshot(const std::string& graph,
                                  const common::http::Address& address,
                                  uint64_t node_id) {
    const auto id = NewId();
    fs::path directory = root_ / ("download-" + id);
    std::string snapshot_id;
    std::string error, error_code;
    bool installed = false;
    common::http::Client client(address);
    try {
      RG_CHECK(started_.load(), common::ErrorCode::QueryCancelled,
               "server is stopping");
      auto manifest =
          Exchange(client, http::verb::post, "/snapshots", {{"graph", graph}});
      auto remote_id = StringParameter(manifest, "snapshot_id");
      RG_CHECK(remote_id.find_first_not_of("0123456789abcdef-") ==
                       std::string::npos &&
                   remote_id.size() == 36,
               common::ErrorCode::InvalidParameter,
               "invalid remote snapshot ID");
      snapshot_id = std::move(remote_id);
      RG_CHECK(manifest.at("version") == 1 &&
                   StringParameter(manifest, "graph") == graph &&
                   IsUnsignedInteger(manifest.at("index")) &&
                   IsUnsignedInteger(manifest.at("term")) &&
                   json::value_to<uint64_t>(manifest.at("index")) > 0 &&
                   json::value_to<uint64_t>(manifest.at("term")) > 0 &&
                   manifest.at("files").is_array() &&
                   !manifest.at("files").as_array().empty() &&
                   manifest.at("files").as_array().size() <= 4096,
               common::ErrorCode::InvalidParameter,
               "invalid snapshot manifest");
      GraphSnapshot snapshot{
          .graph = graph,
          .index = json::value_to<uint64_t>(manifest.at("index")),
          .term = json::value_to<uint64_t>(manifest.at("term"))};
      fs::create_directories(directory / "data");
      std::unordered_set<std::string> paths;
      size_t index = 0;
      for (const auto& file : manifest.at("files").as_array()) {
        RG_CHECK(started_.load(), common::ErrorCode::QueryCancelled,
                 "server is stopping");
        auto path = json::value_to<std::string>(file.at("path"));
        auto relative = fs::path(path);
        RG_CHECK(
            relative.parent_path() == "data" && relative.filename() != "." &&
                relative.filename() != ".." && !relative.filename().empty() &&
                path.find('\\') == std::string::npos &&
                path.find('\0') == std::string::npos &&
                paths.insert(path).second,
            common::ErrorCode::InvalidParameter, "invalid snapshot file path");
        RG_CHECK(IsUnsignedInteger(file.at("size")) &&
                     IsUnsignedInteger(file.at("crc32")) &&
                     json::value_to<uint64_t>(file.at("crc32")) <=
                         std::numeric_limits<uint32_t>::max(),
                 common::ErrorCode::InvalidParameter,
                 "invalid snapshot file size or checksum");
        auto size = json::value_to<uint64_t>(file.at("size"));
        client.Download(
            "/snapshots/" + snapshot_id + "/files/" + std::to_string(index),
            directory / relative, size);
        RG_CHECK(fs::file_size(directory / relative) == size &&
                     FileChecksum(directory / relative) ==
                         json::value_to<uint32_t>(file.at("crc32")),
                 common::ErrorCode::IOError, "snapshot checksum mismatch [{}]",
                 path);
        ++index;
      }
      RG_CHECK(started_.load(), common::ErrorCode::QueryCancelled,
               "server is stopping");
      manager_->ReplaceGraphFromSnapshot(graph, directory.string(), snapshot);
      installed = true;
    } catch (const common::Exception& e) {
      error = e.message();
      error_code = common::ErrorCodeToString(e.code());
    } catch (const std::exception& e) {
      error = e.what();
      error_code = "InternalError";
    }
    bool cleaned = snapshot_id.empty();
    std::string cleanup_error;
    if (!snapshot_id.empty()) {
      try {
        Exchange(client, http::verb::delete_, "/snapshots/" + snapshot_id);
        cleaned = true;
      } catch (const std::exception& e) {
        cleanup_error = e.what();
      }
    }
    std::error_code ec;
    fs::remove_all(directory, ec);
    // The leader may retry as soon as it processes the report. Release this
    // graph's update slot first so the new trigger can enqueue another import.
    {
      std::lock_guard lock(mutex_);
      updating_graphs_.erase(graph);
    }
    // Installation determines Raft success; cleanup errors must not invalidate
    // a checkpoint already installed on the follower.
    bool reported = false;
    common::http::Client reporter(address,
                                  {.timeout = std::chrono::seconds(5)});
    for (int attempt = 0; attempt < 3 && !reported; ++attempt) {
      try {
        Exchange(
            reporter, http::verb::post, "/snapshot-reports",
            {{"graph", graph}, {"node_id", node_id}, {"success", installed}});
        reported = true;
      } catch (const std::exception& e) {
        LOG_WARN("failed to report snapshot for graph [{}]: {}", graph,
                 e.what());
      }
    }
    if (!error.empty())
      LOG_WARN("snapshot update for graph [{}] failed ({}): {}", graph,
               error_code, error);
    if (!cleaned)
      LOG_WARN("snapshot cleanup for graph [{}] failed: {}", graph,
               cleanup_error);
  }

  GraphManager* manager_;
  fs::path root_;
  common::http::Server http_;
  asio::thread_pool updates_{1};
  std::atomic<bool> started_{false};
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Snapshot>> snapshots_;
  std::unordered_set<std::string> updating_graphs_;
};

HttpServer::HttpServer() = default;
HttpServer::~HttpServer() = default;
bool HttpServer::Start(GraphManager* manager, const std::string& data_path,
                       const std::string& host, uint32_t port) {
  if (Started()) return true;
  try {
    RG_CHECK(port > 0 && port <= 65535, common::ErrorCode::InvalidParameter,
             "HTTP port must be between 1 and 65535");
    impl_ = std::make_unique<Impl>(
        manager, fs::path(data_path) / ".http-snapshots", host, port);
    impl_->Run();
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("failed to start HTTP server: {}", e.what());
    impl_.reset();
    return false;
  }
}
void HttpServer::Stop() { impl_.reset(); }
bool HttpServer::Started() const { return impl_ && impl_->Started(); }
uint16_t HttpServer::Port() const { return impl_ ? impl_->Port() : 0; }
void HttpServer::HandleSnapshotTrigger(const std::string& graph,
                                       const raftpb::Message& message) {
  RG_CHECK(Started(), common::ErrorCode::QueryCancelled,
           "HTTP server is not running");
  impl_->HandleSnapshotTrigger(graph, message);
}

}  // namespace server
