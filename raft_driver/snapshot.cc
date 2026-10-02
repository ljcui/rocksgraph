#include "raft_driver/snapshot.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/crc.hpp>
#include <boost/endian/conversion.hpp>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <utility>

#include "common/exception.h"
#include "common/logger.h"
#include "etcd_raft/confchange/restore.h"
#include "etcd_raft/raftpb/confstate.h"

namespace raft {
namespace {

constexpr std::array<unsigned char, 4> kMagic{0x17, 0xB0, 0x60, 0x60};

void CheckIO(bool ok, const std::string& operation) {
  RG_CHECK(ok, common::ErrorCode::IOError, "snapshot {}: {}", operation,
           std::strerror(errno));
}

bool SafeId(const std::string& id) {
  return !id.empty() && id.size() <= 128 &&
         std::all_of(id.begin(), id.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_';
         });
}

void SyncFile(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  CheckIO(fd >= 0, "open " + path.string());
  int result = ::fsync(fd);
  int saved_errno = errno;
  ::close(fd);
  errno = saved_errno;
  CheckIO(result == 0, "sync " + path.string());
}

uint32_t FileChecksum(const std::filesystem::path& path,
                      const std::function<bool()>& cancelled = {}) {
  std::ifstream stream(path, std::ios::binary);
  RG_CHECK(stream.is_open(), common::ErrorCode::IOError,
           "cannot open snapshot file {}", path.string());
  std::vector<char> buffer(kSnapshotChunkSize);
  boost::crc_32_type crc;
  while (stream) {
    RG_CHECK(!cancelled || !cancelled(), common::ErrorCode::QueryCancelled,
             "snapshot scan cancelled");
    stream.read(buffer.data(), buffer.size());
    crc.process_bytes(buffer.data(), static_cast<size_t>(stream.gcount()));
  }
  RG_CHECK(stream.eof(), common::ErrorCode::IOError,
           "cannot read snapshot file {}", path.string());
  return crc.checksum();
}

void SocketIO(int fd, char* data, size_t size, bool write,
              const std::function<bool()>& cancelled) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (size != 0) {
    RG_CHECK(!cancelled(), common::ErrorCode::QueryCancelled,
             "snapshot transfer cancelled");
    RG_CHECK(std::chrono::steady_clock::now() < deadline,
             common::ErrorCode::IOError, "snapshot transfer timed out");
    pollfd event{fd, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    int result = ::poll(&event, 1, 100);
    if (result < 0 && errno == EINTR) continue;
    CheckIO(result >= 0, "poll socket");
    if (result == 0) continue;
    ssize_t count = write ? ::send(fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT)
                          : ::recv(fd, data, size, MSG_DONTWAIT);
    if (count < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    CheckIO(count > 0, write ? "send" : "receive");
    data += count;
    size -= count;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  }
}

void Exchange(int fd, const std::string& graph, meta::SnapshotFrame frame,
              const std::function<bool()>& cancelled) {
  meta::RaftMessage envelope;
  envelope.set_graph(graph);
  *envelope.mutable_snapshot_frame() = std::move(frame);
  std::string body = envelope.SerializeAsString();
  uint32_t size =
      boost::endian::native_to_big(static_cast<uint32_t>(body.size()));
  SocketIO(fd, reinterpret_cast<char*>(&size), sizeof(size), true, cancelled);
  SocketIO(fd, body.data(), body.size(), true, cancelled);
  SocketIO(fd, reinterpret_cast<char*>(&size), sizeof(size), false, cancelled);
  size = boost::endian::big_to_native(size);
  RG_CHECK(size > 0 && size <= 64 * 1024, common::ErrorCode::InputError,
           "invalid snapshot acknowledgement size");
  body.resize(size);
  SocketIO(fd, body.data(), size, false, cancelled);
  meta::SnapshotFrame response;
  RG_CHECK(
      response.ParseFromString(body) &&
          response.kind() == meta::SnapshotFrame::ACK &&
          response.transfer_id() == envelope.snapshot_frame().transfer_id(),
      common::ErrorCode::InputError, "invalid snapshot acknowledgement");
  RG_CHECK(response.error().empty(), common::ErrorCode::StorageEngineError,
           "snapshot rejected: {}", response.error());
}

}  // namespace

SnapshotFiles::~SnapshotFiles() {
  if (!keep && !directory.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    if (ec)
      LOG_WARN("cannot remove snapshot {}: {}", directory.string(),
               ec.message());
  }
}

uint32_t SnapshotChecksum(const char* data, size_t size) {
  boost::crc_32_type crc;
  crc.process_bytes(data, size);
  return crc.checksum();
}

void CheckSnapshotFileName(const std::string& name) {
  RG_CHECK(!name.empty() && name.size() <= 255 && name != "." && name != ".." &&
               name.find_first_of("/\\\0", 0, 3) == std::string::npos,
           common::ErrorCode::InvalidParameter, "invalid snapshot file name");
}

meta::SnapshotReference ParseSnapshotReference(
    const raftpb::Snapshot& snapshot) {
  meta::SnapshotReference reference;
  RG_CHECK(reference.ParseFromString(snapshot.data()) &&
               reference.version() == kSnapshotVersion &&
               SafeId(reference.id()),
           common::ErrorCode::InputError, "invalid snapshot reference");
  RG_CHECK(snapshot.metadata().index() > 0 && snapshot.metadata().term() > 0,
           common::ErrorCode::InputError, "invalid snapshot position");
  std::unordered_set<uint64_t> members;
  const auto& state = snapshot.metadata().conf_state();
  auto progress_tracker = tracker::MakeProgressTracker(256, 0);
  auto [config, progress, error] = confchange::Restore(
      {progress_tracker, snapshot.metadata().index()}, state);
  RG_CHECK(error == nullptr, common::ErrorCode::InputError,
           "invalid snapshot ConfState: {}", error.String());
  progress_tracker.config_ = std::move(config);
  RG_CHECK(raftpb::Equivalent(state, progress_tracker.ConfState()) == nullptr,
           common::ErrorCode::InputError, "invalid snapshot ConfState");
  for (auto id : state.voters()) members.insert(id);
  for (auto id : state.voters_outgoing()) members.insert(id);
  for (auto id : state.learners()) members.insert(id);
  for (auto id : state.learners_next()) members.insert(id);
  RG_CHECK(
      !members.empty() && members.size() == reference.node_infos().nodes_size(),
      common::ErrorCode::InputError,
      "snapshot member addresses do not match ConfState");
  for (auto id : members) {
    auto iter = reference.node_infos().nodes().find(id);
    RG_CHECK(id > 0 && iter != reference.node_infos().nodes().end() &&
                 iter->second.node_id() == id && !iter->second.ip().empty() &&
                 iter->second.raft_poft() > 0 &&
                 iter->second.raft_poft() <= 65535,
             common::ErrorCode::InputError, "invalid snapshot member {}", id);
  }
  return reference;
}

void SyncDirectory(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CheckIO(fd >= 0, "open directory " + path.string());
  int result = ::fsync(fd);
  int saved_errno = errno;
  ::close(fd);
  errno = saved_errno;
  CheckIO(result == 0, "sync directory " + path.string());
}

void WriteSnapshotRecord(const std::filesystem::path& path,
                         const std::string& data) {
  auto temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(data.data(), data.size());
    stream.flush();
    RG_CHECK(stream.good(), common::ErrorCode::IOError,
             "cannot write snapshot record {}", path.string());
  }
  SyncFile(temporary);
  std::filesystem::rename(temporary, path);
  SyncDirectory(path.parent_path());
}

std::string ReadSnapshotRecord(const std::filesystem::path& path) {
  RG_CHECK(std::filesystem::file_size(path) <= 128 * 1024 * 1024,
           common::ErrorCode::InputError, "snapshot record too large");
  std::ifstream stream(path, std::ios::binary);
  RG_CHECK(stream.is_open(), common::ErrorCode::IOError,
           "cannot read snapshot record {}", path.string());
  std::string data((std::istreambuf_iterator<char>(stream)), {});
  RG_CHECK(!stream.bad(), common::ErrorCode::IOError,
           "cannot read snapshot record");
  return data;
}

void RemoveSnapshotRecord(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  SyncDirectory(path.parent_path());
}

void DescribeSnapshotFiles(SnapshotFiles& snapshot,
                           const std::function<bool()>& cancelled) {
  for (const auto& entry :
       std::filesystem::directory_iterator(snapshot.directory)) {
    RG_CHECK(entry.is_regular_file() && !entry.is_symlink(),
             common::ErrorCode::IOError, "invalid checkpoint file");
    meta::SnapshotFile file;
    file.set_name(entry.path().filename().string());
    CheckSnapshotFileName(file.name());
    file.set_size(entry.file_size());
    file.set_checksum(FileChecksum(entry.path(), cancelled));
    snapshot.files.emplace_back(std::move(file));
  }
  std::sort(snapshot.files.begin(), snapshot.files.end(),
            [](const auto& left, const auto& right) {
              return left.name() < right.name();
            });
}

void SendSnapshotFiles(const std::string& graph, const std::string& ip,
                       uint16_t port, const raftpb::Message& message,
                       const SnapshotFiles& snapshot,
                       const std::function<bool()>& cancelled) {
  boost::asio::io_service service;
  boost::asio::ip::tcp::socket socket(service);
  auto endpoint =
      boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(ip), port);
  socket.open(endpoint.protocol());
  socket.non_blocking(true);
  boost::system::error_code error;
  socket.connect(endpoint, error);
  if (error == boost::asio::error::in_progress ||
      error == boost::asio::error::would_block) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
      RG_CHECK(!cancelled(), common::ErrorCode::QueryCancelled,
               "snapshot transfer cancelled");
      RG_CHECK(std::chrono::steady_clock::now() < deadline,
               common::ErrorCode::IOError, "snapshot connect timed out");
      pollfd event{socket.native_handle(), POLLOUT, 0};
      int result = ::poll(&event, 1, 100);
      if (result < 0 && errno == EINTR) continue;
      CheckIO(result >= 0, "connect poll");
      if (result == 0) continue;
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      CheckIO(::getsockopt(socket.native_handle(), SOL_SOCKET, SO_ERROR,
                           &socket_error, &length) == 0,
              "connect status");
      RG_CHECK(socket_error == 0, common::ErrorCode::IOError,
               "snapshot connect failed: {}", std::strerror(socket_error));
      break;
    }
  } else {
    RG_CHECK(!error, common::ErrorCode::IOError, "snapshot connect failed: {}",
             error.message());
  }
  std::array<unsigned char, 4> magic = kMagic;
  SocketIO(socket.native_handle(), reinterpret_cast<char*>(magic.data()),
           magic.size(), true, cancelled);
  const auto reference = ParseSnapshotReference(message.snapshot());
  static std::atomic<uint64_t> next_transfer{0};
  std::string transfer =
      reference.id() + "-" + std::to_string(next_transfer.fetch_add(1));
  meta::SnapshotFrame frame;
  frame.set_kind(meta::SnapshotFrame::BEGIN);
  frame.set_transfer_id(transfer);
  *frame.mutable_message() = message;
  frame.set_file_count(snapshot.files.size());
  Exchange(socket.native_handle(), graph, frame, cancelled);
  std::vector<char> buffer(kSnapshotChunkSize);
  for (const auto& file : snapshot.files) {
    frame.Clear();
    frame.set_kind(meta::SnapshotFrame::FILE);
    frame.set_transfer_id(transfer);
    *frame.mutable_file() = file;
    Exchange(socket.native_handle(), graph, frame, cancelled);
    std::ifstream stream(snapshot.directory / file.name(), std::ios::binary);
    RG_CHECK(stream.is_open(), common::ErrorCode::IOError,
             "cannot open snapshot file");
    uint64_t offset = 0;
    while (offset < file.size()) {
      auto count = std::min<uint64_t>(buffer.size(), file.size() - offset);
      stream.read(buffer.data(), count);
      RG_CHECK(static_cast<uint64_t>(stream.gcount()) == count,
               common::ErrorCode::IOError, "cannot read snapshot file");
      frame.Clear();
      frame.set_kind(meta::SnapshotFrame::CHUNK);
      frame.set_transfer_id(transfer);
      frame.set_offset(offset);
      frame.set_data(buffer.data(), count);
      frame.set_checksum(SnapshotChecksum(buffer.data(), count));
      Exchange(socket.native_handle(), graph, frame, cancelled);
      offset += count;
    }
  }
  frame.Clear();
  frame.set_kind(meta::SnapshotFrame::FINISH);
  frame.set_transfer_id(transfer);
  Exchange(socket.native_handle(), graph, frame, cancelled);
}

void SnapshotReceiver::FinishFile() {
  if (!file_open_) return;
  const auto& file = snapshot_->files.back();
  RG_CHECK(offset_ == file.size(), common::ErrorCode::InputError,
           "incomplete snapshot file");
  auto path = snapshot_->directory / file.name();
  RG_CHECK(file_checksum_.checksum() == file.checksum(),
           common::ErrorCode::InputError, "snapshot file checksum mismatch");
  SyncFile(path);
  file_open_ = false;
}

void SnapshotReceiver::Consume(const meta::SnapshotFrame& frame,
                               const std::filesystem::path& graph_path) {
  if (frame.kind() == meta::SnapshotFrame::BEGIN) {
    RG_CHECK(!snapshot_ && !finished_ && SafeId(frame.transfer_id()) &&
                 frame.has_message() &&
                 frame.message().type() == raftpb::MsgSnap &&
                 frame.message().from() > 0 && frame.message().to() > 0 &&
                 frame.message().term() >=
                     frame.message().snapshot().metadata().term() &&
                 frame.file_count() > 0 && frame.file_count() <= 1000000,
             common::ErrorCode::InputError, "invalid snapshot begin");
    ParseSnapshotReference(frame.message().snapshot());
    transfer_id_ = frame.transfer_id();
    file_count_ = frame.file_count();
    message_ = frame.message();
    snapshot_ = std::make_shared<SnapshotFiles>();
    snapshot_->snapshot = message_.snapshot();
    snapshot_->directory = graph_path / "snapshots" / "in" / transfer_id_;
    RG_CHECK(!std::filesystem::exists(snapshot_->directory),
             common::ErrorCode::InputError, "snapshot transfer already exists");
    std::filesystem::create_directories(snapshot_->directory);
    return;
  }
  RG_CHECK(snapshot_ && !finished_ && frame.transfer_id() == transfer_id_,
           common::ErrorCode::InputError, "snapshot transfer does not match");
  if (frame.kind() == meta::SnapshotFrame::FILE) {
    FinishFile();
    CheckSnapshotFileName(frame.file().name());
    RG_CHECK(files_received_ < file_count_, common::ErrorCode::InputError,
             "too many snapshot files");
    auto path = snapshot_->directory / frame.file().name();
    RG_CHECK(!std::filesystem::exists(path), common::ErrorCode::InputError,
             "duplicate snapshot file");
    std::ofstream stream(path, std::ios::binary);
    RG_CHECK(stream.good(), common::ErrorCode::IOError,
             "cannot create snapshot file");
    snapshot_->files.clear();
    snapshot_->files.push_back(frame.file());
    ++files_received_;
    offset_ = 0;
    file_checksum_.reset();
    file_open_ = true;
    return;
  }
  if (frame.kind() == meta::SnapshotFrame::CHUNK) {
    RG_CHECK(
        file_open_ && frame.offset() == offset_ && !frame.data().empty() &&
            frame.data().size() <= kSnapshotChunkSize &&
            frame.data().size() <= snapshot_->files.back().size() - offset_ &&
            SnapshotChecksum(frame.data().data(), frame.data().size()) ==
                frame.checksum(),
        common::ErrorCode::InputError, "invalid snapshot chunk");
    std::ofstream stream(snapshot_->directory / snapshot_->files.back().name(),
                         std::ios::binary | std::ios::app);
    stream.write(frame.data().data(), frame.data().size());
    stream.flush();
    RG_CHECK(stream.good(), common::ErrorCode::IOError,
             "cannot write snapshot chunk");
    file_checksum_.process_bytes(frame.data().data(), frame.data().size());
    offset_ += frame.data().size();
    return;
  }
  RG_CHECK(frame.kind() == meta::SnapshotFrame::FINISH,
           common::ErrorCode::InputError, "unexpected snapshot frame");
  FinishFile();
  RG_CHECK(files_received_ == file_count_, common::ErrorCode::InputError,
           "incomplete snapshot file list");
  SyncDirectory(snapshot_->directory);
  SyncDirectory(snapshot_->directory.parent_path());
  SyncDirectory(snapshot_->directory.parent_path().parent_path());
  SyncDirectory(graph_path);
  finished_ = true;
}

std::shared_ptr<SnapshotFiles> SnapshotReceiver::Finish() {
  RG_CHECK(finished_ && snapshot_, common::ErrorCode::InternalError,
           "snapshot transfer is incomplete");
  return std::exchange(snapshot_, {});
}

}  // namespace raft
