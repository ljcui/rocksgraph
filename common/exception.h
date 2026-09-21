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
  CypherException,
  ParserException,
  InputError,
  EvaluationException,
  InvalidIndexQuery,
  FullTextIndexNotFound,
  VectorIndexNotFound,
  VertexUniqueIndexNotFound,
  EdgePropertyIndexAlreadyExists,
  EdgePropertyIndexNotFound,
  BoltDataException,
  ValueException,
  OutOfRange,
  InvalidParameter,
  VectorIndexException,
  VertexVectorIndexAlreadyExists,
  VertexFullTextIndexAlreadyExists,
  ConnectionDisconnected,
  Unimplemented,
  IOException,
  NotFound,
  InternalError,
  QueryCancelled,
  MemoryLimitExceeded,
};

[[nodiscard]] const char *ErrorCodeToString(ErrorCode code) noexcept;
[[nodiscard]] const char *ErrorCodeDesc(ErrorCode code) noexcept;

class Exception : public std::runtime_error {
 public:
  Exception(ErrorCode code, const char *type, std::string message,
            const char *file, int line, const char *function)
      : std::runtime_error(BuildWhat(type, message, file, line, function)),
        code_(code),
        type_(type != nullptr ? type : "Exception"),
        message_(std::move(message)),
        file_(file != nullptr ? file : ""),
        function_(function != nullptr ? function : ""),
        line_(line) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string &Type() const noexcept { return type_; }
  [[nodiscard]] const std::string &Message() const noexcept { return message_; }
  [[nodiscard]] const std::string &msg() const noexcept { return message_; }
  [[nodiscard]] const std::string &File() const noexcept { return file_; }
  [[nodiscard]] const std::string &Function() const noexcept {
    return function_;
  }
  [[nodiscard]] int Line() const noexcept { return line_; }

 private:
  static std::string BuildWhat(const char *type, const std::string &message,
                               const char *file, int line,
                               const char *function) {
    std::string out;
    const char *type_name = type != nullptr ? type : "Exception";
    out.append(type_name);
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
  std::string type_;
  std::string message_;
  std::string file_;
  std::string function_;
  int line_ = 0;
};

#define RG_DEFINE_EXCEPTION(name, base, code)                            \
  class name : public base {                                             \
   public:                                                               \
    name(std::string message, const char *file, int line,                \
         const char *function)                                           \
        : base(code, #name, std::move(message), file, line, function) {} \
  }

RG_DEFINE_EXCEPTION(InvalidArgumentError, Exception,
                    ErrorCode::InvalidParameter);
RG_DEFINE_EXCEPTION(NotFoundError, Exception, ErrorCode::NotFound);
RG_DEFINE_EXCEPTION(InternalError, Exception, ErrorCode::InternalError);
RG_DEFINE_EXCEPTION(QueryCancelledError, Exception, ErrorCode::QueryCancelled);
RG_DEFINE_EXCEPTION(MemoryLimitExceededError, Exception,
                    ErrorCode::MemoryLimitExceeded);

#undef RG_DEFINE_EXCEPTION

class RocksGraphException : public Exception {
 public:
  RocksGraphException(ErrorCode code, std::string message, const char *file,
                      int line, const char *function)
      : Exception(code, ErrorCodeToString(code), std::move(message), file, line,
                  function) {}
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

#define RG_THROW(exception_type, ...) \
  throw exception_type(__VA_ARGS__, __FILE__, __LINE__, __func__)

#define RG_THROW_IF(condition, exception_type, ...) \
  do {                                              \
    if (condition) {                                \
      RG_THROW(exception_type, __VA_ARGS__);        \
    }                                               \
  } while (0)

#define RG_CHECK(condition, exception_type, ...) \
  do {                                           \
    if (!(condition)) {                          \
      RG_THROW(exception_type, __VA_ARGS__);     \
    }                                            \
  } while (0)

#define RG_THROW_CODE(code, ...)                                            \
  throw ::common::RocksGraphException(                                      \
      ::common::ErrorCode::code,                                            \
      ::common::FormatErrorMessage(::common::ErrorCode::code __VA_OPT__(, ) \
                                       __VA_ARGS__),                        \
      __FILE__, __LINE__, __func__)
