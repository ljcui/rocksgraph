#include "exceptions.h"

const char* ErrorCodeToString(ErrorCode code) {
  switch (code) {
#define X(code, msg)    \
  case ErrorCode::code: \
    return #code;
    ERROR_CODES
#undef X
    default:
      return "Unknown Error Code";
  }
}

const char* ErrorCodeDesc(ErrorCode code) {
  switch (code) {
#define X(code, msg)    \
  case ErrorCode::code: \
    return msg;
    ERROR_CODES
#undef X
    default:
      return "Unknown Error Code";
  }
}

LgraphException::LgraphException(ErrorCode code)
    : code_(code), msg_(ErrorCodeDesc(code)) {
  what_ = fmt::format("[{}] {}", ErrorCodeToString(code_), msg_);
}

LgraphException::LgraphException(ErrorCode code, std::string msg)
    : code_(code), msg_(std::move(msg)) {
  what_ = fmt::format("[{}] {}", ErrorCodeToString(code_), msg_);
}

LgraphException::LgraphException(ErrorCode code, const char* msg)
    : code_(code), msg_(msg) {
  what_ = fmt::format("[{}] {}", ErrorCodeToString(code_), msg_);
}
