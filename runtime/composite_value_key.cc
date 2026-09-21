#include "runtime/composite_value_key.h"

#include <functional>

namespace rg::slotted {

namespace {

std::size_t HashCombine(std::size_t seed, std::size_t value) noexcept {
  constexpr std::size_t kMagic =
      static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
  return seed ^ (value + kMagic + (seed << 6U) + (seed >> 2U));
}

}  // namespace

bool CompositeValueKey::operator==(
    const CompositeValueKey &other) const noexcept {
  return CompositeValueKeyEqual{}(*this, other);
}

std::size_t CompositeValueKeyHash::operator()(
    const CompositeValueKey &key) const noexcept {
  std::size_t seed = std::hash<std::size_t>{}(key.values.size());
  for (const Value &value : key.values) {
    seed = HashCombine(seed, ValueHash{}(value));
  }
  return seed;
}

bool CompositeValueKeyEqual::operator()(
    const CompositeValueKey &left,
    const CompositeValueKey &right) const noexcept {
  if (left.values.size() != right.values.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.values.size(); ++index) {
    if (!ValueEqual{}(left.values[index], right.values[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace rg::slotted
