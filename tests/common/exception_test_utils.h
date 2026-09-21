#pragma once

#include <gtest/gtest.h>

#include <exception>
#include <utility>

#include "common/exception.h"

namespace common::test {

template <typename Callable>
void ExpectExceptionCode(Callable &&callable, ErrorCode expected_code,
                         const char *file, int line) {
  try {
    std::forward<Callable>(callable)();
  } catch (const Exception &error) {
    if (error.code() != expected_code) {
      ADD_FAILURE_AT(file, line)
          << "expected error code " << ErrorCodeToString(expected_code)
          << ", actual " << ErrorCodeToString(error.code()) << ": "
          << error.message();
    }
    return;
  } catch (const std::exception &error) {
    ADD_FAILURE_AT(file, line) << "expected common::Exception with error code "
                               << ErrorCodeToString(expected_code)
                               << ", caught std::exception: " << error.what();
    return;
  } catch (...) {
    ADD_FAILURE_AT(file, line)
        << "expected common::Exception with error code "
        << ErrorCodeToString(expected_code) << ", caught unknown exception";
    return;
  }
  ADD_FAILURE_AT(file, line)
      << "expected common::Exception with error code "
      << ErrorCodeToString(expected_code) << ", no exception was thrown";
}

}  // namespace common::test

#define RG_EXPECT_ERROR(statement, error_code)                        \
  ::common::test::ExpectExceptionCode([&] { statement; }, error_code, \
                                      __FILE__, __LINE__)
