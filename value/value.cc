#include "value/value.h"

#include <cassert>
#include <sstream>

namespace rg {

namespace {

bool PtrEqual(const Value::NodePtr &left, const Value::NodePtr &right) {
  if (!left || !right) {
    return left == right;
  }
  return *left == *right;
}

bool PtrEqual(const Value::RelationshipPtr &left,
              const Value::RelationshipPtr &right) {
  if (!left || !right) {
    return left == right;
  }
  return *left == *right;
}

void AppendValue(std::ostringstream &oss, const Value &value);

void AppendStringList(std::ostringstream &oss,
                      const std::vector<std::string> &items) {
  oss << '[';
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << items[i];
  }
  oss << ']';
}

void AppendValueList(std::ostringstream &oss, const Value::List &items) {
  oss << '[';
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    AppendValue(oss, items[i]);
  }
  oss << ']';
}

void AppendValueMap(std::ostringstream &oss, const Value::Map &items) {
  oss << '{';
  bool first = true;
  for (const auto &entry : items) {
    if (!first) {
      oss << ", ";
    }
    first = false;
    oss << entry.first << ": ";
    AppendValue(oss, entry.second);
  }
  oss << '}';
}

void AppendNode(std::ostringstream &oss, const Node &node) {
  oss << "Node{id=" << node.id << ", labels=";
  AppendStringList(oss, node.labels);
  oss << ", properties=";
  AppendValueMap(oss, node.properties);
  oss << '}';
}

void AppendRelationship(std::ostringstream &oss, const Relationship &rel) {
  oss << "Relationship{id=" << rel.id << ", start=" << rel.start_node_id
      << ", end=" << rel.end_node_id << ", type=" << rel.type
      << ", properties=";
  AppendValueMap(oss, rel.properties);
  oss << '}';
}

void AppendPath(std::ostringstream &oss, const Path &path) {
  oss << "Path{nodes=";
  oss << '[';
  for (size_t i = 0; i < path.nodes.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    if (path.nodes[i]) {
      AppendNode(oss, *path.nodes[i]);
    } else {
      oss << "null";
    }
  }
  oss << "], relationships=";
  oss << '[';
  for (size_t i = 0; i < path.relationships.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    if (path.relationships[i]) {
      AppendRelationship(oss, *path.relationships[i]);
    } else {
      oss << "null";
    }
  }
  oss << "]}";
}

void AppendValue(std::ostringstream &oss, const Value &value) {
  switch (value.type()) {
    case ValueType::kNull:
      oss << "null";
      break;
    case ValueType::kBool:
      oss << (value.as_bool() ? "true" : "false");
      break;
    case ValueType::kInteger:
      oss << value.as_integer();
      break;
    case ValueType::kDouble:
      oss << value.as_double();
      break;
    case ValueType::kString:
      oss << '"' << value.as_string() << '"';
      break;
    case ValueType::kList:
      AppendValueList(oss, value.as_list());
      break;
    case ValueType::kMap:
      AppendValueMap(oss, value.as_map());
      break;
    case ValueType::kNode:
      AppendNode(oss, value.as_node());
      break;
    case ValueType::kRelationship:
      AppendRelationship(oss, value.as_relationship());
      break;
    case ValueType::kPath:
      AppendPath(oss, value.as_path());
      break;
    case ValueType::kDate: {
      const auto &date = value.as_date();
      oss << "Date{" << date.year << '-' << date.month << '-' << date.day
          << '}';
      break;
    }
    case ValueType::kLocalTime: {
      const auto &local_time = value.as_local_time();
      oss << "LocalTime{" << local_time.hour << ':' << local_time.minute << ':'
          << local_time.second << '.' << local_time.nanosecond << '}';
      break;
    }
    case ValueType::kTime: {
      const auto &time = value.as_time();
      oss << "Time{" << time.local_time.hour << ':' << time.local_time.minute
          << ':' << time.local_time.second << '.' << time.local_time.nanosecond
          << ", offset=" << time.utc_offset_seconds << '}';
      break;
    }
    case ValueType::kLocalDateTime: {
      const auto &dt = value.as_local_date_time();
      oss << "LocalDateTime{" << dt.date.year << '-' << dt.date.month << '-'
          << dt.date.day << 'T' << dt.time.hour << ':' << dt.time.minute << ':'
          << dt.time.second << '.' << dt.time.nanosecond << '}';
      break;
    }
    case ValueType::kDateTime: {
      const auto &dt = value.as_date_time();
      oss << "DateTime{" << dt.local_date_time.date.year << '-'
          << dt.local_date_time.date.month << '-' << dt.local_date_time.date.day
          << 'T' << dt.local_date_time.time.hour << ':'
          << dt.local_date_time.time.minute << ':'
          << dt.local_date_time.time.second << '.'
          << dt.local_date_time.time.nanosecond
          << ", offset=" << dt.utc_offset_seconds << '}';
      break;
    }
    case ValueType::kDuration: {
      const auto &duration = value.as_duration();
      oss << "Duration{months=" << duration.months << ", days=" << duration.days
          << ", seconds=" << duration.seconds
          << ", nanoseconds=" << duration.nanoseconds << '}';
      break;
    }
    case ValueType::kPoint: {
      const auto &point = value.as_point();
      oss << "Point{srid=" << point.srid << ", coordinates=";
      oss << '[';
      for (size_t i = 0; i < point.coordinates.size(); ++i) {
        if (i > 0) {
          oss << ", ";
        }
        oss << point.coordinates[i];
      }
      oss << "]}";
      break;
    }
  }
}

}  // namespace

Value::Value() : storage_(std::monostate{}) {}

Value Value::Null() { return {}; }

Value::Value(bool value) : storage_(value) {}

Value::Value(int value) : storage_(static_cast<int64_t>(value)) {}

Value::Value(int64_t value) : storage_(value) {}

Value::Value(double value) : storage_(value) {}

Value::Value(std::string value) : storage_(std::move(value)) {}

Value::Value(const char *value) {
  assert(value != nullptr);
  storage_ = (value != nullptr) ? std::string(value) : std::string();
}

Value::Value(List value) : storage_(std::move(value)) {}

Value::Value(Map value) : storage_(std::move(value)) {}

Value::Value(NodePtr value) : storage_(std::move(value)) {
  assert(std::get<NodePtr>(storage_) != nullptr);
}

Value::Value(RelationshipPtr value) : storage_(std::move(value)) {
  assert(std::get<RelationshipPtr>(storage_) != nullptr);
}

Value::Value(PathPtr value) : storage_(std::move(value)) {
  assert(std::get<PathPtr>(storage_) != nullptr);
}

Value::Value(Date value) : storage_(value) {}

Value::Value(LocalTime value) : storage_(value) {}

Value::Value(Time value) : storage_(value) {}

Value::Value(LocalDateTime value) : storage_(value) {}

Value::Value(DateTime value) : storage_(value) {}

Value::Value(Duration value) : storage_(value) {}

Value::Value(Point value) : storage_(std::move(value)) {}

ValueType Value::type() const {
  if (std::holds_alternative<std::monostate>(storage_)) {
    return ValueType::kNull;
  }
  if (std::holds_alternative<bool>(storage_)) {
    return ValueType::kBool;
  }
  if (std::holds_alternative<int64_t>(storage_)) {
    return ValueType::kInteger;
  }
  if (std::holds_alternative<double>(storage_)) {
    return ValueType::kDouble;
  }
  if (std::holds_alternative<std::string>(storage_)) {
    return ValueType::kString;
  }
  if (std::holds_alternative<List>(storage_)) {
    return ValueType::kList;
  }
  if (std::holds_alternative<Map>(storage_)) {
    return ValueType::kMap;
  }
  if (std::holds_alternative<NodePtr>(storage_)) {
    return ValueType::kNode;
  }
  if (std::holds_alternative<RelationshipPtr>(storage_)) {
    return ValueType::kRelationship;
  }
  if (std::holds_alternative<PathPtr>(storage_)) {
    return ValueType::kPath;
  }
  if (std::holds_alternative<Date>(storage_)) {
    return ValueType::kDate;
  }
  if (std::holds_alternative<LocalTime>(storage_)) {
    return ValueType::kLocalTime;
  }
  if (std::holds_alternative<Time>(storage_)) {
    return ValueType::kTime;
  }
  if (std::holds_alternative<LocalDateTime>(storage_)) {
    return ValueType::kLocalDateTime;
  }
  if (std::holds_alternative<DateTime>(storage_)) {
    return ValueType::kDateTime;
  }
  if (std::holds_alternative<Duration>(storage_)) {
    return ValueType::kDuration;
  }
  return ValueType::kPoint;
}

bool Value::is_null() const {
  return std::holds_alternative<std::monostate>(storage_);
}

bool Value::is_bool() const { return std::holds_alternative<bool>(storage_); }

bool Value::is_integer() const {
  return std::holds_alternative<int64_t>(storage_);
}

bool Value::is_double() const {
  return std::holds_alternative<double>(storage_);
}

bool Value::is_string() const {
  return std::holds_alternative<std::string>(storage_);
}

bool Value::is_list() const { return std::holds_alternative<List>(storage_); }

bool Value::is_map() const { return std::holds_alternative<Map>(storage_); }

bool Value::is_node() const {
  return std::holds_alternative<NodePtr>(storage_);
}

bool Value::is_relationship() const {
  return std::holds_alternative<RelationshipPtr>(storage_);
}

bool Value::is_path() const {
  return std::holds_alternative<PathPtr>(storage_);
}

bool Value::is_date() const { return std::holds_alternative<Date>(storage_); }

bool Value::is_local_time() const {
  return std::holds_alternative<LocalTime>(storage_);
}

bool Value::is_time() const { return std::holds_alternative<Time>(storage_); }

bool Value::is_local_date_time() const {
  return std::holds_alternative<LocalDateTime>(storage_);
}

bool Value::is_date_time() const {
  return std::holds_alternative<DateTime>(storage_);
}

bool Value::is_duration() const {
  return std::holds_alternative<Duration>(storage_);
}

bool Value::is_point() const { return std::holds_alternative<Point>(storage_); }

bool Value::as_bool() const {
  assert(is_bool());
  return std::get<bool>(storage_);
}

int64_t Value::as_integer() const {
  assert(is_integer());
  return std::get<int64_t>(storage_);
}

double Value::as_double() const {
  assert(is_double());
  return std::get<double>(storage_);
}

const std::string &Value::as_string() const {
  assert(is_string());
  return std::get<std::string>(storage_);
}

const Value::List &Value::as_list() const {
  assert(is_list());
  return std::get<List>(storage_);
}

Value::List &Value::as_list() {
  assert(is_list());
  return std::get<List>(storage_);
}

const Value::Map &Value::as_map() const {
  assert(is_map());
  return std::get<Map>(storage_);
}

Value::Map &Value::as_map() {
  assert(is_map());
  return std::get<Map>(storage_);
}

const Node &Value::as_node() const {
  assert(is_node());
  const auto &ptr = std::get<NodePtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

Node &Value::as_node() {
  assert(is_node());
  auto &ptr = std::get<NodePtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

const Relationship &Value::as_relationship() const {
  assert(is_relationship());
  const auto &ptr = std::get<RelationshipPtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

Relationship &Value::as_relationship() {
  assert(is_relationship());
  auto &ptr = std::get<RelationshipPtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

const Path &Value::as_path() const {
  assert(is_path());
  const auto &ptr = std::get<PathPtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

Path &Value::as_path() {
  assert(is_path());
  auto &ptr = std::get<PathPtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

const Date &Value::as_date() const {
  assert(is_date());
  return std::get<Date>(storage_);
}

const LocalTime &Value::as_local_time() const {
  assert(is_local_time());
  return std::get<LocalTime>(storage_);
}

const Time &Value::as_time() const {
  assert(is_time());
  return std::get<Time>(storage_);
}

const LocalDateTime &Value::as_local_date_time() const {
  assert(is_local_date_time());
  return std::get<LocalDateTime>(storage_);
}

const DateTime &Value::as_date_time() const {
  assert(is_date_time());
  return std::get<DateTime>(storage_);
}

const Duration &Value::as_duration() const {
  assert(is_duration());
  return std::get<Duration>(storage_);
}

const Point &Value::as_point() const {
  assert(is_point());
  return std::get<Point>(storage_);
}

std::string Value::ToString() const {
  std::ostringstream oss;
  AppendValue(oss, *this);
  return oss.str();
}

bool Value::operator==(const Value &other) const {
  if (type() != other.type()) {
    return false;
  }
  switch (type()) {
    case ValueType::kNull:
      return true;
    case ValueType::kBool:
      return as_bool() == other.as_bool();
    case ValueType::kInteger:
      return as_integer() == other.as_integer();
    case ValueType::kDouble:
      return as_double() == other.as_double();
    case ValueType::kString:
      return as_string() == other.as_string();
    case ValueType::kList:
      return as_list() == other.as_list();
    case ValueType::kMap:
      return as_map() == other.as_map();
    case ValueType::kNode: {
      const auto &left = std::get<NodePtr>(storage_);
      const auto &right = std::get<NodePtr>(other.storage_);
      return PtrEqual(left, right);
    }
    case ValueType::kRelationship: {
      const auto &left = std::get<RelationshipPtr>(storage_);
      const auto &right = std::get<RelationshipPtr>(other.storage_);
      return PtrEqual(left, right);
    }
    case ValueType::kPath:
      return as_path() == other.as_path();
    case ValueType::kDate:
      return as_date() == other.as_date();
    case ValueType::kLocalTime:
      return as_local_time() == other.as_local_time();
    case ValueType::kTime:
      return as_time() == other.as_time();
    case ValueType::kLocalDateTime:
      return as_local_date_time() == other.as_local_date_time();
    case ValueType::kDateTime:
      return as_date_time() == other.as_date_time();
    case ValueType::kDuration:
      return as_duration() == other.as_duration();
    case ValueType::kPoint:
      return as_point() == other.as_point();
  }
  return false;
}

bool Value::operator!=(const Value &other) const { return !(*this == other); }

bool operator==(const Node &left, const Node &right) {
  return left.id == right.id && left.labels == right.labels &&
         left.properties == right.properties;
}

bool operator==(const Relationship &left, const Relationship &right) {
  return left.id == right.id && left.start_node_id == right.start_node_id &&
         left.end_node_id == right.end_node_id && left.type == right.type &&
         left.properties == right.properties;
}

bool operator==(const Path &left, const Path &right) {
  if (left.nodes.size() != right.nodes.size()) {
    return false;
  }
  if (left.relationships.size() != right.relationships.size()) {
    return false;
  }
  for (size_t i = 0; i < left.nodes.size(); ++i) {
    if (!PtrEqual(left.nodes[i], right.nodes[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < left.relationships.size(); ++i) {
    if (!PtrEqual(left.relationships[i], right.relationships[i])) {
      return false;
    }
  }
  return true;
}

bool operator==(const Date &left, const Date &right) {
  return left.year == right.year && left.month == right.month &&
         left.day == right.day;
}

bool operator==(const LocalTime &left, const LocalTime &right) {
  return left.hour == right.hour && left.minute == right.minute &&
         left.second == right.second && left.nanosecond == right.nanosecond;
}

bool operator==(const Time &left, const Time &right) {
  return left.local_time == right.local_time &&
         left.utc_offset_seconds == right.utc_offset_seconds;
}

bool operator==(const LocalDateTime &left, const LocalDateTime &right) {
  return left.date == right.date && left.time == right.time;
}

bool operator==(const DateTime &left, const DateTime &right) {
  return left.local_date_time == right.local_date_time &&
         left.utc_offset_seconds == right.utc_offset_seconds;
}

bool operator==(const Duration &left, const Duration &right) {
  return left.months == right.months && left.days == right.days &&
         left.seconds == right.seconds && left.nanoseconds == right.nanoseconds;
}

bool operator==(const Point &left, const Point &right) {
  return left.srid == right.srid && left.coordinates == right.coordinates;
}

}  // namespace rg
