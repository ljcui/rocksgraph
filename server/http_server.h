#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace raftpb {
class Message;
}

namespace server {
class GraphManager;

// Snapshot endpoints and update tasks, built on the reusable
// common::http::Server. POST /snapshots {"graph":"name"}: synchronous
// checkpoint and file manifest. GET /snapshots/<id>/files/<number>: stream a
// manifest file. DELETE /snapshots/<id>: release a checkpoint.
// POST /snapshot-reports reports completion to the sending Raft leader.
// Updates are triggered exclusively by received Raft MsgSnap messages.
class HttpServer final {
 public:
  HttpServer();
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  bool Start(GraphManager* manager, const std::string& data_path,
             const std::string& host, uint32_t port);
  void Stop();
  bool Started() const;
  uint16_t Port() const;
  void HandleSnapshotTrigger(const std::string& graph,
                             const raftpb::Message& message);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace server
