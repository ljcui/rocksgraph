#pragma once
#include <string>

namespace bolt {

struct Date {
  int64_t days;
};

struct Time {
  int64_t nanoseconds;
  int64_t tz_offset_seconds;
};

struct LocalTime {
  int64_t nanoseconds;
};

struct DateTime {
  int64_t seconds;
  int64_t nanoseconds;
  int64_t tz_offset_seconds;
};

struct DateTimeZoneId {
  int64_t seconds;
  int64_t nanoseconds;
  std::string tz_id;
};

struct LocalDateTime {
  int64_t seconds;
  int64_t nanoseconds;
};

struct LegacyDateTime {
  int64_t seconds;
  int64_t nanoseconds;
  int64_t tz_offset_seconds;
};

struct LegacyDateTimeZoneId {
  int64_t seconds;
  int64_t nanoseconds;
  std::string tz_id;
};

// Duration represents temporal amount containing months, days, seconds and
// nanoseconds. Supports longer durations than time.Duration
struct Duration {
  int64_t months;
  int64_t days;
  int64_t seconds;
  int64_t nanos;
};

}  // namespace bolt
