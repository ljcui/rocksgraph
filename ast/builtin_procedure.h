#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ast/semantic_table.h"

namespace ast {

enum class BuiltinProcedureKind {
  kLabels,
  kPropertyKeys,
  kRelationshipTypes,
  kProcedures,
};

struct BuiltinProcedureYield {
  std::string name;
  SemanticVariableType type = SemanticVariableType::kUnknown;
};

struct BuiltinProcedure {
  BuiltinProcedureKind kind = BuiltinProcedureKind::kLabels;
  std::string name;
  std::string signature;
  std::string description;
  std::vector<BuiltinProcedureYield> yields;
  std::size_t argument_count = 0;
  bool read_only = true;
  bool works_on_system = false;
};

[[nodiscard]] const std::vector<BuiltinProcedure> &BuiltinProcedures();
[[nodiscard]] const BuiltinProcedure *FindBuiltinProcedure(
    std::string_view name);
[[nodiscard]] const BuiltinProcedureYield *FindBuiltinProcedureYield(
    const BuiltinProcedure &procedure, std::string_view field_name);

}  // namespace ast
