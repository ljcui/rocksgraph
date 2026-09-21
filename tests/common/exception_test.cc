#include "common/exception.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <type_traits>

static_assert(std::is_base_of_v<std::runtime_error, common::Exception>);
static_assert(
    std::is_base_of_v<common::Exception, common::RocksGraphException>);

TEST(ExceptionTest, ErrorCodeNamesAndDescriptionsAreConsistent) {
  EXPECT_STREQ(
      common::ErrorCodeToString(common::ErrorCode::VertexIndexAlreadyExists),
      "VertexIndexAlreadyExists");
  EXPECT_STREQ(common::ErrorCodeDesc(common::ErrorCode::InvalidIndexQuery),
               "Invalid index query.");
}

TEST(ExceptionTest, ThrowCodeFormatsMessageAndCapturesSourceLocation) {
  try {
    RG_THROW_CODE(InvalidParameter, "invalid value {}", 7);
  } catch (const common::RocksGraphException &error) {
    EXPECT_EQ(error.code(), common::ErrorCode::InvalidParameter);
    EXPECT_EQ(error.Type(), "InvalidParameter");
    EXPECT_EQ(error.msg(), "invalid value 7");
    EXPECT_NE(error.File().find("exception_test.cc"), std::string::npos);
    EXPECT_EQ(error.Function(), "TestBody");
    EXPECT_GT(error.Line(), 0);
    EXPECT_NE(std::string(error.what()).find("InvalidParameter"),
              std::string::npos);
    return;
  }
  FAIL() << "expected common::RocksGraphException";
}

TEST(ExceptionTest, ThrowCodeUsesDefaultDescription) {
  try {
    RG_THROW_CODE(IndexNotReady);
  } catch (const common::RocksGraphException &error) {
    EXPECT_EQ(error.code(), common::ErrorCode::IndexNotReady);
    EXPECT_EQ(error.msg(), "Index is still building.");
    return;
  }
  FAIL() << "expected common::RocksGraphException";
}

TEST(ExceptionTest, TypedExceptionsCarryErrorCodes) {
  try {
    RG_THROW(common::InvalidArgumentError, "invalid argument");
  } catch (const common::Exception &error) {
    EXPECT_EQ(error.code(), common::ErrorCode::InvalidParameter);
    EXPECT_EQ(error.msg(), "invalid argument");
    EXPECT_EQ(error.Type(), "InvalidArgumentError");
    return;
  }
  FAIL() << "expected common::InvalidArgumentError";
}

TEST(ExceptionTest, TypedExceptionCodesHaveDescriptions) {
  EXPECT_STREQ(common::ErrorCodeToString(common::ErrorCode::InternalError),
               "InternalError");
  EXPECT_STREQ(common::ErrorCodeDesc(common::ErrorCode::QueryCancelled),
               "Query execution was cancelled.");
  EXPECT_STREQ(common::ErrorCodeDesc(common::ErrorCode::MemoryLimitExceeded),
               "Query memory limit exceeded.");
}
