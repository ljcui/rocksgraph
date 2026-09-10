#pragma once
#include <string>

namespace bolt {
enum class BoltMsg : uint8_t {
  Reset = 0x0f,
  Run = 0x10,
  // DiscardAll = 0x2f,
  DiscardN = 0x2f,  // Different name >= 4.0
  // PullAll    = 0x3f,
  PullN = 0x3f,  // Different name >= 4.0
  Record = 0x71,
  Success = 0x70,
  Ignored = 0x7e,
  Failure = 0x7f,
  Hello = 0x01,
  Logon = 0x6A,
  Logoff = 0x6B,
  Goodbye = 0x02,
  Begin = 0x11,
  Commit = 0x12,
  Rollback = 0x13,
  Route = 0x66,  // > 4.2
  Telemetry = 0x54,
};

inline std::string ToString(BoltMsg msg) {
  switch (msg) {
    case BoltMsg::Reset:
      return "BoltMsg Reset";
    case BoltMsg::Run:
      return "BoltMsg Run";
    case BoltMsg::DiscardN:
      return "BoltMsg DiscardN";
    case BoltMsg::PullN:
      return "BoltMsg PullN";
    case BoltMsg::Record:
      return "BoltMsg Record";
    case BoltMsg::Success:
      return "BoltMsg Success";
    case BoltMsg::Ignored:
      return "BoltMsg Ignored";
    case BoltMsg::Failure:
      return "BoltMsg Failure";
    case BoltMsg::Hello:
      return "BoltMsg Hello";
    case BoltMsg::Logon:
      return "BoltMsg Logon";
    case BoltMsg::Logoff:
      return "BoltMsg Logoff";
    case BoltMsg::Goodbye:
      return "BoltMsg Goodbye";
    case BoltMsg::Begin:
      return "BoltMsg Begin";
    case BoltMsg::Commit:
      return "BoltMsg Commit";
    case BoltMsg::Rollback:
      return "BoltMsg Rollback";
    case BoltMsg::Route:
      return "BoltMsg Route";
    case BoltMsg::Telemetry:
      return "BoltMsg Telemetry";
    default:
      return "Unkown bolt msg";
  }
}

}  // namespace bolt
