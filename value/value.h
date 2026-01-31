#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace rg {

struct Node;
struct Relationship;
struct Path;

struct Date {
  int32_t year = 0;
  int32_t month = 0;
  int32_t day = 0;
};

struct LocalTime {
  int32_t hour = 0;
  int32_t minute = 0;
  int32_t second = 0;
  int32_t nanosecond = 0;
};

struct Time {
  LocalTime local_time;
  int32_t utc_offset_seconds = 0;
};

struct LocalDateTime {
  Date date;
  LocalTime time;
};

struct DateTime {
  LocalDateTime local_date_time;
  int32_t utc_offset_seconds = 0;
};

struct Duration {
  int64_t months = 0;
  int64_t days = 0;
  int64_t seconds = 0;
  int32_t nanoseconds = 0;
};

struct Point {
  int32_t srid = 0;
  std::vector<double> coordinates;
};

enum class ValueType {
  kNull,
  kBool,
  kInteger,
  kDouble,
  kString,
  kList,
  kMap,
  kNode,
  kRelationship,
  kPath,
  kDate,
  kLocalTime,
  kTime,
  kLocalDateTime,
  kDateTime,
  kDuration,
  kPoint,
};

class Value {
 public:
  using List = std::vector<Value>;
  using Map = std::map<std::string, Value>;
  using NodePtr = std::shared_ptr<Node>;
  using RelationshipPtr = std::shared_ptr<Relationship>;
  using PathPtr = std::shared_ptr<Path>;

  Value();
  static Value Null();

  explicit Value(bool value);
  explicit Value(int value);
  explicit Value(int64_t value);
  explicit Value(double value);
  explicit Value(std::string value);
  explicit Value(const char *value);
  explicit Value(List value);
  explicit Value(Map value);
  explicit Value(NodePtr value);
  explicit Value(RelationshipPtr value);
  explicit Value(PathPtr value);
  explicit Value(Date value);
  explicit Value(LocalTime value);
  explicit Value(Time value);
  explicit Value(LocalDateTime value);
  explicit Value(DateTime value);
  explicit Value(Duration value);
  explicit Value(Point value);

  ValueType type() const;

  bool is_null() const;
  bool is_bool() const;
  bool is_integer() const;
  bool is_double() const;
  bool is_string() const;
  bool is_list() const;
  bool is_map() const;
  bool is_node() const;
  bool is_relationship() const;
  bool is_path() const;
  bool is_date() const;
  bool is_local_time() const;
  bool is_time() const;
  bool is_local_date_time() const;
  bool is_date_time() const;
  bool is_duration() const;
  bool is_point() const;

  bool as_bool() const;
  int64_t as_integer() const;
  double as_double() const;
  const std::string &as_string() const;
  const List &as_list() const;
  List &as_list();
  const Map &as_map() const;
  Map &as_map();

  const Node &as_node() const;
  Node &as_node();
  const Relationship &as_relationship() const;
  Relationship &as_relationship();
  const Path &as_path() const;
  Path &as_path();

  const Date &as_date() const;
  const LocalTime &as_local_time() const;
  const Time &as_time() const;
  const LocalDateTime &as_local_date_time() const;
  const DateTime &as_date_time() const;
  const Duration &as_duration() const;
  const Point &as_point() const;

  std::string ToString() const;

  bool operator==(const Value &other) const;
  bool operator!=(const Value &other) const;

 private:
  using Storage = std::variant<std::monostate, bool, int64_t, double, std::string,
                               List, Map, NodePtr, RelationshipPtr, PathPtr,
                               Date, LocalTime, Time, LocalDateTime, DateTime,
                               Duration, Point>;
  Storage storage_;
};

struct Node {
  int64_t id = 0;
  std::vector<std::string> labels;
  Value::Map properties;
};

struct Relationship {
  int64_t id = 0;
  int64_t start_node_id = 0;
  int64_t end_node_id = 0;
  std::string type;
  Value::Map properties;
};

struct Path {
  std::vector<Value::NodePtr> nodes;
  std::vector<Value::RelationshipPtr> relationships;
};

bool operator==(const Node &left, const Node &right);
bool operator==(const Relationship &left, const Relationship &right);
bool operator==(const Path &left, const Path &right);

bool operator==(const Date &left, const Date &right);
bool operator==(const LocalTime &left, const LocalTime &right);
bool operator==(const Time &left, const Time &right);
bool operator==(const LocalDateTime &left, const LocalDateTime &right);
bool operator==(const DateTime &left, const DateTime &right);
bool operator==(const Duration &left, const Duration &right);
bool operator==(const Point &left, const Point &right);

}  // namespace rg
