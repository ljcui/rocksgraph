#pragma once

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast_node.h"

namespace ast {

class SemanticTableAnalyzer;

enum class SemanticVariableType {
  kUnknown,
  kNode,
  kRelationship,
  kPath,
  kScalar,
  kList,
  kMap,
};

[[nodiscard]] std::string_view ToString(SemanticVariableType type);
std::ostream &operator<<(std::ostream &out, SemanticVariableType type);

class SemanticTable {
 public:
  [[nodiscard]] std::optional<SemanticVariableType> VariableType(
      std::string_view name) const;
  [[nodiscard]] std::optional<SemanticVariableType> VariableTypeAt(
      const ASTNode &node, std::string_view name) const;
  [[nodiscard]] const std::unordered_map<std::string, SemanticVariableType> &
  VariableTypesAt(const ASTNode &node) const;
  [[nodiscard]] std::optional<SemanticVariableType> KnownFunctionResultType(
      std::string_view function_name) const;
  [[nodiscard]] std::optional<SemanticVariableType> KnownProcedureYieldType(
      std::string_view procedure_name, std::string_view field_name) const;
  [[nodiscard]] std::vector<std::string> KnownProcedureYieldFields(
      std::string_view procedure_name) const;
  [[nodiscard]] std::optional<bool> KnownProcedureReadOnly(
      std::string_view procedure_name) const;
  [[nodiscard]] const std::unordered_map<std::string, SemanticVariableType> &
  VariableTypes() const {
    return variable_types_;
  }

  [[nodiscard]] const std::unordered_set<std::string> &ExpressionDependencies(
      const Expression &expression) const;
  [[nodiscard]] bool ContainsAggregation(const Expression &expression) const;
  [[nodiscard]] const std::vector<std::string> &ProjectionOutputs(
      const ProjectionBody &body) const;

 private:
  friend class SemanticTableAnalyzer;
  friend SemanticTable AnalyzeSemanticTable(const ASTNode &node);

  void RecordVariableType(std::string_view name, SemanticVariableType type);
  void RecordVariableTypes(
      const ASTNode &node,
      std::unordered_map<std::string, SemanticVariableType> types);
  void RecordExpressionDependencies(const Expression &expression,
                                    std::unordered_set<std::string> symbols);
  void RecordAggregation(const Expression &expression, bool contains);
  void RecordProjectionOutputs(const ProjectionBody &body,
                               std::vector<std::string> outputs);

  std::unordered_map<std::string, SemanticVariableType> variable_types_;
  std::unordered_map<const ASTNode *,
                     std::unordered_map<std::string, SemanticVariableType>>
      scoped_variable_types_;
  std::unordered_map<const Expression *, std::unordered_set<std::string>>
      expression_dependencies_;
  std::unordered_set<const Expression *> aggregation_expressions_;
  std::unordered_map<const ProjectionBody *, std::vector<std::string>>
      projection_outputs_;
};

[[nodiscard]] SemanticTable AnalyzeSemanticTable(const ASTNode &node);

}  // namespace ast
