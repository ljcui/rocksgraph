#include <gtest/gtest.h>

#include <any>
#include <cstdint>

#include "bolt/io_service.h"
#include "bolt/temporal.h"
#include "bolt/to_string.h"
#include "common/exception.h"
#include "raft_driver/io_service.h"

TEST(ExceptionBoundaryTest, BoltIoServicePoolRejectsZeroSize) {
  EXPECT_THROW({ bolt::IOServicePool pool(0); }, common::InvalidArgumentError);
}

TEST(ExceptionBoundaryTest, RaftIoServicePoolRejectsZeroSize) {
  EXPECT_THROW({ raft::IOServicePool pool(0); }, common::InvalidArgumentError);
}

TEST(ExceptionBoundaryTest, BoltValuePrinterRejectsOutOfRangeDate) {
  EXPECT_THROW(bolt::Print(std::any(bolt::Date{.days = 1'000'000'000'000LL})),
               common::InvalidArgumentError);
}

TEST(ExceptionBoundaryTest, BoltValuePrinterRejectsUnsupportedType) {
  EXPECT_THROW(bolt::Print(std::any(std::int32_t{7})),
               common::InvalidArgumentError);
}
