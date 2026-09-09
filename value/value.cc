#include "value/value.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>

#include "value/temporal.h"

namespace rg {

bool ValueLess(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) {
    return !left.IsNull() && right.IsNull();
  }
  if ((left.IsInteger() || left.IsDouble()) &&
      (right.IsInteger() || right.IsDouble())) {
    if (left.IsInteger() && right.IsInteger()) {
      return left.AsInteger() < right.AsInteger();
    }
    if (left.IsDouble() && right.IsDouble()) {
      return left.AsDouble() < right.AsDouble();
    }
    const Value &integer = left.IsInteger() ? left : right;
    const Value &floating = left.IsDouble() ? left : right;
    const double number = floating.AsDouble();
    bool integer_less = false;
    if (!std::isnan(number)) {
      if (number >=
          static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        integer_less = true;
      } else if (number >= static_cast<double>(
                               std::numeric_limits<std::int64_t>::min())) {
        const auto truncated = static_cast<std::int64_t>(number);
        integer_less = integer.AsInteger() < truncated ||
                       (integer.AsInteger() == truncated &&
                        static_cast<double>(truncated) < number);
      }
    }
    if (left.IsInteger()) {
      return integer_less;
    }
    return !std::isnan(number) && !ValuesEqual(left, right) && !integer_less;
  }
  if (left.Type() != right.Type()) {
    return static_cast<int>(left.Type()) < static_cast<int>(right.Type());
  }
  if (left.IsString()) {
    return left.AsString() < right.AsString();
  }
  if (left.IsBool()) {
    return !left.AsBool() && right.AsBool();
  }
  if (left.IsNode()) {
    return left.AsNode().id < right.AsNode().id;
  }
  if (left.IsRelationship()) {
    return left.AsRelationship().id < right.AsRelationship().id;
  }
  return left.ToString() < right.ToString();
}

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

bool DoubleAsInteger(double value, std::int64_t *integer) noexcept {
  if (!std::isfinite(value) || std::trunc(value) != value ||
      value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  *integer = static_cast<std::int64_t>(value);
  return true;
}

bool NumericEqual(const Value &left, const Value &right,
                  bool nan_equal) noexcept {
  if (left.IsInteger() && right.IsInteger()) {
    return left.AsInteger() == right.AsInteger();
  }
  if (left.IsDouble() && right.IsDouble()) {
    const double lhs = left.AsDouble();
    const double rhs = right.AsDouble();
    return lhs == rhs || (nan_equal && std::isnan(lhs) && std::isnan(rhs));
  }
  const Value &integer = left.IsInteger() ? left : right;
  const Value &floating = left.IsDouble() ? left : right;
  std::int64_t converted = 0;
  return DoubleAsInteger(floating.AsDouble(), &converted) &&
         integer.AsInteger() == converted;
}

std::size_t HashCombine(std::size_t seed, std::size_t value) noexcept {
  constexpr std::size_t kMagic =
      static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
  return seed ^ (value + kMagic + (seed << 6U) + (seed >> 2U));
}

std::size_t HashTag(ValueType type) noexcept {
  return std::hash<int>{}(static_cast<int>(type));
}

std::size_t HashInteger(std::int64_t value) noexcept {
  return std::hash<std::int64_t>{}(value);
}

std::size_t HashDouble(double value) noexcept {
  if (std::isnan(value)) {
    return static_cast<std::size_t>(0x7ff8000000000000ULL);
  }
  if (value == 0.0) {
    value = 0.0;
  }
  return std::hash<double>{}(value);
}

std::size_t HashNumber(const Value &value) noexcept {
  std::int64_t integer = 0;
  if (value.IsInteger()) {
    integer = value.AsInteger();
  } else if (!DoubleAsInteger(value.AsDouble(), &integer)) {
    return HashCombine(HashTag(ValueType::kInteger),
                       HashDouble(value.AsDouble()));
  }
  return HashCombine(HashTag(ValueType::kInteger), HashInteger(integer));
}

template <typename Integer>
void AddIntegerHash(std::size_t *seed, Integer value) noexcept {
  *seed = HashCombine(*seed, std::hash<Integer>{}(value));
}

void AddLocalTimeHash(std::size_t *seed, const LocalTime &time) noexcept {
  AddIntegerHash(seed, time.hour);
  AddIntegerHash(seed, time.minute);
  AddIntegerHash(seed, time.second);
  AddIntegerHash(seed, time.nanosecond);
}

template <typename Entity>
bool EntityIdentityEqual(const std::shared_ptr<Entity> &left,
                         const std::shared_ptr<Entity> &right) noexcept {
  if (!left || !right) {
    return left == right;
  }
  return left->id == right->id;
}

template <typename Entity>
void AddEntityIdentityHash(std::size_t *seed,
                           const std::shared_ptr<Entity> &entity) noexcept {
  *seed = HashCombine(*seed, entity ? HashInteger(entity->id) : 0U);
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
  switch (value.Type()) {
    case ValueType::kNull:
      oss << "null";
      break;
    case ValueType::kBool:
      oss << (value.AsBool() ? "true" : "false");
      break;
    case ValueType::kInteger:
      oss << value.AsInteger();
      break;
    case ValueType::kDouble:
      oss << value.AsDouble();
      break;
    case ValueType::kString:
      oss << '"' << value.AsString() << '"';
      break;
    case ValueType::kList:
      AppendValueList(oss, value.AsList());
      break;
    case ValueType::kMap:
      AppendValueMap(oss, value.AsMap());
      break;
    case ValueType::kNode:
      AppendNode(oss, value.AsNode());
      break;
    case ValueType::kRelationship:
      AppendRelationship(oss, value.AsRelationship());
      break;
    case ValueType::kPath:
      AppendPath(oss, value.AsPath());
      break;
    case ValueType::kDate: {
      oss << FormatDate(value.AsDate());
      break;
    }
    case ValueType::kLocalTime: {
      oss << FormatLocalTime(value.AsLocalTime());
      break;
    }
    case ValueType::kTime: {
      oss << FormatTime(value.AsTime());
      break;
    }
    case ValueType::kLocalDateTime: {
      oss << FormatLocalDateTime(value.AsLocalDateTime());
      break;
    }
    case ValueType::kDateTime: {
      oss << FormatDateTime(value.AsDateTime());
      break;
    }
    case ValueType::kDuration: {
      oss << FormatDuration(value.AsDuration());
      break;
    }
    case ValueType::kPoint: {
      const auto &point = value.AsPoint();
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

ValueType Value::Type() const {
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

bool Value::IsNull() const {
  return std::holds_alternative<std::monostate>(storage_);
}

bool Value::IsBool() const { return std::holds_alternative<bool>(storage_); }

bool Value::IsInteger() const {
  return std::holds_alternative<int64_t>(storage_);
}

bool Value::IsDouble() const {
  return std::holds_alternative<double>(storage_);
}

bool Value::IsString() const {
  return std::holds_alternative<std::string>(storage_);
}

bool Value::IsList() const { return std::holds_alternative<List>(storage_); }

bool Value::IsMap() const { return std::holds_alternative<Map>(storage_); }

bool Value::IsNode() const { return std::holds_alternative<NodePtr>(storage_); }

bool Value::IsRelationship() const {
  return std::holds_alternative<RelationshipPtr>(storage_);
}

bool Value::IsPath() const { return std::holds_alternative<PathPtr>(storage_); }

bool Value::IsDate() const { return std::holds_alternative<Date>(storage_); }

bool Value::IsLocalTime() const {
  return std::holds_alternative<LocalTime>(storage_);
}

bool Value::IsTime() const { return std::holds_alternative<Time>(storage_); }

bool Value::IsLocalDateTime() const {
  return std::holds_alternative<LocalDateTime>(storage_);
}

bool Value::IsDateTime() const {
  return std::holds_alternative<DateTime>(storage_);
}

bool Value::IsDuration() const {
  return std::holds_alternative<Duration>(storage_);
}

bool Value::IsPoint() const { return std::holds_alternative<Point>(storage_); }

bool Value::AsBool() const {
  assert(IsBool());
  return std::get<bool>(storage_);
}

int64_t Value::AsInteger() const {
  assert(IsInteger());
  return std::get<int64_t>(storage_);
}

double Value::AsDouble() const {
  assert(IsDouble());
  return std::get<double>(storage_);
}

const std::string &Value::AsString() const {
  assert(IsString());
  return std::get<std::string>(storage_);
}

const Value::List &Value::AsList() const {
  assert(IsList());
  return std::get<List>(storage_);
}

Value::List &Value::AsList() {
  assert(IsList());
  return std::get<List>(storage_);
}

const Value::Map &Value::AsMap() const {
  assert(IsMap());
  return std::get<Map>(storage_);
}

Value::Map &Value::AsMap() {
  assert(IsMap());
  return std::get<Map>(storage_);
}

const Node &Value::AsNode() const {
  assert(IsNode());
  const auto &ptr = std::get<NodePtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

const Relationship &Value::AsRelationship() const {
  assert(IsRelationship());
  const auto &ptr = std::get<RelationshipPtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

const Path &Value::AsPath() const {
  assert(IsPath());
  const auto &ptr = std::get<PathPtr>(storage_);
  assert(ptr != nullptr);
  return *ptr;
}

const Date &Value::AsDate() const {
  assert(IsDate());
  return std::get<Date>(storage_);
}

const LocalTime &Value::AsLocalTime() const {
  assert(IsLocalTime());
  return std::get<LocalTime>(storage_);
}

const Time &Value::AsTime() const {
  assert(IsTime());
  return std::get<Time>(storage_);
}

const LocalDateTime &Value::AsLocalDateTime() const {
  assert(IsLocalDateTime());
  return std::get<LocalDateTime>(storage_);
}

const DateTime &Value::AsDateTime() const {
  assert(IsDateTime());
  return std::get<DateTime>(storage_);
}

const Duration &Value::AsDuration() const {
  assert(IsDuration());
  return std::get<Duration>(storage_);
}

const Point &Value::AsPoint() const {
  assert(IsPoint());
  return std::get<Point>(storage_);
}

std::string Value::ToString() const {
  std::ostringstream oss;
  AppendValue(oss, *this);
  return oss.str();
}

bool Value::operator==(const Value &other) const {
  if (Type() != other.Type()) {
    return false;
  }
  switch (Type()) {
    case ValueType::kNull:
      return true;
    case ValueType::kBool:
      return AsBool() == other.AsBool();
    case ValueType::kInteger:
      return AsInteger() == other.AsInteger();
    case ValueType::kDouble:
      return AsDouble() == other.AsDouble();
    case ValueType::kString:
      return AsString() == other.AsString();
    case ValueType::kList:
      return AsList() == other.AsList();
    case ValueType::kMap:
      return AsMap() == other.AsMap();
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
      return AsPath() == other.AsPath();
    case ValueType::kDate:
      return AsDate() == other.AsDate();
    case ValueType::kLocalTime:
      return AsLocalTime() == other.AsLocalTime();
    case ValueType::kTime:
      return AsTime() == other.AsTime();
    case ValueType::kLocalDateTime:
      return AsLocalDateTime() == other.AsLocalDateTime();
    case ValueType::kDateTime:
      return AsDateTime() == other.AsDateTime();
    case ValueType::kDuration:
      return AsDuration() == other.AsDuration();
    case ValueType::kPoint:
      return AsPoint() == other.AsPoint();
  }
  return false;
}

bool Value::operator!=(const Value &other) const { return !(*this == other); }

bool ValuesEqual(const Value &left, const Value &right) {
  const bool left_numeric = left.IsInteger() || left.IsDouble();
  const bool right_numeric = right.IsInteger() || right.IsDouble();
  if (left_numeric && right_numeric) {
    return NumericEqual(left, right, false);
  }
  if (left.Type() != right.Type()) {
    return false;
  }
  if (left.IsNode()) {
    return left.AsNode().id == right.AsNode().id;
  }
  if (left.IsRelationship()) {
    return left.AsRelationship().id == right.AsRelationship().id;
  }
  if (left.IsList()) {
    if (left.AsList().size() != right.AsList().size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.AsList().size(); ++index) {
      if (!ValuesEqual(left.AsList()[index], right.AsList()[index])) {
        return false;
      }
    }
    return true;
  }
  if (left.IsMap()) {
    if (left.AsMap().size() != right.AsMap().size()) {
      return false;
    }
    for (const auto &[key, value] : left.AsMap()) {
      const auto found = right.AsMap().find(key);
      if (found == right.AsMap().end() || !ValuesEqual(value, found->second)) {
        return false;
      }
    }
    return true;
  }
  return left == right;
}

bool CompositeValueKey::operator==(
    const CompositeValueKey &other) const noexcept {
  return ValueEqual{}(*this, other);
}

bool ValueEqual::operator()(const Value &left,
                            const Value &right) const noexcept {
  const bool left_numeric = left.IsInteger() || left.IsDouble();
  const bool right_numeric = right.IsInteger() || right.IsDouble();
  if (left_numeric && right_numeric) {
    return NumericEqual(left, right, true);
  }
  if (left.Type() != right.Type()) {
    return false;
  }
  switch (left.Type()) {
    case ValueType::kNull:
      return true;
    case ValueType::kBool:
      return left.AsBool() == right.AsBool();
    case ValueType::kInteger:
    case ValueType::kDouble:
      return NumericEqual(left, right, true);
    case ValueType::kString:
      return left.AsString() == right.AsString();
    case ValueType::kList: {
      const auto &lhs = left.AsList();
      const auto &rhs = right.AsList();
      if (lhs.size() != rhs.size()) {
        return false;
      }
      for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!(*this)(lhs[index], rhs[index])) {
          return false;
        }
      }
      return true;
    }
    case ValueType::kMap: {
      const auto &lhs = left.AsMap();
      const auto &rhs = right.AsMap();
      if (lhs.size() != rhs.size()) {
        return false;
      }
      auto left_item = lhs.begin();
      auto right_item = rhs.begin();
      while (left_item != lhs.end()) {
        if (left_item->first != right_item->first ||
            !(*this)(left_item->second, right_item->second)) {
          return false;
        }
        ++left_item;
        ++right_item;
      }
      return true;
    }
    case ValueType::kNode:
      return left.AsNode().id == right.AsNode().id;
    case ValueType::kRelationship:
      return left.AsRelationship().id == right.AsRelationship().id;
    case ValueType::kPath: {
      const Path &lhs = left.AsPath();
      const Path &rhs = right.AsPath();
      if (lhs.nodes.size() != rhs.nodes.size() ||
          lhs.relationships.size() != rhs.relationships.size()) {
        return false;
      }
      for (std::size_t index = 0; index < lhs.nodes.size(); ++index) {
        if (!EntityIdentityEqual(lhs.nodes[index], rhs.nodes[index])) {
          return false;
        }
      }
      for (std::size_t index = 0; index < lhs.relationships.size(); ++index) {
        if (!EntityIdentityEqual(lhs.relationships[index],
                                 rhs.relationships[index])) {
          return false;
        }
      }
      return true;
    }
    case ValueType::kDate:
      return left.AsDate() == right.AsDate();
    case ValueType::kLocalTime:
      return left.AsLocalTime() == right.AsLocalTime();
    case ValueType::kTime:
      return left.AsTime() == right.AsTime();
    case ValueType::kLocalDateTime:
      return left.AsLocalDateTime() == right.AsLocalDateTime();
    case ValueType::kDateTime:
      return left.AsDateTime() == right.AsDateTime();
    case ValueType::kDuration:
      return left.AsDuration() == right.AsDuration();
    case ValueType::kPoint: {
      const Point &lhs = left.AsPoint();
      const Point &rhs = right.AsPoint();
      if (lhs.srid != rhs.srid ||
          lhs.coordinates.size() != rhs.coordinates.size()) {
        return false;
      }
      for (std::size_t index = 0; index < lhs.coordinates.size(); ++index) {
        const double left_coordinate = lhs.coordinates[index];
        const double right_coordinate = rhs.coordinates[index];
        if (left_coordinate != right_coordinate &&
            !(std::isnan(left_coordinate) && std::isnan(right_coordinate))) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

bool ValueEqual::operator()(const CompositeValueKey &left,
                            const CompositeValueKey &right) const noexcept {
  if (left.values.size() != right.values.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.values.size(); ++index) {
    if (!(*this)(left.values[index], right.values[index])) {
      return false;
    }
  }
  return true;
}

std::size_t ValueHash::operator()(const Value &value) const noexcept {
  if (value.IsInteger() || value.IsDouble()) {
    return HashNumber(value);
  }

  std::size_t seed = HashTag(value.Type());
  switch (value.Type()) {
    case ValueType::kNull:
      return seed;
    case ValueType::kBool:
      return HashCombine(seed, std::hash<bool>{}(value.AsBool()));
    case ValueType::kInteger:
    case ValueType::kDouble:
      return HashNumber(value);
    case ValueType::kString:
      return HashCombine(seed, std::hash<std::string>{}(value.AsString()));
    case ValueType::kList:
      AddIntegerHash(&seed, value.AsList().size());
      for (const Value &item : value.AsList()) {
        seed = HashCombine(seed, (*this)(item));
      }
      return seed;
    case ValueType::kMap:
      AddIntegerHash(&seed, value.AsMap().size());
      for (const auto &[name, item] : value.AsMap()) {
        seed = HashCombine(seed, std::hash<std::string>{}(name));
        seed = HashCombine(seed, (*this)(item));
      }
      return seed;
    case ValueType::kNode:
      return HashCombine(seed, HashInteger(value.AsNode().id));
    case ValueType::kRelationship:
      return HashCombine(seed, HashInteger(value.AsRelationship().id));
    case ValueType::kPath: {
      const Path &path = value.AsPath();
      AddIntegerHash(&seed, path.nodes.size());
      for (const auto &node : path.nodes) {
        AddEntityIdentityHash(&seed, node);
      }
      AddIntegerHash(&seed, path.relationships.size());
      for (const auto &relationship : path.relationships) {
        AddEntityIdentityHash(&seed, relationship);
      }
      return seed;
    }
    case ValueType::kDate: {
      const Date &date = value.AsDate();
      AddIntegerHash(&seed, date.year);
      AddIntegerHash(&seed, date.month);
      AddIntegerHash(&seed, date.day);
      return seed;
    }
    case ValueType::kLocalTime:
      AddLocalTimeHash(&seed, value.AsLocalTime());
      return seed;
    case ValueType::kTime: {
      const Time &time = value.AsTime();
      AddLocalTimeHash(&seed, time.local_time);
      AddIntegerHash(&seed, time.utc_offset_seconds);
      return HashCombine(seed, std::hash<std::string>{}(time.timezone));
    }
    case ValueType::kLocalDateTime: {
      const LocalDateTime &date_time = value.AsLocalDateTime();
      AddIntegerHash(&seed, date_time.date.year);
      AddIntegerHash(&seed, date_time.date.month);
      AddIntegerHash(&seed, date_time.date.day);
      AddLocalTimeHash(&seed, date_time.time);
      return seed;
    }
    case ValueType::kDateTime: {
      const DateTime &date_time = value.AsDateTime();
      AddIntegerHash(&seed, date_time.local_date_time.date.year);
      AddIntegerHash(&seed, date_time.local_date_time.date.month);
      AddIntegerHash(&seed, date_time.local_date_time.date.day);
      AddLocalTimeHash(&seed, date_time.local_date_time.time);
      AddIntegerHash(&seed, date_time.utc_offset_seconds);
      return HashCombine(seed, std::hash<std::string>{}(date_time.timezone));
    }
    case ValueType::kDuration: {
      const Duration &duration = value.AsDuration();
      AddIntegerHash(&seed, duration.months);
      AddIntegerHash(&seed, duration.days);
      AddIntegerHash(&seed, duration.seconds);
      AddIntegerHash(&seed, duration.nanoseconds);
      return seed;
    }
    case ValueType::kPoint: {
      const Point &point = value.AsPoint();
      AddIntegerHash(&seed, point.srid);
      AddIntegerHash(&seed, point.coordinates.size());
      for (double coordinate : point.coordinates) {
        seed = HashCombine(seed, HashDouble(coordinate));
      }
      return seed;
    }
  }
  return seed;
}

std::size_t ValueHash::operator()(const CompositeValueKey &key) const noexcept {
  std::size_t seed = std::hash<std::size_t>{}(key.values.size());
  for (const Value &value : key.values) {
    seed = HashCombine(seed, (*this)(value));
  }
  return seed;
}

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
         left.utc_offset_seconds == right.utc_offset_seconds &&
         left.timezone == right.timezone;
}

bool operator==(const LocalDateTime &left, const LocalDateTime &right) {
  return left.date == right.date && left.time == right.time;
}

bool operator==(const DateTime &left, const DateTime &right) {
  return left.local_date_time == right.local_date_time &&
         left.utc_offset_seconds == right.utc_offset_seconds &&
         left.timezone == right.timezone;
}

bool operator==(const Duration &left, const Duration &right) {
  return left.months == right.months && left.days == right.days &&
         left.seconds == right.seconds && left.nanoseconds == right.nanoseconds;
}

bool operator==(const Point &left, const Point &right) {
  return left.srid == right.srid && left.coordinates == right.coordinates;
}

}  // namespace rg
