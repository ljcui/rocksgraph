#pragma once

#include <spdlog/fmt/fmt.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace common {

enum class ErrorCode {
  UnknownError,
  VertexIdNotFound,
  StorageEngineError,
  EdgeTypeNotFound,
  EdgeIdNotFound,
  VertexIndexAlreadyExists,
  IndexValueAlreadyExists,
  NoSuchGraph,
  GraphAlreadyExists,
  IndexNotReady,
  SemanticError,
  ParseError,
  InputError,
  InvalidIndexQuery,
  FullTextIndexNotFound,
  VectorIndexNotFound,
  VertexUniqueIndexNotFound,
  EdgePropertyIndexAlreadyExists,
  EdgePropertyIndexNotFound,
  BoltDataError,
  ValueError,
  OutOfRange,
  InvalidParameter,
  VectorIndexError,
  VertexVectorIndexAlreadyExists,
  VertexFullTextIndexAlreadyExists,
  ConnectionDisconnected,
  Unimplemented,
  IOError,
  InternalError,
  QueryCancelled,
  MemoryLimitExceeded,
};

[[nodiscard]] const char *ErrorCodeToString(ErrorCode code) noexcept;
[[nodiscard]] const char *ErrorCodeDesc(ErrorCode code) noexcept;

class Exception final : public std::runtime_error {
 public:
  Exception(ErrorCode code, std::string message, const char *file, int line,
            const char *function)
      : std::runtime_error(BuildWhat(code, message, file, line, function)),
        code_(code),
        message_(std::move(message)),
        file_(file != nullptr ? file : ""),
        function_(function != nullptr ? function : ""),
        line_(line) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }
  [[nodiscard]] const std::string &file() const noexcept { return file_; }
  [[nodiscard]] const std::string &function() const noexcept {
    return function_;
  }
  [[nodiscard]] int line() const noexcept { return line_; }

 private:
  static std::string BuildWhat(ErrorCode code, const std::string &message,
                               const char *file, int line,
                               const char *function) {
    std::string out;
    out.append(ErrorCodeToString(code));
    if (!message.empty()) {
      out.append(": ");
      out.append(message);
    }
    if (file != nullptr && *file != '\0') {
      out.append(" @ ");
      out.append(file);
      out.push_back(':');
      out.append(std::to_string(line));
      if (function != nullptr && *function != '\0') {
        out.push_back(' ');
        out.append(function);
      }
    }
    return out;
  }

  ErrorCode code_ = ErrorCode::UnknownError;
  std::string message_;
  std::string file_;
  std::string function_;
  int line_ = 0;
};

inline std::string FormatErrorMessage(ErrorCode code) {
  return ErrorCodeDesc(code);
}

inline std::string FormatErrorMessage(ErrorCode, std::string message) {
  return message;
}

inline std::string FormatErrorMessage(ErrorCode, std::string_view message) {
  return std::string(message);
}

inline std::string FormatErrorMessage(ErrorCode, const char *message) {
  return message != nullptr ? message : "";
}

inline std::string FormatErrorMessage(
    ErrorCode, const std::vector<std::string> &messages) {
  std::string out;
  for (size_t i = 0; i < messages.size(); ++i) {
    if (i > 0) {
      out.append("; ");
    }
    out.append(messages[i]);
  }
  return out;
}

template <typename... Ts>
  requires(sizeof...(Ts) > 0)
std::string FormatErrorMessage(ErrorCode, const char *format,
                               const Ts &...args) {
  return fmt::format(fmt::runtime(format != nullptr ? format : ""), args...);
}

}  // namespace common

#define RG_THROW(error_code, ...)                                          \
  throw ::common::Exception(                                               \
      error_code,                                                          \
      ::common::FormatErrorMessage(error_code __VA_OPT__(, ) __VA_ARGS__), \
      __FILE__, __LINE__, __func__)

#define RG_CHECK(condition, error_code, ...)           \
  do {                                                 \
    if (!(condition)) {                                \
      RG_THROW(error_code __VA_OPT__(, ) __VA_ARGS__); \
    }                                                  \
  } while (0)
