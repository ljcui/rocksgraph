#include <gtest/gtest.h>

#include "common/flags.h"
#include "common/logger.h"
#include "test_util.h"

namespace {

void ConfigureTestLogger() {
  gflags::SetCommandLineOptionWithMode("log_level", "critical",
                                       gflags::SET_FLAG_IF_DEFAULT);
  spdlog::set_level(spdlog::level::from_str(FLAGS_log_level));
}

class TestDataCleanupListener : public testing::EmptyTestEventListener {
 public:
  void OnTestStart(const testing::TestInfo &) override {
    testutil::CleanupTestDataDirectories();
  }

  void OnTestEnd(const testing::TestInfo &) override {
    testutil::CleanupTestDataDirectories();
  }
};

}  // namespace

int main(int argc, char **argv) {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %t %l %s:%#] %v");
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  ConfigureTestLogger();
  testutil::CleanupTestDataDirectories();
  testing::UnitTest::GetInstance()->listeners().Append(
      new TestDataCleanupListener());
  int ret = RUN_ALL_TESTS();
  testutil::CleanupTestDataDirectories();
  return ret;
}
