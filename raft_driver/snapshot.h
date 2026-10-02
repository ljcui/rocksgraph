#pragma once

#include <atomic>
#include <boost/crc.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "proto/graph_replication.pb.h"

namespace raft {

inline constexpr size_t kSnapshotChunkSize = 1024 * 1024;
inline constexpr uint32_t kSnapshotVersion = 1;

struct SnapshotFiles {
  ~SnapshotFiles();
  raftpb::Snapshot snapshot;
  std::filesystem::path directory;
  std::vector<meta::SnapshotFile> files;
  bool keep = false;
};

uint32_t SnapshotChecksum(const char* data, size_t size);
void CheckSnapshotFileName(const std::string& name);
meta::SnapshotReference ParseSnapshotReference(
    const raftpb::Snapshot& snapshot);
void SyncDirectory(const std::filesystem::path& path);
void WriteSnapshotRecord(const std::filesystem::path& path,
                         const std::string& data);
std::string ReadSnapshotRecord(const std::filesystem::path& path);
void RemoveSnapshotRecord(const std::filesystem::path& path);
void DescribeSnapshotFiles(SnapshotFiles& snapshot,
                           const std::function<bool()>& cancelled = {});

// Blocking file/network work runs on the snapshot worker pool, never on a
// Raft shard. Network operations poll cancellation and have an idle timeout.
void SendSnapshotFiles(const std::string& graph, const std::string& ip,
                       uint16_t port, const raftpb::Message& message,
                       const SnapshotFiles& snapshot,
                       const std::function<bool()>& cancelled);

// One instance belongs to one dedicated connection. Consume is called serially
// on the snapshot worker pool; destruction removes incomplete transfers.
class SnapshotReceiver {
 public:
  void Consume(const meta::SnapshotFrame& frame,
               const std::filesystem::path& graph_path);
  std::shared_ptr<SnapshotFiles> Finish();
  const raftpb::Message& message() const { return message_; }

 private:
  void FinishFile();
  std::shared_ptr<SnapshotFiles> snapshot_;
  raftpb::Message message_;
  std::string transfer_id_;
  uint64_t file_count_ = 0;
  uint64_t files_received_ = 0;
  bool finished_ = false;
  uint64_t offset_ = 0;
  boost::crc_32_type file_checksum_;
  bool file_open_ = false;
};

}  // namespace raft
