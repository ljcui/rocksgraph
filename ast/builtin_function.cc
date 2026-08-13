#include "ast/builtin_function.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace ast {
namespace {

std::string LowerAscii(std::string_view input) {
  std::string out(input);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

std::string ArgumentCount(std::size_t count) {
  return std::to_string(count) + (count == 1 ? " argument" : " arguments");
}

}  // namespace

const std::vector<BuiltinFunction> &BuiltinFunctions() {
  static const std::vector<BuiltinFunction> kFunctions = {
      {.kind = BuiltinFunctionKind::kAverage,
       .name = "avg",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1,
       .aggregate = true,
       .allows_distinct = true},
      {.kind = BuiltinFunctionKind::kCollect,
       .name = "collect",
       .result_type = SemanticVariableType::kList,
       .minimum_argument_count = 1,
       .maximum_argument_count = 1,
       .aggregate = true,
       .allows_distinct = true},
      {.kind = BuiltinFunctionKind::kCount,
       .name = "count",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1,
       .aggregate = true,
       .allows_distinct = true},
      {.kind = BuiltinFunctionKind::kMaximum,
       .name = "max",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1,
       .aggregate = true,
       .allows_distinct = true},
      {.kind = BuiltinFunctionKind::kMinimum,
       .name = "min",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1,
       .aggregate = true,
       .allows_distinct = true},
      {.kind = BuiltinFunctionKind::kSum,
       .name = "sum",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1,
       .aggregate = true,
       .allows_distinct = true},
      {.kind = BuiltinFunctionKind::kCoalesce,
       .name = "coalesce",
       .minimum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kId,
       .name = "id",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kIsEmpty,
       .name = "isEmpty",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kKeys,
       .name = "keys",
       .result_type = SemanticVariableType::kList,
       .list_element_type = SemanticVariableType::kScalar,
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kLabels,
       .name = "labels",
       .result_type = SemanticVariableType::kList,
       .list_element_type = SemanticVariableType::kScalar,
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kLength,
       .name = "length",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kNodes,
       .name = "nodes",
       .result_type = SemanticVariableType::kList,
       .list_element_type = SemanticVariableType::kNode,
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kProperties,
       .name = "properties",
       .result_type = SemanticVariableType::kMap,
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kRange,
       .name = "range",
       .result_type = SemanticVariableType::kList,
       .list_element_type = SemanticVariableType::kScalar,
       .minimum_argument_count = 2,
       .maximum_argument_count = 3},
      {.kind = BuiltinFunctionKind::kRelationships,
       .name = "relationships",
       .result_type = SemanticVariableType::kList,
       .list_element_type = SemanticVariableType::kRelationship,
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kSize,
       .name = "size",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kSplit,
       .name = "split",
       .result_type = SemanticVariableType::kList,
       .list_element_type = SemanticVariableType::kScalar,
       .minimum_argument_count = 2,
       .maximum_argument_count = 2},
      {.kind = BuiltinFunctionKind::kToBoolean,
       .name = "toBoolean",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kToFloat,
       .name = "toFloat",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kToInteger,
       .name = "toInteger",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kToLower,
       .name = "toLower",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kToString,
       .name = "toString",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kToUpper,
       .name = "toUpper",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kTrim,
       .name = "trim",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
      {.kind = BuiltinFunctionKind::kType,
       .name = "type",
       .minimum_argument_count = 1,
       .maximum_argument_count = 1},
  };
  return kFunctions;
}

const BuiltinFunction *FindBuiltinFunction(std::string_view name) {
  const std::string normalized = LowerAscii(name);
  for (const auto &function : BuiltinFunctions()) {
    if (LowerAscii(function.name) == normalized) {
      return &function;
    }
  }
  return nullptr;
}

bool BuiltinFunctionAcceptsArgumentCount(const BuiltinFunction &function,
                                         std::size_t argument_count) {
  return argument_count >= function.minimum_argument_count &&
         (!function.maximum_argument_count.has_value() ||
          argument_count <= *function.maximum_argument_count);
}

std::string BuiltinFunctionArgumentCountError(const BuiltinFunction &function) {
  std::string expected;
  if (!function.maximum_argument_count.has_value()) {
    expected = "at least " + ArgumentCount(function.minimum_argument_count);
  } else if (function.minimum_argument_count ==
             *function.maximum_argument_count) {
    expected = ArgumentCount(function.minimum_argument_count);
  } else {
    expected = std::to_string(function.minimum_argument_count) + " to " +
               ArgumentCount(*function.maximum_argument_count);
  }
  return function.name + "() expects " + expected;
}

}  // namespace ast
