#include <gtest/gtest.h>

#include <boost/endian/conversion.hpp>

#include "common/exceptions.h"
#include "graphdb/value_codec.h"
#include "value/value.h"

using graphdb::DeserializeValue;
using graphdb::SerializeValue;
using rg::Value;

TEST(Value, basic) {
  Value v1(Value::Map{{"key1", Value(1)}, {"key2", Value(2.1)}});
  Value v2(Value::Map{{"key3", Value(true)}, {"key4", Value("dddd")}});
  Value v3(Value::List{Value(100), Value("str"), v1, v2});
  EXPECT_EQ(
      v3.ToString(),
      R"([100, "str", {key1: 1, key2: 2.1}, {key3: true, key4: "dddd"}])");
}

TEST(Value, storageCodecRoundTrip) {
  const std::vector<Value> values = {
      Value::Null(),
      Value(true),
      Value(std::int64_t{-42}),
      Value(3.5),
      Value("text"),
      Value(Value::List{Value(1), Value("two"), Value::Null()}),
      Value(rg::Date{2026, 9, 10}),
      Value(rg::LocalTime{12, 34, 56, 789, true}),
      Value(rg::Time{{12, 34, 56, 789, true}, 28'800, "Asia/Shanghai"}),
      Value(rg::LocalDateTime{{2026, 9, 10}, {12, 34, 56, 789, true}}),
      Value(rg::DateTime{
          {{2026, 9, 10}, {12, 34, 56, 789, true}}, 28'800, "Asia/Shanghai"}),
      Value(rg::Duration{14, 3, 4, 5}),
      Value(rg::Point{4326, {120.1, 30.2}}),
  };

  for (const auto& value : values) {
    EXPECT_EQ(DeserializeValue(SerializeValue(value)), value);
  }
}

TEST(Value, storageCodecRejectsInvalidData) {
  EXPECT_THROW(DeserializeValue({}), LgraphException);

  std::string encoded = SerializeValue(Value(1));
  encoded.push_back('\0');
  EXPECT_THROW(DeserializeValue(encoded), LgraphException);

  EXPECT_THROW(SerializeValue(Value(Value::Map{{"key", Value(1)}})),
               LgraphException);
}

TEST(Endian, big) {
  int64_t first = 0;
  std::string last, now;
  auto big = boost::endian::native_to_big(first);
  last.append((const char*)&big, sizeof(big));
  for (int64_t i = 1; i < 10000000; i++) {
    auto c = boost::endian::native_to_big(i);
    now.append((const char*)&c, sizeof(c));
    EXPECT_TRUE(now > last);
    last = now;
    now.clear();
  }
  int a = 1;
  int b = -1;
  auto big_a = boost::endian::native_to_big(a);
  auto big_b = boost::endian::native_to_big(b);
  std::string a_str, b_str;
  a_str.append((const char*)&big_a, sizeof(big_a));
  b_str.append((const char*)&big_b, sizeof(big_b));
  EXPECT_TRUE(a_str < b_str);
}
