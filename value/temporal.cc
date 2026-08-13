#include "value/temporal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

#include "common/exception.h"

namespace rg {
namespace {

using Days = std::chrono::days;
using LocalSeconds = std::chrono::local_time<std::chrono::seconds>;
using SysDays = std::chrono::sys_days;
using namespace std::chrono_literals;

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::int64_t kSecondsPerDay = 86'400;
constexpr long double kAverageMonthSeconds = 2'629'746.0L;
constexpr int kMinimumYear = -999'999'999;
constexpr int kMaximumYear = 999'999'999;

[[noreturn]] void InvalidTemporal(std::string message) {
  THROW(common::InvalidArgumentError, std::move(message));
}

std::int64_t ParseInteger(std::string_view text, std::string_view context) {
  try {
    std::size_t parsed = 0;
    const std::int64_t value = std::stoll(std::string(text), &parsed);
    if (parsed != text.size()) {
      InvalidTemporal("invalid " + std::string(context));
    }
    return value;
  } catch (const std::exception &) {
    InvalidTemporal("invalid " + std::string(context));
  }
}

long double ParseDecimal(std::string_view text, std::string_view context) {
  try {
    std::size_t parsed = 0;
    const long double value = std::stold(std::string(text), &parsed);
    if (parsed != text.size() || !std::isfinite(value)) {
      InvalidTemporal("invalid " + std::string(context));
    }
    return value;
  } catch (const std::exception &) {
    InvalidTemporal("invalid " + std::string(context));
  }
}

int CheckedInt(std::int64_t value, int minimum, int maximum,
               std::string_view component) {
  if (value < minimum || value > maximum) {
    InvalidTemporal(std::string(component) + " is out of range");
  }
  return static_cast<int>(value);
}

bool LeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    InvalidTemporal("month is out of range");
  }
  return month == 2 && LeapYear(year) ? 29 : kDays[month - 1];
}

void ValidateDate(const Date &date) {
  if (date.year < kMinimumYear || date.year > kMaximumYear) {
    InvalidTemporal("year is out of range");
  }
  if (date.day < 1 || date.day > DaysInMonth(date.year, date.month)) {
    InvalidTemporal("invalid calendar date");
  }
}

std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  std::int64_t adjusted_year = year;
  adjusted_year -= month <= 2;
  const std::int64_t era =
      (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
  const unsigned day_of_year =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

SysDays ToSysDays(const Date &date) {
  ValidateDate(date);
  return SysDays{
      Days{DaysFromCivil(date.year, static_cast<unsigned>(date.month),
                         static_cast<unsigned>(date.day))}};
}

Date FromSysDays(SysDays value) {
  std::int64_t days = value.time_since_epoch().count() + 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
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
  if (year < kMinimumYear || year > kMaximumYear) {
    InvalidTemporal("year is out of range");
  }
  return {static_cast<int>(year), static_cast<int>(month),
          static_cast<int>(day)};
}

int IsoWeekday(SysDays day) { return std::chrono::weekday{day}.iso_encoding(); }

struct IsoWeek {
  int year = 0;
  int week = 0;
  int weekday = 0;
};

IsoWeek IsoWeekComponents(const Date &date) {
  const SysDays day = ToSysDays(date);
  const int weekday = IsoWeekday(day);
  const SysDays thursday = day + Days{4 - weekday};
  const int week_year = FromSysDays(thursday).year;
  const SysDays january_four = ToSysDays(Date{week_year, 1, 4});
  const SysDays week_one = january_four - Days{IsoWeekday(january_four) - 1};
  return {week_year, static_cast<int>((day - week_one).count() / 7) + 1,
          weekday};
}

Date DateFromIsoWeek(int year, int week, int weekday) {
  if (week < 1 || week > 53 || weekday < 1 || weekday > 7) {
    InvalidTemporal("invalid ISO week date");
  }
  const SysDays january_four = ToSysDays(Date{year, 1, 4});
  const SysDays week_one = january_four - Days{IsoWeekday(january_four) - 1};
  const Date result =
      FromSysDays(week_one + Days{(week - 1) * 7 + weekday - 1});
  const IsoWeek components = IsoWeekComponents(result);
  if (components.year != year || components.week != week ||
      components.weekday != weekday) {
    InvalidTemporal("invalid ISO week date");
  }
  return result;
}

Date DateFromOrdinal(int year, int ordinal) {
  const int maximum = LeapYear(year) ? 366 : 365;
  if (ordinal < 1 || ordinal > maximum) {
    InvalidTemporal("ordinal day is out of range");
  }
  return FromSysDays(ToSysDays(Date{year, 1, 1}) + Days{ordinal - 1});
}

int OrdinalDay(const Date &date) {
  const SysDays first = ToSysDays(Date{date.year, 1, 1});
  return static_cast<int>((ToSysDays(date) - first).count()) + 1;
}

Date DateFromQuarter(int year, int quarter, int day_of_quarter) {
  if (quarter < 1 || quarter > 4) {
    InvalidTemporal("quarter is out of range");
  }
  const int first_month = (quarter - 1) * 3 + 1;
  const SysDays start = ToSysDays(Date{year, first_month, 1});
  const SysDays end = quarter == 4 ? ToSysDays(Date{year + 1, 1, 1})
                                   : ToSysDays(Date{year, first_month + 3, 1});
  if (day_of_quarter < 1 || day_of_quarter > (end - start).count()) {
    InvalidTemporal("day of quarter is out of range");
  }
  return FromSysDays(start + Days{day_of_quarter - 1});
}

std::optional<std::int64_t> MapInteger(const Value::Map &map,
                                       std::string_view key) {
  const auto found = map.find(std::string(key));
  if (found == map.end()) {
    return std::nullopt;
  }
  if (!found->second.IsInteger()) {
    InvalidTemporal(std::string(key) + " must be an integer");
  }
  return found->second.AsInteger();
}

std::optional<long double> MapNumber(const Value::Map &map,
                                     std::string_view key) {
  const auto found = map.find(std::string(key));
  if (found == map.end()) {
    return std::nullopt;
  }
  if (found->second.IsInteger()) {
    return static_cast<long double>(found->second.AsInteger());
  }
  if (found->second.IsDouble()) {
    return static_cast<long double>(found->second.AsDouble());
  }
  InvalidTemporal(std::string(key) + " must be numeric");
}

const Value *MapValue(const Value::Map &map, std::string_view key) {
  const auto found = map.find(std::string(key));
  return found == map.end() ? nullptr : &found->second;
}

std::optional<std::string> MapString(const Value::Map &map,
                                     std::string_view key) {
  const Value *value = MapValue(map, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (!value->IsString()) {
    InvalidTemporal(std::string(key) + " must be a string");
  }
  return value->AsString();
}

int MapIntOr(const Value::Map &map, std::string_view key, int fallback,
             int minimum, int maximum) {
  const auto value = MapInteger(map, key);
  return value.has_value() ? CheckedInt(*value, minimum, maximum, key)
                           : fallback;
}

Date DateFromTemporal(const Value &value) {
  if (value.IsDate()) {
    return value.AsDate();
  }
  if (value.IsLocalDateTime()) {
    return value.AsLocalDateTime().date;
  }
  if (value.IsDateTime()) {
    return value.AsDateTime().local_date_time.date;
  }
  InvalidTemporal("date component must be a date or date-time value");
}

std::optional<Date> BaseDateFromMap(const Value::Map &map) {
  if (const Value *value = MapValue(map, "date"); value != nullptr) {
    return DateFromTemporal(*value);
  }
  if (const Value *value = MapValue(map, "datetime"); value != nullptr) {
    return DateFromTemporal(*value);
  }
  return std::nullopt;
}

int DayOfQuarter(const Date &date) {
  const int first_month = ((date.month - 1) / 3) * 3 + 1;
  return static_cast<int>(
             (ToSysDays(date) - ToSysDays(Date{date.year, first_month, 1}))
                 .count()) +
         1;
}

Date ApplyDateComponents(const Value::Map &map, std::optional<Date> base) {
  if (MapInteger(map, "week").has_value()) {
    int year = base.has_value() ? IsoWeekComponents(*base).year : 0;
    if (const auto specified = MapInteger(map, "year"); specified.has_value()) {
      year = CheckedInt(*specified, kMinimumYear, kMaximumYear, "year");
    }
    if (year == 0) {
      InvalidTemporal("year is required for ISO week date");
    }
    const int week = CheckedInt(*MapInteger(map, "week"), 1, 53, "week");
    const int weekday =
        MapIntOr(map, "dayOfWeek",
                 base.has_value() ? IsoWeekComponents(*base).weekday : 1, 1, 7);
    return DateFromIsoWeek(year, week, weekday);
  }

  int year = base.has_value() ? base->year : 0;
  if (const auto specified = MapInteger(map, "year"); specified.has_value()) {
    year = CheckedInt(*specified, kMinimumYear, kMaximumYear, "year");
  }
  if (year == 0) {
    InvalidTemporal("year is required for date construction");
  }
  if (const auto ordinal = MapInteger(map, "ordinalDay"); ordinal.has_value()) {
    return DateFromOrdinal(year, CheckedInt(*ordinal, 1, 366, "ordinalDay"));
  }
  if (const auto quarter = MapInteger(map, "quarter"); quarter.has_value()) {
    const int day =
        MapIntOr(map, "dayOfQuarter", base ? DayOfQuarter(*base) : 1, 1, 92);
    return DateFromQuarter(year, CheckedInt(*quarter, 1, 4, "quarter"), day);
  }

  const int month = MapIntOr(map, "month", base ? base->month : 1, 1, 12);
  const int day = MapIntOr(map, "day", base ? base->day : 1, 1, 31);
  const Date result{year, month, day};
  (void)ToSysDays(result);
  return result;
}

Date DateFromMap(const Value::Map &map) {
  return ApplyDateComponents(map, BaseDateFromMap(map));
}

int FractionNanoseconds(std::string_view fraction) {
  if (fraction.empty() || fraction.size() > 9 ||
      !std::ranges::all_of(fraction,
                           [](char ch) { return ch >= '0' && ch <= '9'; })) {
    InvalidTemporal("invalid fractional second");
  }
  std::string padded(fraction);
  padded.append(9 - padded.size(), '0');
  return CheckedInt(ParseInteger(padded, "fractional second"), 0, 999'999'999,
                    "nanosecond");
}

LocalTime ParseLocalTime(std::string_view text) {
  static const std::regex kExtended(
      R"(^([0-9]{2})(?::([0-9]{2})(?::([0-9]{2})(?:\.([0-9]{1,9}))?)?)?$)");
  static const std::regex kBasic(
      R"(^([0-9]{2})(?:([0-9]{2})(?:([0-9]{2})(?:\.([0-9]{1,9}))?)?)?$)");
  std::smatch match;
  const std::string input(text);
  if (!std::regex_match(input, match, kExtended) &&
      !std::regex_match(input, match, kBasic)) {
    InvalidTemporal("invalid local time");
  }
  const int hour =
      CheckedInt(ParseInteger(match[1].str(), "hour"), 0, 23, "hour");
  const int minute =
      match[2].matched
          ? CheckedInt(ParseInteger(match[2].str(), "minute"), 0, 59, "minute")
          : 0;
  const bool has_seconds = match[3].matched;
  const int second =
      has_seconds
          ? CheckedInt(ParseInteger(match[3].str(), "second"), 0, 59, "second")
          : 0;
  const int nanosecond =
      match[4].matched ? FractionNanoseconds(match[4].str()) : 0;
  return {hour, minute, second, nanosecond, has_seconds};
}

LocalTime TimeFromTemporal(const Value &value) {
  if (value.IsLocalTime()) {
    return value.AsLocalTime();
  }
  if (value.IsTime()) {
    return value.AsTime().local_time;
  }
  if (value.IsLocalDateTime()) {
    return value.AsLocalDateTime().time;
  }
  if (value.IsDateTime()) {
    return value.AsDateTime().local_date_time.time;
  }
  InvalidTemporal("time component must be a time or date-time value");
}

std::optional<LocalTime> BaseTimeFromMap(const Value::Map &map) {
  if (const Value *value = MapValue(map, "time"); value != nullptr) {
    return TimeFromTemporal(*value);
  }
  if (const Value *value = MapValue(map, "datetime"); value != nullptr) {
    return TimeFromTemporal(*value);
  }
  return std::nullopt;
}

LocalTime ApplyTimeComponents(const Value::Map &map,
                              std::optional<LocalTime> base) {
  const int hour = MapIntOr(map, "hour", base ? base->hour : 0, 0, 23);
  const int minute = MapIntOr(map, "minute", base ? base->minute : 0, 0, 59);
  const bool specifies_fraction = MapInteger(map, "millisecond").has_value() ||
                                  MapInteger(map, "microsecond").has_value() ||
                                  MapInteger(map, "nanosecond").has_value();
  const bool has_seconds = base ? base->has_seconds : false;
  const bool specifies_second = MapInteger(map, "second").has_value();
  const int second = MapIntOr(map, "second", base ? base->second : 0, 0, 59);
  std::int64_t nanosecond = base ? base->nanosecond : 0;
  if (specifies_fraction) {
    nanosecond = MapInteger(map, "millisecond").value_or(0) * 1'000'000 +
                 MapInteger(map, "microsecond").value_or(0) * 1'000 +
                 MapInteger(map, "nanosecond").value_or(0);
  }
  return {hour, minute, second,
          CheckedInt(nanosecond, 0, 999'999'999, "nanosecond"),
          has_seconds || specifies_second || specifies_fraction};
}

LocalTime LocalTimeFromMap(const Value::Map &map) {
  return ApplyTimeComponents(map, BaseTimeFromMap(map));
}

Date ParseDate(std::string_view text) {
  static const std::regex kWeek(
      R"(^([+-]?[0-9]{4,})-?W([0-9]{2})(?:-?([0-9]))?$)");
  static const std::regex kExtendedCalendar(
      R"(^([+-]?[0-9]{4,})-([0-9]{2})(?:-([0-9]{2}))?$)");
  static const std::regex kExtendedOrdinal(R"(^([+-]?[0-9]{4,})-([0-9]{3})$)");
  static const std::regex kDigits(R"(^([+-]?)([0-9]+)$)");
  std::smatch match;
  const std::string input(text);
  if (std::regex_match(input, match, kWeek)) {
    return DateFromIsoWeek(
        CheckedInt(ParseInteger(match[1].str(), "year"), kMinimumYear,
                   kMaximumYear, "year"),
        CheckedInt(ParseInteger(match[2].str(), "week"), 1, 53, "week"),
        match[3].matched ? CheckedInt(ParseInteger(match[3].str(), "weekday"),
                                      1, 7, "weekday")
                         : 1);
  }
  if (std::regex_match(input, match, kExtendedOrdinal)) {
    return DateFromOrdinal(
        CheckedInt(ParseInteger(match[1].str(), "year"), kMinimumYear,
                   kMaximumYear, "year"),
        CheckedInt(ParseInteger(match[2].str(), "ordinal day"), 1, 366,
                   "ordinal day"));
  }
  if (std::regex_match(input, match, kExtendedCalendar)) {
    const Date result{
        CheckedInt(ParseInteger(match[1].str(), "year"), kMinimumYear,
                   kMaximumYear, "year"),
        CheckedInt(ParseInteger(match[2].str(), "month"), 1, 12, "month"),
        match[3].matched
            ? CheckedInt(ParseInteger(match[3].str(), "day"), 1, 31, "day")
            : 1};
    (void)ToSysDays(result);
    return result;
  }
  if (!std::regex_match(input, match, kDigits)) {
    InvalidTemporal("invalid date");
  }
  const std::string digits = match[2].str();
  const int sign = match[1].str() == "-" ? -1 : 1;
  if (digits.size() == 4) {
    return {sign * CheckedInt(ParseInteger(digits, "year"), 1, kMaximumYear,
                              "year"),
            1, 1};
  }
  if (digits.size() == 6 || digits.size() == 8) {
    const Date result{
        sign * CheckedInt(ParseInteger(digits.substr(0, 4), "year"), 1,
                          kMaximumYear, "year"),
        CheckedInt(ParseInteger(digits.substr(4, 2), "month"), 1, 12, "month"),
        digits.size() == 8
            ? CheckedInt(ParseInteger(digits.substr(6, 2), "day"), 1, 31, "day")
            : 1};
    (void)ToSysDays(result);
    return result;
  }
  if (digits.size() == 7) {
    return DateFromOrdinal(
        sign * CheckedInt(ParseInteger(digits.substr(0, 4), "year"), 1,
                          kMaximumYear, "year"),
        CheckedInt(ParseInteger(digits.substr(4), "ordinal day"), 1, 366,
                   "ordinal day"));
  }
  InvalidTemporal("invalid date");
}

std::string FormatOffset(int offset_seconds) {
  if (offset_seconds == 0) {
    return "Z";
  }
  const char sign = offset_seconds < 0 ? '-' : '+';
  int remaining = std::abs(offset_seconds);
  const int hours = remaining / 3600;
  remaining %= 3600;
  const int minutes = remaining / 60;
  const int seconds = remaining % 60;
  std::ostringstream out;
  out << sign << std::setw(2) << std::setfill('0') << hours << ':'
      << std::setw(2) << minutes;
  if (seconds != 0) {
    out << ':' << std::setw(2) << seconds;
  }
  return out.str();
}

int ParseOffset(std::string_view text) {
  if (text == "Z" || text == "z") {
    return 0;
  }
  static const std::regex kOffset(
      R"(^([+-])([0-9]{2})(?::?([0-9]{2}))?(?::?([0-9]{2}))?$)");
  std::smatch match;
  const std::string input(text);
  if (!std::regex_match(input, match, kOffset)) {
    InvalidTemporal("invalid UTC offset");
  }
  const int hour = CheckedInt(ParseInteger(match[2].str(), "offset hour"), 0,
                              18, "offset hour");
  const int minute =
      match[3].matched
          ? CheckedInt(ParseInteger(match[3].str(), "offset minute"), 0, 59,
                       "offset minute")
          : 0;
  const int second =
      match[4].matched
          ? CheckedInt(ParseInteger(match[4].str(), "offset second"), 0, 59,
                       "offset second")
          : 0;
  if (hour == 18 && (minute != 0 || second != 0)) {
    InvalidTemporal("UTC offset is out of range");
  }
  const int value = hour * 3600 + minute * 60 + second;
  return match[1].str() == "-" ? -value : value;
}

LocalSeconds ToLocalSeconds(const LocalDateTime &date_time) {
  const auto date = std::chrono::local_days{
      std::chrono::year{date_time.date.year} /
      std::chrono::month{static_cast<unsigned>(date_time.date.month)} /
      std::chrono::day{static_cast<unsigned>(date_time.date.day)}};
  return date + std::chrono::hours{date_time.time.hour} +
         std::chrono::minutes{date_time.time.minute} +
         std::chrono::seconds{date_time.time.second};
}

int ZoneOffset(const std::string &zone_name,
               const LocalDateTime &local_date_time) {
  if (zone_name == "Europe/Stockholm" &&
      ToSysDays(local_date_time.date) <
          SysDays{std::chrono::year{1879} / std::chrono::January / 1}) {
    // openCypher follows the Java timezone database's pre-standard-time LMT.
    return 53 * 60 + 28;
  }
  try {
    const std::chrono::time_zone *zone = std::chrono::locate_zone(zone_name);
    const std::chrono::local_info info =
        zone->get_info(ToLocalSeconds(local_date_time));
    if (info.result == std::chrono::local_info::nonexistent) {
      InvalidTemporal("local date-time does not exist in timezone: " +
                      zone_name);
    }
    return static_cast<int>(info.first.offset.count());
  } catch (const common::Exception &) {
    throw;
  } catch (const std::exception &) {
    InvalidTemporal("unknown timezone: " + zone_name);
  }
}

struct ParsedTime {
  LocalTime local_time;
  int offset_seconds = 0;
  std::string timezone;
};

ParsedTime ParseOffsetTime(std::string text, bool require_offset) {
  std::string timezone;
  if (!text.empty() && text.back() == ']') {
    const std::size_t open = text.rfind('[');
    if (open == std::string::npos || open + 1 == text.size() - 1) {
      InvalidTemporal("invalid timezone suffix");
    }
    timezone = text.substr(open + 1, text.size() - open - 2);
    text.erase(open);
  }

  static const std::regex kOffsetAtEnd(
      R"((Z|z|[+-][0-9]{2}(?::?[0-9]{2})?(?::?[0-9]{2})?)$)");
  std::smatch match;
  std::optional<int> offset;
  if (std::regex_search(text, match, kOffsetAtEnd)) {
    offset = ParseOffset(match[1].str());
    text.erase(match.position(1));
  }
  if (require_offset && !offset.has_value() && timezone.empty()) {
    InvalidTemporal("timezone offset is required");
  }
  return {ParseLocalTime(text), offset.value_or(0), std::move(timezone)};
}

LocalDateTime ParseLocalDateTime(std::string_view text) {
  const std::size_t separator = text.find_first_of("Tt");
  if (separator == std::string_view::npos) {
    return {ParseDate(text), LocalTime{0, 0, 0, 0, false}};
  }
  return {ParseDate(text.substr(0, separator)),
          ParseLocalTime(text.substr(separator + 1))};
}

DateTime ParseDateTime(std::string_view text) {
  const std::size_t separator = text.find_first_of("Tt");
  if (separator == std::string_view::npos) {
    InvalidTemporal("invalid date-time");
  }
  const Date date = ParseDate(text.substr(0, separator));
  ParsedTime parsed =
      ParseOffsetTime(std::string(text.substr(separator + 1)), true);
  LocalDateTime local{date, parsed.local_time};
  if (!parsed.timezone.empty() &&
      text.find_first_of("Zz+-", separator + 1) == std::string_view::npos) {
    parsed.offset_seconds = ZoneOffset(parsed.timezone, local);
  }
  return {local, parsed.offset_seconds, std::move(parsed.timezone)};
}

std::string TimezoneFromMap(const Value::Map &map, std::string fallback = {}) {
  return MapString(map, "timezone").value_or(std::move(fallback));
}

int OffsetForTimezone(const std::string &timezone,
                      const LocalDateTime &local_date_time) {
  if (timezone.empty()) {
    return 0;
  }
  if (timezone == "Z" || timezone == "z" || timezone.front() == '+' ||
      timezone.front() == '-') {
    return ParseOffset(timezone);
  }
  return ZoneOffset(timezone, local_date_time);
}

struct SourceOffset {
  int seconds = 0;
  std::string timezone;
  bool has_offset = false;
};

SourceOffset SourceOffsetFromMap(const Value::Map &map) {
  const Value *value = MapValue(map, "datetime");
  if (value == nullptr) {
    value = MapValue(map, "time");
  }
  if (value == nullptr) {
    return {};
  }
  if (value->IsDateTime()) {
    return {value->AsDateTime().utc_offset_seconds,
            value->AsDateTime().timezone, true};
  }
  if (value->IsTime()) {
    return {value->AsTime().utc_offset_seconds, value->AsTime().timezone, true};
  }
  return {};
}

bool NumericTimezone(std::string_view timezone) {
  return timezone.empty() || timezone == "Z" || timezone == "z" ||
         timezone.front() == '+' || timezone.front() == '-';
}

std::string StoredTimezone(std::string_view timezone) {
  return NumericTimezone(timezone) ? std::string{} : std::string(timezone);
}

LocalDateTime ConvertOffset(LocalDateTime local_date_time, int source_offset,
                            int target_offset) {
  const auto adjusted = ToLocalSeconds(local_date_time) +
                        std::chrono::seconds{target_offset - source_offset};
  const auto day = std::chrono::floor<Days>(adjusted);
  const auto time = adjusted - day;
  const auto hours = std::chrono::duration_cast<std::chrono::hours>(time);
  const auto minutes =
      std::chrono::duration_cast<std::chrono::minutes>(time - hours);
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(time - hours - minutes);
  local_date_time.date = FromSysDays(SysDays{day.time_since_epoch()});
  local_date_time.time.hour = static_cast<int>(hours.count());
  local_date_time.time.minute = static_cast<int>(minutes.count());
  local_date_time.time.second = static_cast<int>(seconds.count());
  return local_date_time;
}

int TargetOffsetForConversion(std::string_view timezone,
                              const LocalDateTime &source_local,
                              int source_offset) {
  if (NumericTimezone(timezone)) {
    return timezone.empty() ? 0 : ParseOffset(timezone);
  }
  const std::chrono::sys_seconds instant{
      ToLocalSeconds(source_local).time_since_epoch() -
      std::chrono::seconds{source_offset}};
  try {
    const std::chrono::time_zone *zone = std::chrono::locate_zone(timezone);
    return static_cast<int>(zone->get_info(instant).offset.count());
  } catch (const std::exception &) {
    InvalidTemporal("unknown timezone: " + std::string(timezone));
  }
}

Duration NormalizeDuration(std::int64_t months, std::int64_t days,
                           long double seconds) {
  const long double nanoseconds =
      std::round(seconds * static_cast<long double>(kNanosecondsPerSecond));
  if (nanoseconds <
          static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
      nanoseconds >
          static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
    InvalidTemporal("duration is out of range");
  }
  const std::int64_t total_nanoseconds = static_cast<std::int64_t>(nanoseconds);
  std::int64_t whole_seconds = total_nanoseconds / kNanosecondsPerSecond;
  std::int64_t nanosecond_remainder = total_nanoseconds % kNanosecondsPerSecond;
  if (nanosecond_remainder < 0) {
    --whole_seconds;
    nanosecond_remainder += kNanosecondsPerSecond;
  }
  return {months, days, whole_seconds,
          static_cast<std::int32_t>(nanosecond_remainder)};
}

Duration DurationFromMap(const Value::Map &map) {
  const long double years = MapNumber(map, "years").value_or(0);
  const long double months = MapNumber(map, "months").value_or(0);
  const long double total_months = years * 12 + months;
  const std::int64_t whole_months = static_cast<std::int64_t>(total_months);
  const long double fractional_month_seconds =
      (total_months - whole_months) * kAverageMonthSeconds;
  const std::int64_t month_days =
      static_cast<std::int64_t>(fractional_month_seconds / kSecondsPerDay);
  long double seconds = fractional_month_seconds - month_days * kSecondsPerDay;

  const long double weeks = MapNumber(map, "weeks").value_or(0);
  const long double days_value = MapNumber(map, "days").value_or(0) + weeks * 7;
  const std::int64_t explicit_days = static_cast<std::int64_t>(days_value);
  const std::int64_t whole_days = explicit_days + month_days;
  seconds += (days_value - explicit_days) * kSecondsPerDay;
  seconds += MapNumber(map, "hours").value_or(0) * 3600;
  seconds += MapNumber(map, "minutes").value_or(0) * 60;
  seconds += MapNumber(map, "seconds").value_or(0);
  seconds += MapNumber(map, "milliseconds").value_or(0) / 1000;
  seconds += MapNumber(map, "microseconds").value_or(0) / 1'000'000;
  seconds += MapNumber(map, "nanoseconds").value_or(0) / 1'000'000'000;
  return NormalizeDuration(whole_months, whole_days, seconds);
}

Duration ParseDuration(std::string_view text) {
  static const std::regex kAlternative(
      R"(^P([+-]?[0-9]+)-([+-]?[0-9]+)-([+-]?[0-9]+)T([+-]?[0-9]+):([+-]?[0-9]+):([+-]?[0-9]+(?:\.[0-9]+)?)$)",
      std::regex::icase);
  static const std::regex kIso(
      R"(^P(?:(?:([+-]?[0-9]+(?:\.[0-9]+)?)Y)?(?:([+-]?[0-9]+(?:\.[0-9]+)?)M)?(?:([+-]?[0-9]+(?:\.[0-9]+)?)W)?(?:([+-]?[0-9]+(?:\.[0-9]+)?)D)?)?(?:T(?:([+-]?[0-9]+(?:\.[0-9]+)?)H)?(?:([+-]?[0-9]+(?:\.[0-9]+)?)M)?(?:([+-]?[0-9]+(?:\.[0-9]+)?)S)?)?$)",
      std::regex::icase);
  std::smatch match;
  const std::string input(text);
  if (std::regex_match(input, match, kAlternative)) {
    Value::Map map{
        {"years", Value(ParseInteger(match[1].str(), "duration year"))},
        {"months", Value(ParseInteger(match[2].str(), "duration month"))},
        {"days", Value(ParseInteger(match[3].str(), "duration day"))},
        {"hours", Value(ParseInteger(match[4].str(), "duration hour"))},
        {"minutes", Value(ParseInteger(match[5].str(), "duration minute"))},
        {"seconds", Value(static_cast<double>(
                        ParseDecimal(match[6].str(), "duration second")))}};
    return DurationFromMap(map);
  }
  if (!std::regex_match(input, match, kIso) || input == "P" || input == "PT") {
    InvalidTemporal("invalid duration");
  }
  const auto component = [&](std::size_t index) {
    return match[index].matched
               ? ParseDecimal(match[index].str(), "duration component")
               : 0.0L;
  };
  const long double total_months = component(1) * 12 + component(2);
  const std::int64_t months = static_cast<std::int64_t>(total_months);
  const long double fractional_month_seconds =
      (total_months - months) * kAverageMonthSeconds;
  const std::int64_t month_days =
      static_cast<std::int64_t>(fractional_month_seconds / kSecondsPerDay);
  long double seconds = fractional_month_seconds - month_days * kSecondsPerDay;
  const long double total_days = component(3) * 7 + component(4);
  const std::int64_t explicit_days = static_cast<std::int64_t>(total_days);
  const std::int64_t days = explicit_days + month_days;
  seconds += (total_days - explicit_days) * kSecondsPerDay;
  seconds += component(5) * 3600 + component(6) * 60 + component(7);
  return NormalizeDuration(months, days, seconds);
}

std::string FormatYear(int year) {
  std::ostringstream out;
  if (year < 0) {
    out << '-';
  } else if (year > 9999) {
    out << '+';
  }
  out << std::setw(4) << std::setfill('0')
      << std::abs(static_cast<std::int64_t>(year));
  return out.str();
}

std::string FormatFraction(int nanosecond) {
  if (nanosecond == 0) {
    return {};
  }
  std::ostringstream out;
  out << std::setw(9) << std::setfill('0') << std::abs(nanosecond);
  std::string digits = out.str();
  while (digits.back() == '0') {
    digits.pop_back();
  }
  return digits;
}

LocalDateTime UtcDateTime(std::chrono::system_clock::time_point now) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto day = std::chrono::floor<Days>(seconds);
  const auto time = seconds - day;
  const auto hours = std::chrono::duration_cast<std::chrono::hours>(time);
  const auto minutes =
      std::chrono::duration_cast<std::chrono::minutes>(time - hours);
  const auto whole_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(time - hours - minutes);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
  return {FromSysDays(SysDays{day.time_since_epoch()}),
          {static_cast<int>(hours.count()), static_cast<int>(minutes.count()),
           static_cast<int>(whole_seconds.count()),
           static_cast<int>(nanoseconds.count()), true}};
}

enum class TemporalShape { kDate, kTime, kDateTime };

struct TemporalOperand {
  TemporalShape shape = TemporalShape::kDate;
  Date date{1970, 1, 1};
  LocalTime time{0, 0, 0, 0, false};
  int offset_seconds = 0;
  std::string timezone;
  bool has_date = false;
  bool has_offset = false;
};

TemporalOperand ToTemporalOperand(const Value &value) {
  if (value.IsDate()) {
    return {.shape = TemporalShape::kDate,
            .date = value.AsDate(),
            .has_date = true};
  }
  if (value.IsLocalTime()) {
    return {.shape = TemporalShape::kTime, .time = value.AsLocalTime()};
  }
  if (value.IsTime()) {
    const Time &time = value.AsTime();
    return {.shape = TemporalShape::kTime,
            .time = time.local_time,
            .offset_seconds = time.utc_offset_seconds,
            .timezone = time.timezone,
            .has_offset = true};
  }
  if (value.IsLocalDateTime()) {
    const LocalDateTime &date_time = value.AsLocalDateTime();
    return {.shape = TemporalShape::kDateTime,
            .date = date_time.date,
            .time = date_time.time,
            .has_date = true};
  }
  if (value.IsDateTime()) {
    const DateTime &date_time = value.AsDateTime();
    return {.shape = TemporalShape::kDateTime,
            .date = date_time.local_date_time.date,
            .time = date_time.local_date_time.time,
            .offset_seconds = date_time.utc_offset_seconds,
            .timezone = date_time.timezone,
            .has_date = true,
            .has_offset = true};
  }
  InvalidTemporal("duration endpoints must be temporal values");
}

std::int64_t LocalSecondsSinceEpoch(const TemporalOperand &operand,
                                    bool include_date) {
  const std::int64_t days =
      include_date ? DaysFromCivil(operand.date.year, operand.date.month,
                                   operand.date.day)
                   : 0;
  return days * kSecondsPerDay + operand.time.hour * 3600 +
         operand.time.minute * 60 + operand.time.second;
}

std::string SharedTimezone(const TemporalOperand &left,
                           const TemporalOperand &right) {
  if (!left.timezone.empty()) return left.timezone;
  return right.timezone;
}

int EffectiveOffset(const TemporalOperand &operand,
                    const std::string &shared_timezone, const Date &anchor_date,
                    bool both_have_offsets) {
  if (!shared_timezone.empty()) {
    if (operand.has_offset) return operand.offset_seconds;
    return ZoneOffset(
        shared_timezone,
        {operand.has_date ? operand.date : anchor_date, operand.time});
  }
  return both_have_offsets ? operand.offset_seconds : 0;
}

struct NanosecondDifference {
  std::int64_t seconds = 0;
  int nanoseconds = 0;
};

NanosecondDifference DifferenceInSeconds(const TemporalOperand &left,
                                         const TemporalOperand &right) {
  const std::string timezone = SharedTimezone(left, right);
  const bool include_date = left.has_date && right.has_date;
  const Date anchor_date = left.has_date ? left.date : right.date;
  const bool both_have_offsets = left.has_offset && right.has_offset;
  std::int64_t seconds = LocalSecondsSinceEpoch(right, include_date) -
                         LocalSecondsSinceEpoch(left, include_date);
  if (left.shape != TemporalShape::kDate ||
      right.shape != TemporalShape::kDate) {
    seconds += EffectiveOffset(left, timezone, anchor_date, both_have_offsets) -
               EffectiveOffset(right, timezone, anchor_date, both_have_offsets);
  }
  int nanoseconds = right.time.nanosecond - left.time.nanosecond;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += kNanosecondsPerSecond;
  }
  return {seconds, nanoseconds};
}

Date AddMonthsClamped(const Date &date, std::int64_t months) {
  const std::int64_t month_index =
      static_cast<std::int64_t>(date.year) * 12 + date.month - 1 + months;
  std::int64_t year = month_index / 12;
  int month = static_cast<int>(month_index % 12) + 1;
  if (month <= 0) {
    month += 12;
    --year;
  }
  if (year < kMinimumYear || year > kMaximumYear) {
    InvalidTemporal("duration month difference is out of range");
  }
  return {static_cast<int>(year), month,
          std::min(date.day, DaysInMonth(static_cast<int>(year), month))};
}

std::int64_t DifferenceInMonths(const TemporalOperand &left,
                                const TemporalOperand &right) {
  if (!left.has_date || !right.has_date) {
    return 0;
  }
  std::int64_t months =
      (static_cast<std::int64_t>(right.date.year) - left.date.year) * 12 +
      right.date.month - left.date.month;
  TemporalOperand candidate = left;
  candidate.date = AddMonthsClamped(left.date, months);
  const NanosecondDifference remaining = DifferenceInSeconds(candidate, right);
  const int comparison =
      remaining.seconds < 0
          ? 1
          : (remaining.seconds > 0 || remaining.nanoseconds > 0 ? -1 : 0);
  if (months > 0 && comparison > 0) {
    --months;
  } else if (months < 0 && comparison < 0) {
    ++months;
  }
  return months;
}

Duration SecondsDuration(const NanosecondDifference &difference) {
  return {0, 0, difference.seconds, difference.nanoseconds};
}

Duration CalendarDuration(const TemporalOperand &left,
                          const TemporalOperand &right) {
  if (!left.has_date || !right.has_date) {
    return SecondsDuration(DifferenceInSeconds(left, right));
  }
  const std::int64_t months = DifferenceInMonths(left, right);
  TemporalOperand month_boundary = left;
  month_boundary.date = AddMonthsClamped(left.date, months);
  NanosecondDifference remainder = DifferenceInSeconds(month_boundary, right);
  const std::int64_t division_seconds =
      remainder.seconds < 0 && remainder.nanoseconds > 0 ? remainder.seconds + 1
                                                         : remainder.seconds;
  const std::int64_t days = division_seconds / kSecondsPerDay;
  remainder.seconds -= days * kSecondsPerDay;
  return {months, days, remainder.seconds, remainder.nanoseconds};
}

Value IntegerProperty(std::int64_t value) { return Value(value); }

std::optional<Value> DateProperty(const Date &date, std::string_view property) {
  const IsoWeek iso = IsoWeekComponents(date);
  const int ordinal = OrdinalDay(date);
  if (property == "year") return IntegerProperty(date.year);
  if (property == "quarter") return IntegerProperty((date.month - 1) / 3 + 1);
  if (property == "month") return IntegerProperty(date.month);
  if (property == "week") return IntegerProperty(iso.week);
  if (property == "weekYear") return IntegerProperty(iso.year);
  if (property == "day") return IntegerProperty(date.day);
  if (property == "ordinalDay") return IntegerProperty(ordinal);
  if (property == "weekDay" || property == "dayOfWeek") {
    return IntegerProperty(iso.weekday);
  }
  if (property == "dayOfQuarter") {
    const int first_month = ((date.month - 1) / 3) * 3 + 1;
    const Date first{date.year, first_month, 1};
    return IntegerProperty(
        static_cast<int>((ToSysDays(date) - ToSysDays(first)).count()) + 1);
  }
  return std::nullopt;
}

std::optional<Value> TimeProperty(const LocalTime &time,
                                  std::string_view property) {
  if (property == "hour") return IntegerProperty(time.hour);
  if (property == "minute") return IntegerProperty(time.minute);
  if (property == "second") return IntegerProperty(time.second);
  if (property == "millisecond")
    return IntegerProperty(time.nanosecond / 1'000'000);
  if (property == "microsecond")
    return IntegerProperty(time.nanosecond / 1'000);
  if (property == "nanosecond") return IntegerProperty(time.nanosecond);
  return std::nullopt;
}

std::optional<Value> OffsetProperty(int offset, const std::string &timezone,
                                    std::string_view property) {
  if (property == "timezone") {
    return Value(timezone.empty() ? FormatOffset(offset) : timezone);
  }
  if (property == "offset") return Value(FormatOffset(offset));
  if (property == "offsetMinutes") return IntegerProperty(offset / 60);
  if (property == "offsetSeconds") return IntegerProperty(offset);
  return std::nullopt;
}

}  // namespace

Value ConstructDate(const Value *argument,
                    std::chrono::system_clock::time_point now) {
  if (argument == nullptr) return Value(UtcDateTime(now).date);
  if (argument->IsNull()) return Value::Null();
  if (argument->IsDate()) return *argument;
  if (argument->IsString()) return Value(ParseDate(argument->AsString()));
  if (argument->IsMap()) return Value(DateFromMap(argument->AsMap()));
  if (argument->IsLocalDateTime() || argument->IsDateTime()) {
    return Value(DateFromTemporal(*argument));
  }
  return Value::Null();
}

Value ConstructLocalTime(const Value *argument,
                         std::chrono::system_clock::time_point now) {
  if (argument == nullptr) return Value(UtcDateTime(now).time);
  if (argument->IsNull()) return Value::Null();
  if (argument->IsLocalTime()) return *argument;
  if (argument->IsString()) return Value(ParseLocalTime(argument->AsString()));
  if (argument->IsMap()) return Value(LocalTimeFromMap(argument->AsMap()));
  if (argument->IsTime() || argument->IsLocalDateTime() ||
      argument->IsDateTime()) {
    return Value(TimeFromTemporal(*argument));
  }
  return Value::Null();
}

Value ConstructTime(const Value *argument,
                    std::chrono::system_clock::time_point now) {
  if (argument == nullptr) return Value(Time{UtcDateTime(now).time, 0, {}});
  if (argument->IsNull()) return Value::Null();
  if (argument->IsTime()) return *argument;
  if (argument->IsString()) {
    ParsedTime parsed = ParseOffsetTime(argument->AsString(), false);
    if (!parsed.timezone.empty()) {
      InvalidTemporal("named timezone requires a date");
    }
    return Value(Time{parsed.local_time, parsed.offset_seconds, {}});
  }
  if (argument->IsMap()) {
    const Value::Map &map = argument->AsMap();
    const SourceOffset source = SourceOffsetFromMap(map);
    std::string timezone =
        MapString(map, "timezone")
            .value_or(source.has_offset ? FormatOffset(source.seconds) : "Z");
    if (!NumericTimezone(timezone)) {
      InvalidTemporal("named timezone requires a date");
    }
    const int offset = ParseOffset(timezone);
    LocalDateTime local{
        {1970, 1, 1},
        BaseTimeFromMap(map).value_or(LocalTime{0, 0, 0, 0, false})};
    if (source.has_offset && source.seconds != offset) {
      local = ConvertOffset(local, source.seconds, offset);
    }
    return Value(
        Time{ApplyTimeComponents(map, local.time), offset, std::string{}});
  }
  if (argument->IsLocalTime()) {
    return Value(Time{argument->AsLocalTime(), 0, {}});
  }
  if (argument->IsLocalDateTime()) {
    return Value(Time{argument->AsLocalDateTime().time, 0, {}});
  }
  if (argument->IsDateTime()) {
    const DateTime &date_time = argument->AsDateTime();
    return Value(
        Time{date_time.local_date_time.time, date_time.utc_offset_seconds, {}});
  }
  return Value::Null();
}

Value ConstructLocalDateTime(const Value *argument,
                             std::chrono::system_clock::time_point now) {
  if (argument == nullptr) return Value(UtcDateTime(now));
  if (argument->IsNull()) return Value::Null();
  if (argument->IsLocalDateTime()) return *argument;
  if (argument->IsString()) {
    return Value(ParseLocalDateTime(argument->AsString()));
  }
  if (argument->IsMap()) {
    const Value::Map &map = argument->AsMap();
    return Value(LocalDateTime{DateFromMap(map), LocalTimeFromMap(map)});
  }
  if (argument->IsDateTime()) {
    return Value(argument->AsDateTime().local_date_time);
  }
  return Value::Null();
}

Value ConstructDateTime(const Value *argument,
                        std::chrono::system_clock::time_point now) {
  if (argument == nullptr) return Value(DateTime{UtcDateTime(now), 0, {}});
  if (argument->IsNull()) return Value::Null();
  if (argument->IsDateTime()) return *argument;
  if (argument->IsString()) return Value(ParseDateTime(argument->AsString()));
  if (argument->IsMap()) {
    const Value::Map &map = argument->AsMap();
    const SourceOffset source = SourceOffsetFromMap(map);
    const std::optional<Date> base_date = BaseDateFromMap(map);
    const std::optional<LocalTime> base_time = BaseTimeFromMap(map);
    LocalDateTime local{ApplyDateComponents(map, base_date),
                        ApplyTimeComponents(map, base_time)};

    const std::optional<std::string> specified_timezone =
        MapString(map, "timezone");
    std::string timezone = specified_timezone.value_or(source.timezone);
    int source_offset = source.seconds;
    if (source.has_offset && !NumericTimezone(source.timezone)) {
      source_offset = ZoneOffset(source.timezone, local);
    }
    int offset =
        source.has_offset ? source_offset : OffsetForTimezone(timezone, local);
    if (specified_timezone.has_value()) {
      const int target_offset =
          source.has_offset ? TargetOffsetForConversion(*specified_timezone,
                                                        local, source_offset)
                            : OffsetForTimezone(*specified_timezone, local);
      if (source.has_offset) {
        local = ConvertOffset(local, source_offset, target_offset);
      }
      offset = target_offset;
    }
    return Value(DateTime{local, offset, StoredTimezone(timezone)});
  }
  if (argument->IsLocalDateTime()) {
    return Value(DateTime{argument->AsLocalDateTime(), 0, {}});
  }
  return Value::Null();
}

Value ConstructDuration(const Value *argument) {
  if (argument == nullptr) InvalidTemporal("duration() requires an argument");
  if (argument->IsNull()) return Value::Null();
  if (argument->IsDuration()) return *argument;
  if (argument->IsString()) return Value(ParseDuration(argument->AsString()));
  if (argument->IsMap()) return Value(DurationFromMap(argument->AsMap()));
  return Value::Null();
}

Value DurationBetween(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) return Value::Null();
  return Value(
      CalendarDuration(ToTemporalOperand(left), ToTemporalOperand(right)));
}

Value DurationInMonths(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) return Value::Null();
  const TemporalOperand left_temporal = ToTemporalOperand(left);
  const TemporalOperand right_temporal = ToTemporalOperand(right);
  return Value(
      Duration{DifferenceInMonths(left_temporal, right_temporal), 0, 0, 0});
}

Value DurationInDays(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) return Value::Null();
  const TemporalOperand left_temporal = ToTemporalOperand(left);
  const TemporalOperand right_temporal = ToTemporalOperand(right);
  if (!left_temporal.has_date || !right_temporal.has_date) {
    return Value(Duration{});
  }
  const NanosecondDifference difference =
      DifferenceInSeconds(left_temporal, right_temporal);
  const std::int64_t whole_seconds =
      difference.seconds < 0 && difference.nanoseconds > 0
          ? difference.seconds + 1
          : difference.seconds;
  const std::int64_t days = whole_seconds / kSecondsPerDay;
  return Value(Duration{0, days, 0, 0});
}

Value DurationInSeconds(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) return Value::Null();
  return Value(SecondsDuration(
      DifferenceInSeconds(ToTemporalOperand(left), ToTemporalOperand(right))));
}

std::optional<Value> TemporalProperty(const Value &value,
                                      std::string_view property) {
  if (value.IsDate()) return DateProperty(value.AsDate(), property);
  if (value.IsLocalTime()) return TimeProperty(value.AsLocalTime(), property);
  if (value.IsTime()) {
    if (auto result = TimeProperty(value.AsTime().local_time, property)) {
      return result;
    }
    return OffsetProperty(value.AsTime().utc_offset_seconds,
                          value.AsTime().timezone, property);
  }
  if (value.IsLocalDateTime()) {
    if (auto result = DateProperty(value.AsLocalDateTime().date, property)) {
      return result;
    }
    return TimeProperty(value.AsLocalDateTime().time, property);
  }
  if (value.IsDateTime()) {
    const DateTime &date_time = value.AsDateTime();
    if (auto result = DateProperty(date_time.local_date_time.date, property)) {
      return result;
    }
    if (auto result = TimeProperty(date_time.local_date_time.time, property)) {
      return result;
    }
    if (auto result = OffsetProperty(date_time.utc_offset_seconds,
                                     date_time.timezone, property)) {
      return result;
    }
    const std::int64_t epoch_seconds =
        (ToSysDays(date_time.local_date_time.date) - SysDays{1970y / 1 / 1})
                .count() *
            kSecondsPerDay +
        date_time.local_date_time.time.hour * 3600 +
        date_time.local_date_time.time.minute * 60 +
        date_time.local_date_time.time.second - date_time.utc_offset_seconds;
    if (property == "epochSeconds") return IntegerProperty(epoch_seconds);
    if (property == "epochMillis") {
      return IntegerProperty(epoch_seconds * 1000 +
                             date_time.local_date_time.time.nanosecond /
                                 1'000'000);
    }
    return std::nullopt;
  }
  if (value.IsDuration()) {
    const Duration &duration = value.AsDuration();
    if (property == "years") return IntegerProperty(duration.months / 12);
    if (property == "quarters") return IntegerProperty(duration.months / 3);
    if (property == "months") return IntegerProperty(duration.months);
    if (property == "weeks") return IntegerProperty(duration.days / 7);
    if (property == "days") return IntegerProperty(duration.days);
    if (property == "hours") return IntegerProperty(duration.seconds / 3600);
    if (property == "minutes") return IntegerProperty(duration.seconds / 60);
    if (property == "seconds") return IntegerProperty(duration.seconds);
    if (property == "milliseconds") {
      return IntegerProperty(duration.seconds * 1000 +
                             duration.nanoseconds / 1'000'000);
    }
    if (property == "microseconds") {
      return IntegerProperty(duration.seconds * 1'000'000 +
                             duration.nanoseconds / 1'000);
    }
    if (property == "nanoseconds") {
      return IntegerProperty(duration.seconds * kNanosecondsPerSecond +
                             duration.nanoseconds);
    }
    if (property == "quartersOfYear") {
      return IntegerProperty((duration.months % 12) / 3);
    }
    if (property == "monthsOfQuarter") {
      return IntegerProperty(duration.months % 3);
    }
    if (property == "monthsOfYear") {
      return IntegerProperty(duration.months % 12);
    }
    if (property == "daysOfWeek") return IntegerProperty(duration.days % 7);
    if (property == "minutesOfHour") {
      return IntegerProperty((duration.seconds / 60) % 60);
    }
    if (property == "secondsOfMinute") {
      return IntegerProperty(duration.seconds % 60);
    }
    if (property == "millisecondsOfSecond") {
      return IntegerProperty(duration.nanoseconds / 1'000'000);
    }
    if (property == "microsecondsOfSecond") {
      return IntegerProperty(duration.nanoseconds / 1'000);
    }
    if (property == "nanosecondsOfSecond") {
      return IntegerProperty(duration.nanoseconds);
    }
  }
  return std::nullopt;
}

std::string FormatDate(const Date &date) {
  (void)ToSysDays(date);
  std::ostringstream out;
  out << FormatYear(date.year) << '-' << std::setw(2) << std::setfill('0')
      << date.month << '-' << std::setw(2) << date.day;
  return out.str();
}

std::string FormatLocalTime(const LocalTime &time) {
  std::ostringstream out;
  out << std::setw(2) << std::setfill('0') << time.hour << ':' << std::setw(2)
      << time.minute;
  if (time.has_seconds || time.second != 0 || time.nanosecond != 0) {
    out << ':' << std::setw(2) << time.second;
    const std::string fraction = FormatFraction(time.nanosecond);
    if (!fraction.empty()) out << '.' << fraction;
  }
  return out.str();
}

std::string FormatTime(const Time &time) {
  return FormatLocalTime(time.local_time) +
         FormatOffset(time.utc_offset_seconds) +
         (time.timezone.empty() ? "" : "[" + time.timezone + "]");
}

std::string FormatLocalDateTime(const LocalDateTime &date_time) {
  return FormatDate(date_time.date) + "T" + FormatLocalTime(date_time.time);
}

std::string FormatDateTime(const DateTime &date_time) {
  return FormatLocalDateTime(date_time.local_date_time) +
         FormatOffset(date_time.utc_offset_seconds) +
         (date_time.timezone.empty() ? "" : "[" + date_time.timezone + "]");
}

std::string FormatDuration(const Duration &duration) {
  std::ostringstream out;
  out << 'P';
  const std::int64_t years = duration.months / 12;
  const std::int64_t months = duration.months % 12;
  if (years != 0) out << years << 'Y';
  if (months != 0) out << months << 'M';
  if (duration.days != 0) out << duration.days << 'D';

  const bool negative_time = duration.seconds < 0;
  std::int64_t seconds =
      negative_time ? -(duration.seconds + (duration.nanoseconds > 0 ? 1 : 0))
                    : duration.seconds;
  const int nanoseconds =
      negative_time && duration.nanoseconds > 0
          ? static_cast<int>(kNanosecondsPerSecond) - duration.nanoseconds
          : duration.nanoseconds;
  const std::int64_t hours = seconds / 3600;
  seconds %= 3600;
  const std::int64_t minutes = seconds / 60;
  seconds %= 60;
  if (hours != 0 || minutes != 0 || seconds != 0 || duration.nanoseconds != 0 ||
      (years == 0 && months == 0 && duration.days == 0)) {
    out << 'T';
    const char *sign = negative_time ? "-" : "";
    if (hours != 0) out << sign << hours << 'H';
    if (minutes != 0) out << sign << minutes << 'M';
    if (seconds != 0 || nanoseconds != 0 || (hours == 0 && minutes == 0)) {
      out << sign << seconds;
      const std::string fraction = FormatFraction(nanoseconds);
      if (!fraction.empty()) out << '.' << fraction;
      out << 'S';
    }
  }
  return out.str();
}

}  // namespace rg
