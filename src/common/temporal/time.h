#pragma once

#include <any>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>

#include "common/temporal/duration.h"

class Value;

namespace common {
class Time {
 private:
  int64_t nanoseconds_since_today_with_of_ = 0;
  int64_t tz_offset_seconds_ = 0;
  std::string timezone_name_ = "Z";
  [[nodiscard]] inline std::string timeOffsetToTimezone() const;

 public:
  Time();
  explicit Time(const std::string& str, std::optional<int64_t> days = {});
  explicit Time(const Value& params, int64_t truncate = 0);
  explicit Time(int64_t nanoseconds, int64_t tz_offset_seconds_ = 0)
      : nanoseconds_since_today_with_of_(nanoseconds),
        tz_offset_seconds_(tz_offset_seconds_){};
  [[nodiscard]] std::tuple<int64_t, int64_t> GetStorage() const {
    return {nanoseconds_since_today_with_of_, tz_offset_seconds_};
  }
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] std::string GetTimezoneName() const;
  [[nodiscard]] Value GetUnit(std::string unit) const;
  bool operator<(const Time& rhs) const noexcept;
  bool operator<=(const Time& rhs) const noexcept;
  bool operator>(const Time& rhs) const noexcept;
  bool operator>=(const Time& rhs) const noexcept;
  bool operator==(const Time& rhs) const noexcept;
  bool operator!=(const Time& rhs) const noexcept;
  Time operator-(const Duration& duration) const;
  Time operator+(const Duration& duration) const;
};
}  // namespace common
