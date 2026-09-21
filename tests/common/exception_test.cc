#include "common/exception.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <type_traits>

static_assert(std::is_base_of_v<std::runtime_error, common::Exception>);
static_assert(std::is_final_v<common::Exception>);

TEST(ExceptionTest, ErrorCodeNamesAndDescriptionsAreConsistent) {
  EXPECT_STREQ(
      common::ErrorCodeToString(common::ErrorCode::VertexIndexAlreadyExists),
      "VertexIndexAlreadyExists");
  EXPECT_STREQ(common::ErrorCodeDesc(common::ErrorCode::InvalidIndexQuery),
               "Invalid index query.");
}

TEST(ExceptionTest, ThrowFormatsMessageAndCapturesSourceLocation) {
  try {
    RG_THROW(common::ErrorCode::InvalidParameter, "invalid value {}", 7);
  } catch (const common::Exception &error) {
    EXPECT_EQ(error.code(), common::ErrorCode::InvalidParameter);
    EXPECT_STREQ(common::ErrorCodeToString(error.code()), "InvalidParameter");
    EXPECT_EQ(error.message(), "invalid value 7");
    EXPECT_NE(error.file().find("exception_test.cc"), std::string::npos);
    EXPECT_EQ(error.function(), "TestBody");
    EXPECT_GT(error.line(), 0);
    EXPECT_NE(std::string(error.what()).find("InvalidParameter"),
              std::string::npos);
    return;
  }
  FAIL() << "expected common::Exception";
}

TEST(ExceptionTest, ThrowUsesDefaultDescription) {
  try {
    RG_THROW(common::ErrorCode::IndexNotReady);
  } catch (const common::Exception &error) {
    EXPECT_EQ(error.code(), common::ErrorCode::IndexNotReady);
    EXPECT_EQ(error.message(), "Index is still building.");
    return;
  }
  FAIL() << "expected common::Exception";
}

TEST(ExceptionTest, ErrorCodesHaveDescriptions) {
  EXPECT_STREQ(common::ErrorCodeToString(common::ErrorCode::InternalError),
               "InternalError");
  EXPECT_STREQ(common::ErrorCodeDesc(common::ErrorCode::QueryCancelled),
               "Query execution was cancelled.");
  EXPECT_STREQ(common::ErrorCodeDesc(common::ErrorCode::MemoryLimitExceeded),
               "Query memory limit exceeded.");
}
