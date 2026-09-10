#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bolt/graph.h"
#include "bolt/path.h"
#include "value.h"

namespace common {
struct Node {
  int64_t id;
  std::unordered_set<std::string> labels;
  std::unordered_map<std::string, Value> properties;
  std::string ToString() const;
  bolt::Node ToBolt() const;
};

struct Relationship {
  int64_t id;
  int64_t src;
  int64_t dst;
  std::string type;
  std::unordered_map<std::string, Value> properties;
  std::string ToString() const;
  bolt::Relationship ToBolt() const;
  bolt::RelNode ToBoltUnbound() const;
};

struct PathElement {
  bool is_node;
  std::any data;
};

struct Path {
  std::vector<PathElement> data;
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] bolt::InternalPath ToBolt() const;
};

enum class ResultType : char {
  Value = 0,
  Node = 1,
  Relationship = 2,
  Path = 3
};

struct Result {
  std::any data;
  ResultType type;
  [[nodiscard]] std::string ToString() const;
  [[nodiscard]] std::any ToBolt() const;
};

}  // namespace common

template <>
struct fmt::formatter<std::vector<common::Result>> {
  template <typename ParseContext>
  constexpr auto parse(ParseContext& ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const std::vector<common::Result>& vec, FormatContext& ctx) {
    auto out = ctx.out();
    out = fmt::format_to(out, "[");
    for (auto it = vec.begin(); it != vec.end(); ++it) {
      if (it != vec.begin()) {
        out = fmt::format_to(out, ", ");
      }
      out = fmt::format_to(out, "{}", it->ToString());
    }
    out = fmt::format_to(out, "]");
    return out;
  }
};