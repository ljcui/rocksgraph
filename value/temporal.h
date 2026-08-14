#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "value/value.h"

namespace rg {

[[nodiscard]] Value ConstructDate(const Value *argument,
                                  std::chrono::system_clock::time_point now =
                                      std::chrono::system_clock::now());
[[nodiscard]] Value ConstructLocalTime(
    const Value *argument, std::chrono::system_clock::time_point now =
                               std::chrono::system_clock::now());
[[nodiscard]] Value ConstructTime(const Value *argument,
                                  std::chrono::system_clock::time_point now =
                                      std::chrono::system_clock::now());
[[nodiscard]] Value ConstructLocalDateTime(
    const Value *argument, std::chrono::system_clock::time_point now =
                               std::chrono::system_clock::now());
[[nodiscard]] Value ConstructDateTime(
    const Value *argument, std::chrono::system_clock::time_point now =
                               std::chrono::system_clock::now());
[[nodiscard]] Value ConstructDateTimeFromEpoch(const Value &seconds,
                                               const Value &nanoseconds);
[[nodiscard]] Value ConstructDateTimeFromEpochMillis(
    const Value &milliseconds);
[[nodiscard]] Value CurrentDate(
    const Value *timezone, std::chrono::system_clock::time_point now);
[[nodiscard]] Value CurrentLocalTime(
    const Value *timezone, std::chrono::system_clock::time_point now);
[[nodiscard]] Value CurrentTime(
    const Value *timezone, std::chrono::system_clock::time_point now);
[[nodiscard]] Value CurrentLocalDateTime(
    const Value *timezone, std::chrono::system_clock::time_point now);
[[nodiscard]] Value CurrentDateTime(
    const Value *timezone, std::chrono::system_clock::time_point now);
[[nodiscard]] Value ConstructDuration(const Value *argument);
[[nodiscard]] Value DurationBetween(const Value &left, const Value &right);
[[nodiscard]] Value DurationInMonths(const Value &left, const Value &right);
[[nodiscard]] Value DurationInDays(const Value &left, const Value &right);
[[nodiscard]] Value DurationInSeconds(const Value &left, const Value &right);
[[nodiscard]] Value TruncateDate(const Value &unit, const Value &input,
                                 const Value *fields);
[[nodiscard]] Value TruncateLocalTime(const Value &unit, const Value &input,
                                      const Value *fields);
[[nodiscard]] Value TruncateTime(const Value &unit, const Value &input,
                                 const Value *fields);
[[nodiscard]] Value TruncateLocalDateTime(const Value &unit, const Value &input,
                                          const Value *fields);
[[nodiscard]] Value TruncateDateTime(const Value &unit, const Value &input,
                                     const Value *fields);

[[nodiscard]] std::optional<Value> TemporalProperty(const Value &value,
                                                    std::string_view property);

[[nodiscard]] std::string FormatDate(const Date &date);
[[nodiscard]] std::string FormatLocalTime(const LocalTime &time);
[[nodiscard]] std::string FormatTime(const Time &time);
[[nodiscard]] std::string FormatLocalDateTime(const LocalDateTime &date_time);
[[nodiscard]] std::string FormatDateTime(const DateTime &date_time);
[[nodiscard]] std::string FormatDuration(const Duration &duration);

}  // namespace rg
