/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

/*
 * written by botu.wzy
 */
#include "bolt/connection.h"

#include <boost/endian/conversion.hpp>
#include <cstring>

#include "bolt/messages.h"
#include "bolt/to_string.h"
#include "common/logger.h"
using namespace boost::asio;
using namespace boost::endian;
namespace beast = boost::beast;
namespace bolt {
namespace {

constexpr int kSupportedBoltMajor = 4;
constexpr int kMinSupportedBoltMinor = 0;
constexpr int kMaxSupportedBoltMinor = 4;

bool GetSupportedBoltMinor(const uint8_t* proposal, int* minor) {
  const int major = proposal[3];
  if (major != kSupportedBoltMajor) {
    return false;
  }

  const int max_minor = proposal[2];
  const int range = proposal[1];
  const int min_minor = range > max_minor ? 0 : max_minor - range;
  if (max_minor < kMinSupportedBoltMinor ||
      min_minor > kMaxSupportedBoltMinor) {
    return false;
  }

  const int selected_minor =
      max_minor < kMaxSupportedBoltMinor ? max_minor : kMaxSupportedBoltMinor;
  if (selected_minor < min_minor) {
    return false;
  }

  *minor = selected_minor;
  return true;
}

void SetSelectedBoltVersion(int minor, uint8_t* selected) {
  selected[0] = 0;
  selected[1] = 0;
  selected[2] = static_cast<uint8_t>(minor);
  selected[3] = static_cast<uint8_t>(kSupportedBoltMajor);
}

}  // namespace

void socket_set_options(tcp::socket& socket) {
  socket.set_option(ip::tcp::no_delay(true));
  socket.set_option(socket_base::keep_alive(true));
  socket.set_option(ip::tcp::socket::reuse_address(true));
}

void BoltConnection::WebSocketReadSomeDone(const boost::system::error_code& ec,
                                           std::size_t bytes_transferred) {
  if (ec) {
    LOG_WARN("WebSocketReadSomeDone error: {}", ec.message());
    Close();
    return;
  }
  ws_total_read_ += bytes_transferred;
  if (ws_total_read_ < ws_buffer_size_) {
    WebSocketReadSome();
  } else {
    ws_cb_(ec);
  }
}

void BoltConnection::WebSocketReadSome() {
  ws().async_read_some(
      buffer(ws_buffer_ + ws_total_read_, ws_buffer_size_ - ws_total_read_),
      std::bind(&BoltConnection::WebSocketReadSomeDone, shared_from_this(),
                std::placeholders::_1, std::placeholders::_2));
}

void BoltConnection::WebSocketAsyncRead(
    const mutable_buffer& buffer,
    const std::function<void(const boost::system::error_code& ec)>& cb) {
  ws_buffer_ = (char*)buffer.data();
  ws_buffer_size_ = buffer.size();
  ws_total_read_ = 0;
  ws_cb_ = cb;
  WebSocketReadSome();
}

void BoltConnection::ReadMagicDone(const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("ReadMagicDone error: {}", ec.message());
    Close();
    return;
  }
  if (std::memcmp(buffer4_, bolt_magic_, sizeof(buffer4_)) == 0) {
    protocol_ = Protocol::Socket;
    // Read bolt versions
    async_read(socket(), buffer(buffer16_),
               std::bind(&BoltConnection::ReadVersionNegotiationDone,
                         shared_from_this(), std::placeholders::_1));
  } else if (std::memcmp(buffer4_, ws_magic_, sizeof(buffer4_)) == 0) {
    protocol_ = Protocol::WebSocket;
    ResetToWebSocket();
    // Accept the websocket handshake
    ws().async_accept(buffer(buffer4_),
                      std::bind(&BoltConnection::WebSocketAcceptDone,
                                shared_from_this(), std::placeholders::_1));
  } else {
    LOG_WARN("Unknown protocol magic");
    Close();
  }
}

void BoltConnection::Start() {
  async_read(socket(), buffer(buffer4_),
             std::bind(&BoltConnection::ReadMagicDone, shared_from_this(),
                       std::placeholders::_1));
}

void BoltConnection::Close() { Connection::Close(); }

void BoltConnection::DoSend() {
  for (size_t i = 0; i < msg_queue_.size(); i++) {
    send_buffers_.emplace_back(boost::asio::buffer(msg_queue_[i]));
    if (send_buffers_.size() >= 5) {
      break;
    }
  }
  auto cb = [this](const boost::system::error_code& ec, std::size_t) {
    if (ec) {
      LOG_WARN("async write error: {}, clear {} pending message", ec.message(),
               msg_queue_.size());
      msg_queue_.clear();
      Close();
      return;
    }
    assert(msg_queue_.size() >= send_buffers_.size());
    msg_queue_.erase(msg_queue_.begin(),
                     msg_queue_.begin() + send_buffers_.size());
    send_buffers_.clear();
    if (!msg_queue_.empty()) {
      DoSend();
    }
  };
  if (protocol_ == Protocol::Socket) {
    async_write(socket(), send_buffers_, std::move(cb));
  } else {
    ws().async_write(send_buffers_, std::move(cb));
  }
}
// async respond
// used in io thread
void BoltConnection::Respond(std::string str) {
  if (has_closed()) {
    LOG_WARN("connection is not available, drop this message");
    return;
  }
  bool need_invoke = msg_queue_.empty();
  msg_queue_.push_back(std::move(str));
  if (need_invoke) {
    DoSend();
  }
}

// async respond
// used in non-io thread, thread safe
void BoltConnection::PostResponse(std::string str) {
  if (has_closed()) {
    LOG_WARN("connection is closed, drop this message");
    return;
  }
  io_service().post(
      [self = shared_from_this(), msg = std::move(str)]() mutable {
        self->Respond(std::move(msg));
      });
}

void BoltConnection::ReadBoltIdentificationDone(
    const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("ReadBoltIdentificationDone error: {}", ec.message());
    Close();
    return;
  }
  if (std::memcmp(buffer4_, bolt_magic_, sizeof(buffer4_)) == 0) {
    // read bolt versions
    WebSocketAsyncRead(buffer(buffer16_),
                       std::bind(&BoltConnection::ReadVersionNegotiationDone,
                                 shared_from_this(), std::placeholders::_1));
  } else {
    LOG_WARN("Bolt connection identification is wrong");
    Close();
  }
}

void BoltConnection::WebSocketAcceptDone(const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("WebSocketAcceptDone error: {}", ec.message());
    Close();
    return;
  }
  WebSocketAsyncRead(buffer(buffer4_),
                     std::bind(&BoltConnection::ReadBoltIdentificationDone,
                               shared_from_this(), std::placeholders::_1));
}

void BoltConnection::ReadVersionNegotiationDone(
    const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("ReadVersionNegotiationDone error: {}", ec.message());
    Close();
    return;
  }
  int selected_minor = -1;
  for (int i = 0; i < 4; i++) {
    int candidate_minor = -1;
    if (GetSupportedBoltMinor(buffer16_ + 4 * i, &candidate_minor) &&
        candidate_minor > selected_minor) {
      selected_minor = candidate_minor;
    }
  }
  if (spdlog::get_level() <= spdlog::level::debug) {
    for (int i = 0; i < 4; i++) {
      LOG_DEBUG("protocol version {} major:{}, minor:{}, range:{}",
                std::to_string(i), (int)buffer16_[i * 4 + 3],
                (int)buffer16_[i * 4 + 2], (int)buffer16_[i * 4 + 1]);
    }
  }
  if (selected_minor < 0) {
    LOG_WARN("No matching bolt version found");
    Close();
    return;
  }
  SetSelectedBoltVersion(selected_minor, buffer4_);
  // write accepted version
  if (protocol_ == Protocol::Socket) {
    async_write(socket(), buffer(buffer4_),  // NOLINT
                std::bind(&BoltConnection::WriteResponseDone,
                          shared_from_this(), std::placeholders::_1));
  } else {
    ws().async_write(buffer(buffer4_),
                     std::bind(&BoltConnection::WriteResponseDone,
                               shared_from_this(), std::placeholders::_1));
  }
}

void BoltConnection::WriteResponseDone(const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("WriteResponseDone error: {}", ec.message());
    Close();
    return;
  }
  // read chunk size
  if (protocol_ == Protocol::Socket) {
    async_read(socket(), buffer(&chunk_size_, sizeof(chunk_size_)),  // NOLINT
               std::bind(&BoltConnection::ReadChunkSizeDone, shared_from_this(),
                         std::placeholders::_1));
  } else {
    WebSocketAsyncRead(buffer(&chunk_size_, sizeof(chunk_size_)),
                       std::bind(&BoltConnection::ReadChunkSizeDone,
                                 shared_from_this(), std::placeholders::_1));
  }
}

void BoltConnection::ReadChunkSizeDone(const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("ReadChunkSizeDone error: {}", ec.message());
    Close();
    return;
  }
  big_to_native_inplace(chunk_size_);
  if (chunk_size_ == 0 && !chunk_.empty()) {
    unpacker_.Reset(
        std::string_view((const char*)chunk_.data(), chunk_.size()));
    unpacker_.Next();
    auto len = unpacker_.Len();
    auto tag = static_cast<BoltMsg>(unpacker_.StructTag());
    std::vector<std::any> fields;
    fields.reserve(len);
    try {
      for (uint32_t i = 0; i < len; i++) {
        unpacker_.Next();
        fields.push_back(bolt::ServerHydrator(unpacker_));
      }
      LOG_DEBUG("msg: {}, fields: {}", ToString(tag), Print(fields));
      handle_(*this, tag, std::move(fields));
    } catch (const std::exception& e) {
      LOG_ERROR("Exception in bolt connection: {}", e.what());
      Close();
      return;
    }
    if (has_closed()) {
      return;
    }
    chunk_.resize(0);
  }
  auto old_size = chunk_.size();
  chunk_.resize(old_size + chunk_size_);
  if (protocol_ == Protocol::Socket) {
    async_read(socket(),
               buffer(chunk_.data() + old_size, chunk_size_),  // NOLINT
               std::bind(&BoltConnection::ReadChunkDone, shared_from_this(),
                         std::placeholders::_1));
  } else {
    WebSocketAsyncRead(buffer(chunk_.data() + old_size, chunk_size_),
                       std::bind(&BoltConnection::ReadChunkDone,
                                 shared_from_this(), std::placeholders::_1));
  }
}

void BoltConnection::ReadChunkDone(const boost::system::error_code& ec) {
  if (ec) {
    LOG_WARN("ReadChunkDone error: {}", ec.message());
    Close();
    return;
  }
  if (protocol_ == Protocol::Socket) {
    async_read(socket(), buffer(&chunk_size_, sizeof(chunk_size_)),
               std::bind(&BoltConnection::ReadChunkSizeDone, shared_from_this(),
                         std::placeholders::_1));
  } else {
    WebSocketAsyncRead(buffer(&chunk_size_, sizeof(chunk_size_)),
                       std::bind(&BoltConnection::ReadChunkSizeDone,
                                 shared_from_this(), std::placeholders::_1));
  }
}

}  // namespace bolt
