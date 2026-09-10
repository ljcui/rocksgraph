#include "graphdb/value_codec.h"

#include <cstdint>
#include <type_traits>

#include "common/byte_utils.h"
#include "common/exceptions.h"

namespace graphdb {
namespace {

enum class StoredValueType : std::uint8_t {
  kNull,
  kBool,
  kInteger,
  kDouble,
  kString,
  kList,
  kDate,
  kLocalTime,
  kTime,
  kLocalDateTime,
  kDateTime,
  kDuration,
  kPoint,
};

template <typename T>
void Append(std::string* output, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  output->append(common::AsChars(value), sizeof(value));
}

void AppendString(std::string* output, std::string_view value) {
  Append(output, static_cast<std::uint64_t>(value.size()));
  output->append(value);
}

class Decoder {
 public:
  explicit Decoder(std::string_view input) : input_(input) {}

  template <typename T>
  T Read() {
    static_assert(std::is_trivially_copyable_v<T>);
    Require(sizeof(T));
    const T value = common::ReadValue<T>(input_.data());
    input_.remove_prefix(sizeof(T));
    return value;
  }

  std::string ReadString() {
    const std::uint64_t size = Read<std::uint64_t>();
    if (size > input_.size()) {
      THROW_CODE(StorageEngineError,
                 "stored value contains a truncated string");
    }
    std::string value(input_.substr(0, static_cast<std::size_t>(size)));
    input_.remove_prefix(static_cast<std::size_t>(size));
    return value;
  }

  [[nodiscard]] bool Empty() const { return input_.empty(); }
  [[nodiscard]] std::size_t Remaining() const { return input_.size(); }

 private:
  void Require(std::size_t size) const {
    if (input_.size() < size) {
      THROW_CODE(StorageEngineError, "stored value is truncated");
    }
  }

  std::string_view input_;
};

void EncodeValue(std::string* output, const rg::Value& value) {
  switch (value.Type()) {
    case rg::ValueType::kNull:
      Append(output, StoredValueType::kNull);
      return;
    case rg::ValueType::kBool:
      Append(output, StoredValueType::kBool);
      Append(output, static_cast<std::uint8_t>(value.AsBool()));
      return;
    case rg::ValueType::kInteger:
      Append(output, StoredValueType::kInteger);
      Append(output, value.AsInteger());
      return;
    case rg::ValueType::kDouble:
      Append(output, StoredValueType::kDouble);
      Append(output, value.AsDouble());
      return;
    case rg::ValueType::kString:
      Append(output, StoredValueType::kString);
      AppendString(output, value.AsString());
      return;
    case rg::ValueType::kList: {
      Append(output, StoredValueType::kList);
      const auto& list = value.AsList();
      Append(output, static_cast<std::uint64_t>(list.size()));
      for (const auto& item : list) {
        EncodeValue(output, item);
      }
      return;
    }
    case rg::ValueType::kDate: {
      Append(output, StoredValueType::kDate);
      const auto& date = value.AsDate();
      Append(output, date.year);
      Append(output, date.month);
      Append(output, date.day);
      return;
    }
    case rg::ValueType::kLocalTime: {
      Append(output, StoredValueType::kLocalTime);
      const auto& time = value.AsLocalTime();
      Append(output, time.hour);
      Append(output, time.minute);
      Append(output, time.second);
      Append(output, time.nanosecond);
      Append(output, static_cast<std::uint8_t>(time.has_seconds));
      return;
    }
    case rg::ValueType::kTime: {
      Append(output, StoredValueType::kTime);
      const auto& time = value.AsTime();
      EncodeValue(output, rg::Value(time.local_time));
      Append(output, time.utc_offset_seconds);
      AppendString(output, time.timezone);
      return;
    }
    case rg::ValueType::kLocalDateTime: {
      Append(output, StoredValueType::kLocalDateTime);
      const auto& date_time = value.AsLocalDateTime();
      EncodeValue(output, rg::Value(date_time.date));
      EncodeValue(output, rg::Value(date_time.time));
      return;
    }
    case rg::ValueType::kDateTime: {
      Append(output, StoredValueType::kDateTime);
      const auto& date_time = value.AsDateTime();
      EncodeValue(output, rg::Value(date_time.local_date_time));
      Append(output, date_time.utc_offset_seconds);
      AppendString(output, date_time.timezone);
      return;
    }
    case rg::ValueType::kDuration: {
      Append(output, StoredValueType::kDuration);
      const auto& duration = value.AsDuration();
      Append(output, duration.months);
      Append(output, duration.days);
      Append(output, duration.seconds);
      Append(output, duration.nanoseconds);
      return;
    }
    case rg::ValueType::kPoint: {
      Append(output, StoredValueType::kPoint);
      const auto& point = value.AsPoint();
      Append(output, point.srid);
      Append(output, static_cast<std::uint64_t>(point.coordinates.size()));
      for (double coordinate : point.coordinates) {
        Append(output, coordinate);
      }
      return;
    }
    case rg::ValueType::kMap:
    case rg::ValueType::kNode:
    case rg::ValueType::kRelationship:
    case rg::ValueType::kPath:
      THROW_CODE(ValueException,
                 "value type {} cannot be stored as a graph property",
                 static_cast<int>(value.Type()));
  }
  THROW_CODE(ValueException, "unknown graph property value type");
}

rg::LocalTime DecodeLocalTime(Decoder* decoder) {
  return {.hour = decoder->Read<std::int32_t>(),
          .minute = decoder->Read<std::int32_t>(),
          .second = decoder->Read<std::int32_t>(),
          .nanosecond = decoder->Read<std::int32_t>(),
          .has_seconds = decoder->Read<std::uint8_t>() != 0};
}

rg::Value DecodeValue(Decoder* decoder) {
  const auto type = decoder->Read<StoredValueType>();
  switch (type) {
    case StoredValueType::kNull:
      return rg::Value::Null();
    case StoredValueType::kBool:
      return rg::Value(decoder->Read<std::uint8_t>() != 0);
    case StoredValueType::kInteger:
      return rg::Value(decoder->Read<std::int64_t>());
    case StoredValueType::kDouble:
      return rg::Value(decoder->Read<double>());
    case StoredValueType::kString:
      return rg::Value(decoder->ReadString());
    case StoredValueType::kList: {
      const std::uint64_t size = decoder->Read<std::uint64_t>();
      if (size > static_cast<std::uint64_t>(decoder->Remaining())) {
        THROW_CODE(StorageEngineError, "stored list is too large");
      }
      rg::Value::List list;
      list.reserve(static_cast<std::size_t>(size));
      for (std::uint64_t i = 0; i < size; ++i) {
        list.push_back(DecodeValue(decoder));
      }
      return rg::Value(std::move(list));
    }
    case StoredValueType::kDate:
      return rg::Value(rg::Date{decoder->Read<std::int32_t>(),
                                decoder->Read<std::int32_t>(),
                                decoder->Read<std::int32_t>()});
    case StoredValueType::kLocalTime:
      return rg::Value(DecodeLocalTime(decoder));
    case StoredValueType::kTime: {
      rg::Value local_time = DecodeValue(decoder);
      if (!local_time.IsLocalTime()) {
        THROW_CODE(StorageEngineError, "stored time has an invalid local time");
      }
      return rg::Value(rg::Time{local_time.AsLocalTime(),
                                decoder->Read<std::int32_t>(),
                                decoder->ReadString()});
    }
    case StoredValueType::kLocalDateTime: {
      rg::Value date = DecodeValue(decoder);
      rg::Value time = DecodeValue(decoder);
      if (!date.IsDate() || !time.IsLocalTime()) {
        THROW_CODE(StorageEngineError,
                   "stored local datetime has invalid components");
      }
      return rg::Value(rg::LocalDateTime{date.AsDate(), time.AsLocalTime()});
    }
    case StoredValueType::kDateTime: {
      rg::Value local = DecodeValue(decoder);
      if (!local.IsLocalDateTime()) {
        THROW_CODE(StorageEngineError,
                   "stored datetime has an invalid local datetime");
      }
      return rg::Value(rg::DateTime{local.AsLocalDateTime(),
                                    decoder->Read<std::int32_t>(),
                                    decoder->ReadString()});
    }
    case StoredValueType::kDuration:
      return rg::Value(rg::Duration{
          decoder->Read<std::int64_t>(), decoder->Read<std::int64_t>(),
          decoder->Read<std::int64_t>(), decoder->Read<std::int32_t>()});
    case StoredValueType::kPoint: {
      rg::Point point;
      point.srid = decoder->Read<std::int32_t>();
      const std::uint64_t size = decoder->Read<std::uint64_t>();
      if (size >
          static_cast<std::uint64_t>(decoder->Remaining() / sizeof(double))) {
        THROW_CODE(StorageEngineError, "stored point is too large");
      }
      point.coordinates.reserve(static_cast<std::size_t>(size));
      for (std::uint64_t i = 0; i < size; ++i) {
        point.coordinates.push_back(decoder->Read<double>());
      }
      return rg::Value(std::move(point));
    }
  }
  THROW_CODE(StorageEngineError, "stored value has an unknown type tag");
}

}  // namespace

std::string SerializeValue(const rg::Value& value) {
  std::string output;
  EncodeValue(&output, value);
  return output;
}

rg::Value DeserializeValue(std::string_view data) {
  if (data.empty()) {
    THROW_CODE(StorageEngineError, "stored value is empty");
  }
  Decoder decoder(data);
  rg::Value value = DecodeValue(&decoder);
  if (!decoder.Empty()) {
    THROW_CODE(StorageEngineError, "stored value contains trailing bytes");
  }
  return value;
}

}  // namespace graphdb
