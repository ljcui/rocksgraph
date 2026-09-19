#pragma once

#include <boost/endian/conversion.hpp>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/byte_utils.h"

namespace graphdb {

// GraphDB IDs are native integers in memory. These helpers define the stable
// big-endian representation used only at persistent key/value boundaries.
template <std::integral T>
void AppendBigEndianId(std::string& output, T id) {
  static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));
  const T encoded = boost::endian::native_to_big(id);
  output.append(common::AsChars(encoded), sizeof(encoded));
}

template <std::integral T>
[[nodiscard]] std::string EncodeBigEndianId(T id) {
  std::string encoded;
  encoded.reserve(sizeof(id));
  AppendBigEndianId(encoded, id);
  return encoded;
}

template <std::integral T>
[[nodiscard]] T ReadBigEndianId(const char* data) {
  static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));
  return boost::endian::big_to_native(common::ReadValue<T>(data));
}

template <std::integral T>
[[nodiscard]] T ReadBigEndianId(std::string_view encoded) {
  return ReadBigEndianId<T>(encoded.data());
}

}  // namespace graphdb
