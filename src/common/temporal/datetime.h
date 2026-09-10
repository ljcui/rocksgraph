#pragma once

#include <string>

#include "common/temporal/duration.h"

// forword declaration to avoid include value.h here
class Value;

namespace common {
class DateTime {
 private:
  int64_t nanoseconds_since_epoch_ = 0;
  int64_t tz_offset_seconds_ = 0;
  std::string timezone_name_ = "Z";

 public:
  DateTime();
  /**
   * Construct a DateTime object from a temporal value
   *
   * \param   str: a string representation of a temporal value
   */
  explicit DateTime(const std::string& str);
  explicit DateTime(int64_t nanoseconds, int64_t offset_seconds = 0)
      : nanoseconds_since_epoch_(nanoseconds),
        tz_offset_seconds_(offset_seconds){};

  /**
   * Construct a DateTime object from a user supplied map
   *
   * \param params: a map containing the single key 'timezone', or a map
   *                containing temporal values ('year', 'month', 'day',
   *                'hour', 'minute', 'second',
   *                'millisecond', 'microsecond', 'nanosecond') as components.
   */
  explicit DateTime(const Value& params);
  [[nodiscard]] std::tuple<int64_t, int64_t> GetStorage() const {
    return {nanoseconds_since_epoch_, tz_offset_seconds_};
  }
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] std::string GetTimezoneName() const;
  [[nodiscard]] Value GetUnit(std::string unit) const;
  bool operator<(const DateTime& rhs) const noexcept;
  bool operator<=(const DateTime& rhs) const noexcept;
  bool operator>(const DateTime& rhs) const noexcept;
  bool operator>=(const DateTime& rhs) const noexcept;
  bool operator==(const DateTime& rhs) const noexcept;
  bool operator!=(const DateTime& rhs) const noexcept;
  DateTime operator-(const Duration& duration) const;
  DateTime operator+(const Duration& duration) const;
};

}  // namespace common
