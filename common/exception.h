#pragma once

#include <stdexcept>
#include <string>
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
  [[nodiscard]] const std::string &Function() const noexcept { return function_; }
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

#define DEFINE_EXCEPTION(name, base)                               \
  class name : public base {                                       \
   public:                                                         \
    name(std::string message, const char *file, int line,          \
         const char *function)                                     \
        : base(#name, std::move(message), file, line, function) {} \
  }

DEFINE_EXCEPTION(InvalidArgumentError, Exception);
DEFINE_EXCEPTION(NotFoundError, Exception);
DEFINE_EXCEPTION(InternalError, Exception);

}  // namespace common

#define THROW(exception_type, ...) \
  throw exception_type(__VA_ARGS__, __FILE__, __LINE__, __func__)

#define THROW_IF(condition, exception_type, ...) \
  do {                                           \
    if (condition) {                             \
      THROW(exception_type, __VA_ARGS__);        \
    }                                            \
  } while (0)

#define CHECK(condition, exception_type, ...) \
  do {                                        \
    if (!(condition)) {                       \
      THROW(exception_type, __VA_ARGS__);     \
    }                                         \
  } while (0)
