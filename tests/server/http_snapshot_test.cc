#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "common/exception.h"
#include "common/http/server.h"
#include "graphdb/transaction.h"
#include "raft_driver/raft_driver.h"
#include "runtime/query_executor.h"
#include "server/graph_manager.h"
#include "server/http_server.h"

namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
namespace json = boost::json;
using Json = json::value;
namespace fs = std::filesystem;

uint16_t AllocateFreePort() {
  asio::io_context context;
  tcp::acceptor acceptor(context, {tcp::v4(), 0});
  return acceptor.local_endpoint().port();
}

http::response<http::string_body> Request(uint16_t port, http::verb method,
                                          const std::string& target,
                                          const Json& body = json::object{}) {
  asio::io_context context;
  tcp::socket socket(context);
  socket.connect({asio::ip::make_address("127.0.0.1"), port});
  http::request<http::string_body> request{method, target, 11};
  request.set(http::field::host, "127.0.0.1");
  request.body() = json::serialize(body);
  request.prepare_payload();
  http::write(socket, request);
  boost::beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);
  return response;
}

class HttpSnapshotTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("rocksgraph-http-test-" +
             boost::uuids::to_string(boost::uuids::random_generator{}()));
    source_node_ = {.bolt_port = AllocateFreePort(),
                    .raft_port = AllocateFreePort(),
                    .raft_node_id = 1,
                    .http_port = AllocateFreePort()};
    target_node_ = {.bolt_port = AllocateFreePort(),
                    .raft_port = AllocateFreePort(),
                    .raft_node_id = 1,
                    .http_port = AllocateFreePort()};
    // Isolated copies of a single-node group exercise HTTP and restore here;
    // raft_cluster_test covers transfers between actual leaders and followers.
    Open();
  }
  void CreateRaftGraph(server::GraphManager& manager,
                       const server::LocalNodeOptions& local) {
    if (manager.OpenGraph("default")->db_meta().enable_raft()) return;
    manager.DeleteGraph("default");
    meta::RaftNodeInfo node;
    node.set_node_id(local.raft_node_id);
    node.set_ip(local.host);
    node.set_bolt_port(local.bolt_port);
    node.set_raft_poft(local.raft_port);
    node.set_graph("default");
    meta::RaftNodeInfos nodes;
    nodes.mutable_nodes()->emplace(local.raft_node_id, std::move(node));
    manager.CreateGraphWithRaft("default", nodes);
  }
  bool WaitForLeader(server::GraphManager& manager) {
    auto graph = manager.OpenGraph("default");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
      if (graph->GetRaftApplyIndex() > 0 &&
          graph->raft_driver()
                  ->GetRaftStatus()
                  .s.basicStatus_.softState_.raftState_ == eraft::StateLeader)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
  void Open() {
    server::GraphManagerOptions options;
    options.assistant_thread_num = 1;
    options.block_cache_size = 8 * 1024 * 1024;
    options.raft_log_block_cache_size = 8 * 1024 * 1024;
    source_ = server::GraphManager::Open((root_ / "source").string(), options,
                                         source_node_);
    target_ = server::GraphManager::Open((root_ / "target").string(), options,
                                         target_node_);
    CreateRaftGraph(*source_, source_node_);
    CreateRaftGraph(*target_, target_node_);
    ASSERT_TRUE(WaitForLeader(*source_));
    ASSERT_TRUE(WaitForLeader(*target_));
    ASSERT_TRUE(source_http_.Start(source_.get(), (root_ / "source").string(),
                                   "127.0.0.1", source_node_.http_port));
    ASSERT_TRUE(target_http_.Start(target_.get(), (root_ / "target").string(),
                                   "127.0.0.1", target_node_.http_port));
  }
  void TearDown() override {
    source_http_.Stop();
    target_http_.Stop();
    source_.reset();
    target_.reset();
    fs::remove_all(root_);
  }
  void Query(server::GraphManager& manager, const std::string& cypher) {
    ASSERT_TRUE(WaitForLeader(manager));
    auto graph = manager.OpenGraph("default");
    auto transaction = graph->BeginTransaction();
    (void)runtime::ExecuteQuery(*transaction, cypher);
    transaction->Commit();
  }
  int64_t Count(server::GraphManager& manager) {
    auto graph = manager.OpenGraph("default");
    auto transaction = graph->BeginTransaction();
    auto result =
        runtime::ExecuteQuery(*transaction, "MATCH (n) RETURN count(n)");
    transaction->Commit();
    return result.rows.at(0).at(0).AsInteger();
  }
  void TriggerSnapshot(uint16_t remote, uint64_t sender = 1) {
    auto graph = target_->OpenGraph("default");
    auto status = graph->raft_driver()->GetRaftStatus();
    raftpb::Message message;
    message.set_type(raftpb::MsgSnap);
    message.set_from(sender);
    message.set_to(target_node_.raft_node_id);
    message.set_term(status.s.basicStatus_.hardState_.term());
    message.set_context(json::serialize(json::object{
        {"address", "http://127.0.0.1:" + std::to_string(remote)}}));
    target_http_.HandleSnapshotTrigger("default", message);
  }
  fs::path root_;
  server::LocalNodeOptions source_node_, target_node_;
  std::unique_ptr<server::GraphManager> source_, target_;
  server::HttpServer source_http_, target_http_;
};

class SnapshotRemote {
 public:
  explicit SnapshotRemote(uint16_t backend = 0, bool bad_path = false)
      : backend_(backend),
        bad_path_(bad_path),
        http_({.host = "127.0.0.1", .port = 0}, [this](const auto& request) {
          if (request.target() == "/snapshot-reports") {
            std::lock_guard lock(mutex_);
            report_ = json::parse(request.body());
            return common::http::Reply{.body = "{}"};
          }
          if (request.method() == http::verb::delete_) deleted.store(true);
          if (backend_ != 0) {
            auto response = Request(
                backend_, request.method(), std::string(request.target()),
                request.body().empty() ? Json(json::object{})
                                       : json::parse(request.body()));
            return common::http::Reply{.status = response.result(),
                                       .body = response.body()};
          }
          if (request.method() == http::verb::post) {
            return common::http::Reply{
                .body = json::serialize(Json{
                    {"snapshot_id", "00000000-0000-0000-0000-000000000001"},
                    {"version", 1},
                    {"graph", "default"},
                    {"index", 1},
                    {"term", 1},
                    {"files",
                     json::array({{{"path", bad_path_ ? "data/../../escape"
                                                      : "data/CURRENT"},
                                   {"size", 6},
                                   {"crc32", 0}}})}})};
          }
          return common::http::Reply{
              .body =
                  request.method() == http::verb::delete_ ? "{}" : "broken"};
        }) {
    http_.Start();
  }
  uint16_t Port() const { return http_.Port(); }
  Json WaitReport() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
      {
        std::lock_guard lock(mutex_);
        if (!report_.is_null()) return report_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    ADD_FAILURE() << "snapshot completion was not reported";
    return json::object{{"success", false}};
  }
  std::atomic<bool> deleted{false};

 private:
  uint16_t backend_;
  bool bad_path_;
  std::mutex mutex_;
  Json report_;
  common::http::Server http_;
};

TEST_F(HttpSnapshotTest, RejectsZeroHttpPort) {
  server::HttpServer http;
  EXPECT_FALSE(
      http.Start(source_.get(), (root_ / "invalid").string(), "127.0.0.1", 0));
  EXPECT_FALSE(http.Started());
  EXPECT_EQ(http.Port(), 0);
  EXPECT_FALSE(fs::exists(root_ / "invalid"));
}

TEST_F(HttpSnapshotTest, SnapshotRemainsStableAndCanBeDeleted) {
  Query(*source_, "CREATE (:Item {id: 1}), (:Item {id: 2})");
  auto response = Request(source_http_.Port(), http::verb::post, "/snapshots",
                          {{"graph", "default"}});
  ASSERT_EQ(response.result(), http::status::ok) << response.body();
  auto manifest = json::parse(response.body());
  EXPECT_FALSE(manifest.as_object().contains("enable_raft"));
  EXPECT_GT(json::value_to<uint64_t>(manifest.at("index")), 0);
  EXPECT_GT(json::value_to<uint64_t>(manifest.at("term")), 0);
  auto id = json::value_to<std::string>(manifest.at("snapshot_id"));
  Query(*source_, "CREATE (:Item {id: 3})");
  auto directory = root_ / "saved";
  fs::create_directories(directory / "data");
  size_t number = 0;
  for (const auto& file : manifest.at("files").as_array()) {
    auto download =
        Request(source_http_.Port(), http::verb::get,
                "/snapshots/" + id + "/files/" + std::to_string(number++));
    ASSERT_EQ(download.result(), http::status::ok);
    ASSERT_EQ(download.body().size(), json::value_to<size_t>(file.at("size")));
    std::ofstream output(
        directory / json::value_to<std::string>(file.at("path")),
        std::ios::binary);
    output << download.body();
  }
  target_->ReplaceGraphFromSnapshot(
      "default", directory.string(),
      {.graph = "default",
       .index = json::value_to<uint64_t>(manifest.at("index")),
       .term = json::value_to<uint64_t>(manifest.at("term"))});
  {
    auto graph = target_->OpenGraph("default");
    const auto index = json::value_to<uint64_t>(manifest.at("index"));
    EXPECT_EQ(graph->GetRaftApplyIndex(), index);
    auto status = graph->raft_driver()->GetRaftStatus();
    EXPECT_EQ(status.first_log, index);
    EXPECT_EQ(status.s.basicStatus_.id_, target_node_.raft_node_id);
    auto nodes = graph->GetRaftNodeInfos();
    ASSERT_TRUE(nodes);
    EXPECT_EQ(nodes->nodes().at(source_node_.raft_node_id).raft_poft(),
              source_node_.raft_port);
  }
  EXPECT_EQ(Count(*target_), 2);
  EXPECT_EQ(Count(*source_), 3);
  EXPECT_EQ(
      Request(source_http_.Port(), http::verb::delete_, "/snapshots/" + id)
          .result(),
      http::status::ok);
  EXPECT_EQ(Request(source_http_.Port(), http::verb::get, "/snapshots/" + id)
                .result(),
            http::status::not_found);
  EXPECT_TRUE(fs::is_empty(root_ / "source" / ".http-snapshots"));
}

TEST_F(HttpSnapshotTest,
       AsyncUpdateReplacesOnlySelectedGraphAndSurvivesRestart) {
  Query(*source_,
        "CREATE (a:Item {id: 1}), (b:Item {id: 2}), (a)-[:LINK {weight: "
        "9}]->(b)");
  auto source_graph = source_->OpenGraph("default");
  source_graph->AddVertexPropertyIndex("by_id", false, "Item", {"id"});
  source_graph->AddVertexFullTextIndex("item_text", {"Item"}, {"text"});
  source_graph.reset();
  Query(*target_, "CREATE (:Old), (:Old), (:Old)");
  target_->CreateGraph("other");
  auto old_identity = target_->OpenGraph("default")->PlanCacheIdentity();
  SnapshotRemote remote(source_http_.Port());
  // The sender may be a leader added while this follower was offline.
  TriggerSnapshot(remote.Port(), 2);
  auto report = remote.WaitReport();
  ASSERT_TRUE(report.at("success").as_bool()) << json::serialize(report);
  EXPECT_EQ(report.at("graph"), "default");
  EXPECT_EQ(report.at("node_id"), target_node_.raft_node_id);
  EXPECT_TRUE(remote.deleted.load());
  EXPECT_EQ(Count(*target_), 2);
  EXPECT_NE(target_->OpenGraph("default")->PlanCacheIdentity(), old_identity);
  EXPECT_NO_THROW(target_->OpenGraph("other"));
  EXPECT_TRUE(fs::is_empty(root_ / "source" / ".http-snapshots"));
  {
    auto graph = target_->OpenGraph("default");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!graph->meta_info().GetReadyVertexPropertyIndex("by_id") ||
            !graph->meta_info().GetReadyVertexFullTextIndex("item_text")) &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_NE(graph->meta_info().GetReadyVertexPropertyIndex("by_id"), nullptr);
    auto fulltext = graph->meta_info().GetReadyVertexFullTextIndex("item_text");
    ASSERT_NE(fulltext, nullptr);
    EXPECT_TRUE(
        fulltext->meta().path().starts_with((root_ / "target").string()));
    auto transaction = graph->BeginTransaction();
    auto edges = runtime::ExecuteQuery(*transaction,
                                       "MATCH ()-[r:LINK]->() RETURN r.weight");
    transaction->Commit();
    ASSERT_EQ(edges.rows.size(), 1);
    EXPECT_EQ(edges.rows[0][0].AsInteger(), 9);
  }
  source_http_.Stop();
  target_http_.Stop();
  source_.reset();
  target_.reset();
  Open();
  EXPECT_EQ(Count(*target_), 2);
  Query(*target_, "CREATE (:Item {id: 3})");
  EXPECT_EQ(Count(*target_), 3);
}

TEST_F(HttpSnapshotTest, ActiveGraphIsPreservedAndFailureIsReported) {
  Query(*source_, "CREATE (:Item), (:Item)");
  Query(*target_, "CREATE (:Old)");
  auto held = target_->OpenGraph("default");
  SnapshotRemote remote(source_http_.Port());
  TriggerSnapshot(remote.Port());
  TriggerSnapshot(remote.Port());
  EXPECT_FALSE(remote.WaitReport().at("success").as_bool());
  EXPECT_TRUE(remote.deleted.load());
  EXPECT_EQ(Count(*target_), 1);
  EXPECT_TRUE(fs::is_empty(root_ / "source" / ".http-snapshots"));
  EXPECT_TRUE(fs::is_empty(root_ / "target" / ".http-snapshots"));
}

TEST_F(HttpSnapshotTest, RejectsInvalidParametersAndRemovesManualUpdates) {
  EXPECT_EQ(Request(target_http_.Port(), http::verb::post, "/snapshot-updates",
                    {{"graph", "default"}, {"address", "127.0.0.1:1234"}})
                .result(),
            http::status::not_found);
  EXPECT_EQ(
      Request(target_http_.Port(), http::verb::get, "/snapshot-updates/missing")
          .result(),
      http::status::not_found);
  EXPECT_EQ(Request(source_http_.Port(), http::verb::post, "/snapshots",
                    {{"graph", "missing"}})
                .result(),
            http::status::not_found);
  EXPECT_EQ(Request(source_http_.Port(), http::verb::post, "/snapshot-reports",
                    {{"graph", "default"}, {"node_id", -1}, {"success", true}})
                .result(),
            http::status::bad_request);
  auto response =
      Request(source_http_.Port(), http::verb::post, "/snapshot-reports",
              {{"graph", "default"}, {"node_id", 1}, {"success", true}});
  ASSERT_EQ(response.result(), http::status::ok);
  EXPECT_TRUE(json::parse(response.body()).as_object().empty());
}

TEST_F(HttpSnapshotTest, RejectsNonRaftGraphs) {
  source_->CreateGraph("plain");
  target_->CreateGraph("plain");
  auto snapshot = Request(source_http_.Port(), http::verb::post, "/snapshots",
                          {{"graph", "plain"}});
  EXPECT_EQ(snapshot.result(), http::status::bad_request);
  EXPECT_EQ(json::parse(snapshot.body()).at("error"), "InvalidParameter");
  try {
    target_->ReplaceGraphFromSnapshot(
        "plain", (root_ / "unused").string(),
        {.graph = "plain", .index = 1, .term = 1});
    FAIL() << "a non-raft graph must not accept a raft snapshot";
  } catch (const common::Exception& e) {
    EXPECT_EQ(e.code(), common::ErrorCode::InvalidParameter);
  }
  EXPECT_FALSE(target_->OpenGraph("plain")->db_meta().enable_raft());
  EXPECT_TRUE(fs::is_empty(root_ / "source" / ".http-snapshots"));
  EXPECT_TRUE(fs::is_empty(root_ / "target" / ".http-snapshots"));
}

TEST_F(HttpSnapshotTest, OlderSnapshotReplacesLocalData) {
  Query(*target_, "CREATE (:Item {id: 1})");
  auto directory = root_ / "snapshot";
  auto snapshot = target_->CreateSnapshot("default", directory.string());
  Query(*target_, "CREATE (:Item {id: 2})");
  ASSERT_LT(snapshot.index, target_->OpenGraph("default")->GetRaftApplyIndex());
  ASSERT_EQ(Count(*target_), 2);
  ASSERT_NO_THROW(target_->ReplaceGraphFromSnapshot(
      "default", directory.string(), snapshot));
  EXPECT_EQ(target_->OpenGraph("default")->GetRaftApplyIndex(), snapshot.index);
  EXPECT_EQ(Count(*target_), 1);
  Query(*target_, "CREATE (:Item {id: 3})");
  EXPECT_EQ(Count(*target_), 2);
}

TEST_F(HttpSnapshotTest,
       CorruptDownloadPreservesGraphAndDeletesRemoteSnapshot) {
  Query(*target_, "CREATE (:Old)");
  SnapshotRemote remote;
  TriggerSnapshot(remote.Port());
  EXPECT_FALSE(remote.WaitReport().at("success").as_bool());
  EXPECT_TRUE(remote.deleted.load());
  EXPECT_EQ(Count(*target_), 1);
  EXPECT_TRUE(fs::is_empty(root_ / "target" / ".http-snapshots"));
}

TEST_F(HttpSnapshotTest, RejectsRemotePathTraversalAndDeletesRemoteSnapshot) {
  SnapshotRemote remote(0, true);
  TriggerSnapshot(remote.Port());
  EXPECT_FALSE(remote.WaitReport().at("success").as_bool());
  EXPECT_TRUE(remote.deleted.load());
  EXPECT_FALSE(fs::exists(root_ / "target" / "escape"));
}

TEST_F(HttpSnapshotTest, RemoteConnectionFailurePreservesGraph) {
  Query(*target_, "CREATE (:Old)");
  asio::io_context context;
  tcp::acceptor unused(context, {tcp::v4(), 0});
  auto port = unused.local_endpoint().port();
  unused.close();
  TriggerSnapshot(port);
  target_http_.Stop();
  EXPECT_EQ(Count(*target_), 1);
}
}  // namespace
