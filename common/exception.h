#pragma once

#include <spdlog/fmt/fmt.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace common {

class Exception : public std::runtime_error {
 public:
  Exception(const char *type, std::string message, const char *file, int line,
            const char *function)
      : std::runtime_error(BuildWhat(type, message, file, line, function)),
        type_(type != nullptr ? type : "Exception"),
        message_(std::move(message)),
        file_(file != nullptr ? file : ""),
        function_(function != nullptr ? function : ""),
        line_(line) {}

  [[nodiscard]] const std::string &Type() const noexcept { return type_; }
  [[nodiscard]] const std::string &Message() const noexcept { return message_; }
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

  std::string type_;
  std::string message_;
  std::string file_;
  std::string function_;
  int line_ = 0;
};

#define RG_DEFINE_EXCEPTION(name, base)                            \
  class name : public base {                                       \
   public:                                                         \
    name(std::string message, const char *file, int line,          \
         const char *function)                                     \
        : base(#name, std::move(message), file, line, function) {} \
  }

RG_DEFINE_EXCEPTION(InvalidArgumentError, Exception);
RG_DEFINE_EXCEPTION(NotFoundError, Exception);
RG_DEFINE_EXCEPTION(InternalError, Exception);
RG_DEFINE_EXCEPTION(QueryCancelledError, Exception);
RG_DEFINE_EXCEPTION(MemoryLimitExceededError, Exception);

#undef RG_DEFINE_EXCEPTION

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
};

[[nodiscard]] const char *ErrorCodeToString(ErrorCode code) noexcept;
[[nodiscard]] const char *ErrorCodeDesc(ErrorCode code) noexcept;

class RocksGraphException : public Exception {
 public:
  RocksGraphException(ErrorCode code, std::string message, const char *file,
                      int line, const char *function)
      : Exception(ErrorCodeToString(code), std::move(message), file, line,
                  function),
        code_(code) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string &msg() const noexcept { return Message(); }

 private:
  ErrorCode code_;
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
