#pragma once

#include <any>
#include <regex>
#include <string>
#include <unordered_map>

#include "common/temporal/duration.h"

class Value;

namespace common {
class LocalTime {
 private:
  int64_t nanoseconds_since_today_ = 0;

 public:
  LocalTime();
  explicit LocalTime(const std::string& str);
  explicit LocalTime(const Value& params, int64_t truncate = 0);
  explicit LocalTime(int64_t nanoseconds)
      : nanoseconds_since_today_(nanoseconds){};
  [[nodiscard]] int64_t GetStorage() const { return nanoseconds_since_today_; }
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] Value GetUnit(std::string unit) const;
  void fromTimeZone(std::string timezone);
  bool operator<(const LocalTime& rhs) const noexcept;
  bool operator<=(const LocalTime& rhs) const noexcept;
  bool operator>(const LocalTime& rhs) const noexcept;
  bool operator>=(const LocalTime& rhs) const noexcept;
  bool operator==(const LocalTime& rhs) const noexcept;
  bool operator!=(const LocalTime& rhs) const noexcept;
  LocalTime operator-(const Duration& duration) const;
  LocalTime operator+(const Duration& duration) const;
};
}  // namespace common