#include "bolt/hydrator.h"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <functional>
#include <string>

#include "bolt/spatial.h"
#include "bolt/temporal.h"
#include "bolt/to_string.h"

namespace {

std::any Hydrate(std::function<void(bolt::Packer&)> pack) {
  std::string encoded;
  bolt::Packer packer;
  packer.Begin(&encoded);
  pack(packer);
  EXPECT_FALSE(packer.Err().has_value());

  bolt::Unpacker unpacker;
  unpacker.Reset(encoded);
  unpacker.Next();
  return bolt::ServerHydrator(unpacker);
}

TEST(BoltHydratorTest, DecodesTemporalAndSpatialParameters) {
  const std::any date = Hydrate([](bolt::Packer& packer) {
    packer.StructHeader('D', 1);
    packer.Int64(19'000);
  });
  ASSERT_EQ(date.type(), typeid(bolt::Date));
  EXPECT_EQ(std::any_cast<bolt::Date>(date).days, 19'000);

  const std::any duration = Hydrate([](bolt::Packer& packer) {
    packer.StructHeader('E', 4);
    packer.Int64(1);
    packer.Int64(2);
    packer.Int64(3);
    packer.Int64(4);
  });
  ASSERT_EQ(duration.type(), typeid(bolt::Duration));
  const auto parsed_duration = std::any_cast<bolt::Duration>(duration);
  EXPECT_EQ(parsed_duration.months, 1);
  EXPECT_EQ(parsed_duration.days, 2);
  EXPECT_EQ(parsed_duration.seconds, 3);
  EXPECT_EQ(parsed_duration.nanos, 4);

  const std::any point = Hydrate([](bolt::Packer& packer) {
    packer.StructHeader('X', 3);
    packer.Int64(4326);
    packer.Double(1.5);
    packer.Double(2.5);
  });
  ASSERT_EQ(point.type(), typeid(bolt::Point2D));
  const auto parsed_point = std::any_cast<bolt::Point2D>(point);
  EXPECT_EQ(parsed_point.spatialRefId, 4326U);
  EXPECT_DOUBLE_EQ(parsed_point.x, 1.5);
  EXPECT_DOUBLE_EQ(parsed_point.y, 2.5);
}

TEST(BoltHydratorTest, KeepsBytesDistinctFromStrings) {
  const std::any bytes =
      Hydrate([](bolt::Packer& packer) { packer.Bytes("abc"); });
  ASSERT_EQ(bytes.type(), typeid(bolt::ByteArray));
  EXPECT_EQ(std::any_cast<bolt::ByteArray>(bytes).value, "abc");
  EXPECT_EQ(bolt::Print(bytes), "<bytes:3>");
}

}  // namespace
