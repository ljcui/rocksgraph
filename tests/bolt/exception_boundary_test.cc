#include <gtest/gtest.h>

#include <any>
#include <cstdint>

#include "bolt/temporal.h"
#include "bolt/to_string.h"
#include "common/exception.h"
#include "common/io_service_pool.h"
#include "tests/common/exception_test_utils.h"

TEST(ExceptionBoundaryTest, IoServicePoolRejectsZeroSize) {
  RG_EXPECT_ERROR(
      { common::IOServicePool pool(0); }, common::ErrorCode::InvalidParameter);
}

TEST(ExceptionBoundaryTest, BoltValuePrinterRejectsOutOfRangeDate) {
  RG_EXPECT_ERROR(
      bolt::Print(std::any(bolt::Date{.days = 1'000'000'000'000LL})),
      common::ErrorCode::InvalidParameter);
}

TEST(ExceptionBoundaryTest, BoltValuePrinterRejectsUnsupportedType) {
  RG_EXPECT_ERROR(bolt::Print(std::any(std::int32_t{7})),
                  common::ErrorCode::InvalidParameter);
}
