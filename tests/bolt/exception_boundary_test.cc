#include <gtest/gtest.h>

#include <any>
#include <cstdint>

#include "bolt/temporal.h"
#include "bolt/to_string.h"
#include "common/exception.h"
#include "common/io_service_pool.h"

TEST(ExceptionBoundaryTest, IoServicePoolRejectsZeroSize) {
  EXPECT_THROW(
      { common::IOServicePool pool(0); }, common::InvalidArgumentError);
}

TEST(ExceptionBoundaryTest, BoltValuePrinterRejectsOutOfRangeDate) {
  EXPECT_THROW(bolt::Print(std::any(bolt::Date{.days = 1'000'000'000'000LL})),
               common::InvalidArgumentError);
}

TEST(ExceptionBoundaryTest, BoltValuePrinterRejectsUnsupportedType) {
  EXPECT_THROW(bolt::Print(std::any(std::int32_t{7})),
               common::InvalidArgumentError);
}
