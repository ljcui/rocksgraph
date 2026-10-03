#include <gtest/gtest.h>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <optional>

#include "common/exception.h"
#include "common/flags.h"
#include "common/logger.h"

namespace {

// Each case owns its working directory, including when a binary is run directly
// with multiple cases or --gtest_repeat. Relative database paths stay isolated.
class TestWorkingDirectory final {
 public:
  TestWorkingDirectory()
      : original_(std::filesystem::current_path()),
        path_(
            std::filesystem::absolute(std::filesystem::temp_directory_path()) /
            ("rocksgraph-test-" +
             boost::uuids::to_string(boost::uuids::random_generator{}()))) {
    RG_CHECK(std::filesystem::create_directory(path_),
             common::ErrorCode::IOError, "Failed to create test directory");
    std::error_code error;
    std::filesystem::current_path(path_, error);
    if (error) {
      std::filesystem::remove_all(path_);
      RG_THROW(common::ErrorCode::IOError, "Failed to enter test directory: {}",
               error.message());
    }
  }

  ~TestWorkingDirectory() {
    std::error_code error;
    std::filesystem::current_path(original_, error);
    std::filesystem::remove_all(path_, error);
  }

  TestWorkingDirectory(const TestWorkingDirectory&) = delete;
  TestWorkingDirectory& operator=(const TestWorkingDirectory&) = delete;

 private:
  std::filesystem::path original_;
  std::filesystem::path path_;
};

class TestDirectoryListener final : public testing::EmptyTestEventListener {
 public:
  void OnTestStart(const testing::TestInfo&) override { directory_.emplace(); }
  void OnTestEnd(const testing::TestInfo&) override { directory_.reset(); }

 private:
  std::optional<TestWorkingDirectory> directory_;
};

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %t %l %s:%#] %v");
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetCommandLineOptionWithMode("log_level", "critical",
                                       gflags::SET_FLAG_IF_DEFAULT);
  spdlog::set_level(spdlog::level::from_str(FLAGS_log_level));
  testing::UnitTest::GetInstance()->listeners().Append(
      new TestDirectoryListener());
  return RUN_ALL_TESTS();
}
