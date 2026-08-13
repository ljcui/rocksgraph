#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "value/value.h"

namespace rg {

[[nodiscard]] Value ConstructDate(const Value *argument);
[[nodiscard]] Value ConstructLocalTime(const Value *argument);
[[nodiscard]] Value ConstructTime(const Value *argument);
[[nodiscard]] Value ConstructLocalDateTime(const Value *argument);
[[nodiscard]] Value ConstructDateTime(const Value *argument);
[[nodiscard]] Value ConstructDuration(const Value *argument);

[[nodiscard]] std::optional<Value> TemporalProperty(const Value &value,
                                                    std::string_view property);

[[nodiscard]] std::string FormatDate(const Date &date);
[[nodiscard]] std::string FormatLocalTime(const LocalTime &time);
[[nodiscard]] std::string FormatTime(const Time &time);
[[nodiscard]] std::string FormatLocalDateTime(const LocalDateTime &date_time);
[[nodiscard]] std::string FormatDateTime(const DateTime &date_time);
[[nodiscard]] std::string FormatDuration(const Duration &duration);

}  // namespace rg
