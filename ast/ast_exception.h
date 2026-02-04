#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"

namespace ast {

using common::Exception;

class ErrorListException : public Exception {
 public:
  ErrorListException(const char *type, std::vector<std::string> errors,
                     const char *file, int line, const char *function)
      : Exception(type, JoinErrors(errors), file, line, function),
        errors_(std::move(errors)) {}

  const std::vector<std::string> &errors() const noexcept { return errors_; }

 private:
  static std::string JoinErrors(const std::vector<std::string> &errors) {
    std::string out;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i > 0) {
        out.append("; ");
      }
      out.append(errors[i]);
    }
    return out;
  }

  std::vector<std::string> errors_;
};

class ParseError : public ErrorListException {
 public:
  ParseError(std::vector<std::string> errors, const char *file, int line,
             const char *function)
      : ErrorListException("ParseError", std::move(errors), file, line,
                           function) {}
};

class SemanticError : public ErrorListException {
 public:
  SemanticError(std::vector<std::string> errors, const char *file, int line,
                const char *function)
      : ErrorListException("SemanticError", std::move(errors), file, line,
                           function) {}
};

}  // namespace ast
