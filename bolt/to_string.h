#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

#include "bolt/graph.h"
#include "bolt/record.h"
#include "value/temporal.h"

namespace bolt {
namespace detail {

rg::Date DateFromEpochDays(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era = (day_of_era - day_of_era / 1460 +
                                day_of_era / 36524 - day_of_era / 146096) /
                               365;
  std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned month_part = (5 * day_of_year + 2) / 153;
  const unsigned day = day_of_year - (153 * month_part + 2) / 5 + 1;
  const unsigned month = month_part < 10 ? month_part + 3 : month_part - 9;
  year += month <= 2;
  if (year < std::numeric_limits<std::int32_t>::min() ||
      year > std::numeric_limits<std::int32_t>::max()) {
    throw std::runtime_error("Bolt date is outside the supported year range");
  }
  return {static_cast<std::int32_t>(year), static_cast<std::int32_t>(month),
          static_cast<std::int32_t>(day)};
}

rg::LocalTime LocalTimeFromNanoseconds(std::int64_t nanoseconds) {
  constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
  constexpr std::int64_t kNanosecondsPerDay = 86'400 * kNanosecondsPerSecond;
  nanoseconds %= kNanosecondsPerDay;
  if (nanoseconds < 0) {
    nanoseconds += kNanosecondsPerDay;
  }
  const std::int64_t seconds = nanoseconds / kNanosecondsPerSecond;
  return {.hour = static_cast<std::int32_t>(seconds / 3600),
          .minute = static_cast<std::int32_t>((seconds / 60) % 60),
          .second = static_cast<std::int32_t>(seconds % 60),
          .nanosecond =
              static_cast<std::int32_t>(nanoseconds % kNanosecondsPerSecond),
          .has_seconds = true};
}

rg::LocalDateTime LocalDateTimeFromEpoch(std::int64_t seconds,
                                         std::int64_t nanoseconds) {
  constexpr std::int64_t kSecondsPerDay = 86'400;
  constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
  seconds += nanoseconds / kNanosecondsPerSecond;
  nanoseconds %= kNanosecondsPerSecond;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += kNanosecondsPerSecond;
  }
  std::int64_t days = seconds / kSecondsPerDay;
  std::int64_t seconds_of_day = seconds % kSecondsPerDay;
  if (seconds_of_day < 0) {
    --days;
    seconds_of_day += kSecondsPerDay;
  }
  return {DateFromEpochDays(days),
          LocalTimeFromNanoseconds(seconds_of_day * kNanosecondsPerSecond +
                                   nanoseconds)};
}

nlohmann::json ToJsonObj(const bolt::Node& node);
nlohmann::json ToJsonObj(const bolt::Relationship& rel);
nlohmann::json ToJsonObj(const bolt::Path& path);
nlohmann::json ToJsonObj(const std::any& item) {
  if (!item.has_value()) {
    return nullptr;
  }
  if (item.type() == typeid(int64_t)) {
    return std::any_cast<int64_t>(item);
  } else if (item.type() == typeid(bool)) {
    return std::any_cast<bool>(item);
  } else if (item.type() == typeid(float)) {
    return std::any_cast<float>(item);
  } else if (item.type() == typeid(double)) {
    return std::any_cast<double>(item);
  } else if (item.type() == typeid(std::string)) {
    return std::any_cast<const std::string&>(item);
  } else if (item.type() == typeid(const char*)) {
    return std::any_cast<const char*>(item);
  } else if (item.type() == typeid(bolt::Node)) {
    const auto& node = std::any_cast<const bolt::Node&>(item);
    return ToJsonObj(node);
  } else if (item.type() == typeid(bolt::Relationship)) {
    const auto& rel = std::any_cast<const bolt::Relationship&>(item);
    return ToJsonObj(rel);
  } else if (item.type() == typeid(bolt::Path)) {
    const auto& path = std::any_cast<const bolt::Path&>(item);
    return ToJsonObj(path);
  } else if (item.type() == typeid(bolt::Date)) {
    const auto& date = std::any_cast<const bolt::Date&>(item);
    return ToJsonObj(rg::FormatDate(DateFromEpochDays(date.days)));
  } else if (item.type() == typeid(bolt::DateTime)) {
    const auto& dateTime = std::any_cast<const bolt::DateTime&>(item);
    rg::DateTime value{
        LocalDateTimeFromEpoch(dateTime.seconds + dateTime.tz_offset_seconds,
                               dateTime.nanoseconds),
        static_cast<std::int32_t>(dateTime.tz_offset_seconds),
        {}};
    return ToJsonObj(rg::FormatDateTime(value));
  } else if (item.type() == typeid(bolt::LocalDateTime)) {
    const auto& localDateTime = std::any_cast<const bolt::LocalDateTime&>(item);
    return ToJsonObj(rg::FormatLocalDateTime(LocalDateTimeFromEpoch(
        localDateTime.seconds, localDateTime.nanoseconds)));
  } else if (item.type() == typeid(std::vector<std::any>)) {
    const auto& vector = std::any_cast<const std::vector<std::any>&>(item);
    nlohmann::json ret = nlohmann::json::array();
    for (auto& v : vector) {
      ret.push_back(ToJsonObj(v));
    }
    return ret;
  } else if (item.type() == typeid(std::unordered_map<std::string, std::any>)) {
    const auto& map =
        std::any_cast<const std::unordered_map<std::string, std::any>&>(item);
    nlohmann::json ret = nlohmann::json::object();
    for (auto& pair : map) {
      ret[pair.first] = ToJsonObj(pair.second);
    }
    return ret;
  } else if (item.type() == typeid(bolt::LocalTime)) {
    const auto& time = std::any_cast<const bolt::LocalTime&>(item);
    return ToJsonObj(
        rg::FormatLocalTime(LocalTimeFromNanoseconds(time.nanoseconds)));
  } else if (item.type() == typeid(bolt::Time)) {
    const auto& time = std::any_cast<const bolt::Time&>(item);
    rg::Time value{LocalTimeFromNanoseconds(time.nanoseconds),
                   static_cast<std::int32_t>(time.tz_offset_seconds),
                   {}};
    return ToJsonObj(rg::FormatTime(value));
  } else if (item.type() == typeid(bolt::Duration)) {
    const auto& duration = std::any_cast<const bolt::Duration&>(item);
    return ToJsonObj(
        rg::FormatDuration({duration.months, duration.days, duration.seconds,
                            static_cast<std::int32_t>(duration.nanos)}));
  } else {
    auto err = std::string("Unsupported type: ") + item.type().name();
    LOG_ERROR(err);
    throw std::runtime_error(err);
  }
}

nlohmann::json ToJsonObj(const bolt::Node& node) {
  std::string ret("(");
  for (auto& label : node.labels) {
    ret.append(":").append(label);
  }
  ret.append(" {");
  int count = 0;
  for (auto& pair : node.props) {
    if (count > 0) {
      ret.append(",");
    }
    ret.append(pair.first);
    ret.append(":");
    ret.append(ToJsonObj(pair.second).dump());
    count++;
  }
  ret.append("})");
  return ret;
}

nlohmann::json ToJsonObj(const bolt::Relationship& rel) {
  std::string ret;
  ret.append("[:").append(rel.type).append(" {");
  int count = 0;
  for (auto& pair : rel.props) {
    if (count > 0) {
      ret.append(",");
    }
    ret.append(pair.first);
    ret.append(":");
    ret.append(ToJsonObj(pair.second).dump());
    count++;
  }
  ret.append("}]");
  return ret;
}

nlohmann::json ToJsonObj(const bolt::Path& path) {
  std::unordered_map<int64_t, size_t> nodes_index;
  for (size_t i = 0; i < path.nodes.size(); i++) {
    nodes_index[path.nodes[i].id] = i;
  }
  auto ret = ToJsonObj(path.nodes[0]).get<std::string>();
  auto nodeId = path.nodes[0].id;
  bool forward = true;
  for (auto& rel : path.relationships) {
    forward = (rel.startId == nodeId);
    ret.append(forward ? "-" : "<-");
    ret.append(ToJsonObj(rel).get<std::string>());
    ret.append(forward ? "->" : "-");
    ret.append(
        ToJsonObj(path.nodes[nodes_index.at(forward ? rel.endId : rel.startId)])
            .get<std::string>());
    nodeId = forward ? rel.endId : rel.startId;
  }
  return ret;
}
}  // namespace detail

std::string Print(const std::any& boltType) {
  auto j = detail::ToJsonObj(boltType);
  if (j.is_string()) {
    return j.get<std::string>();
  } else {
    return j.dump();
  }
}

nlohmann::json ToJson(const std::any& boltType) {
  return detail::ToJsonObj(boltType);
}

}  // namespace bolt
