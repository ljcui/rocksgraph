#include "ast/builtin_procedure.h"

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

}  // namespace

const std::vector<BuiltinProcedure> &BuiltinProcedures() {
  static const std::vector<BuiltinProcedure> kProcedures = {
      {.kind = BuiltinProcedureKind::kLabels,
       .name = "db.labels",
       .signature = "db.labels() :: (label)",
       .description = "List all node labels in the graph.",
       .yields = {{"label", SemanticVariableType::kScalar}}},
      {.kind = BuiltinProcedureKind::kPropertyKeys,
       .name = "db.propertyKeys",
       .signature = "db.propertyKeys() :: (propertyKey)",
       .description = "List all property keys in the graph.",
       .yields = {{"propertyKey", SemanticVariableType::kScalar}}},
      {.kind = BuiltinProcedureKind::kRelationshipTypes,
       .name = "db.relationshipTypes",
       .signature = "db.relationshipTypes() :: (relationshipType)",
       .description = "List all relationship types in the graph.",
       .yields = {{"relationshipType", SemanticVariableType::kScalar}}},
      {.kind = BuiltinProcedureKind::kProcedures,
       .name = "dbms.procedures",
       .signature = "dbms.procedures() :: (name, signature, description, mode, "
                    "worksOnSystem)",
       .description = "List all built-in procedures.",
       .yields = {{"name", SemanticVariableType::kScalar},
                  {"signature", SemanticVariableType::kScalar},
                  {"description", SemanticVariableType::kScalar},
                  {"mode", SemanticVariableType::kScalar},
                  {"worksOnSystem", SemanticVariableType::kScalar}}},
  };
  return kProcedures;
}

const BuiltinProcedure *FindBuiltinProcedure(std::string_view name) {
  const std::string normalized = LowerAscii(name);
  for (const auto &procedure : BuiltinProcedures()) {
    if (LowerAscii(procedure.name) == normalized) {
      return &procedure;
    }
  }
  return nullptr;
}

const BuiltinProcedureYield *FindBuiltinProcedureYield(
    const BuiltinProcedure &procedure, std::string_view field_name) {
  for (const auto &yield : procedure.yields) {
    if (yield.name == field_name) {
      return &yield;
    }
  }
  return nullptr;
}

}  // namespace ast
