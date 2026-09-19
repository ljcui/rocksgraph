#include "graphdb/id_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace graphdb {
namespace {

TEST(IdCodec, EncodesStableBigEndianBytes) {
  EXPECT_EQ(EncodeBigEndianId<int64_t>(1),
            std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8));
  EXPECT_EQ(EncodeBigEndianId<int64_t>(255),
            std::string("\x00\x00\x00\x00\x00\x00\x00\xFF", 8));
  EXPECT_EQ(EncodeBigEndianId<int64_t>(256),
            std::string("\x00\x00\x00\x00\x00\x00\x01\x00", 8));
}

TEST(IdCodec, RoundTripsSupportedIdWidths) {
  const auto encoded_entity =
      EncodeBigEndianId(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(ReadBigEndianId<int64_t>(encoded_entity),
            std::numeric_limits<int64_t>::max());

  const auto encoded_token =
      EncodeBigEndianId(std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(ReadBigEndianId<uint32_t>(encoded_token),
            std::numeric_limits<uint32_t>::max());
}

TEST(IdCodec, PositiveIdsSortByNumericValue) {
  EXPECT_LT(EncodeBigEndianId<int64_t>(1), EncodeBigEndianId<int64_t>(255));
  EXPECT_LT(EncodeBigEndianId<int64_t>(255), EncodeBigEndianId<int64_t>(256));
}

}  // namespace
}  // namespace graphdb
