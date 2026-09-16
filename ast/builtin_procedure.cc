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
      {.kind = BuiltinProcedureKind::kCreateNodeIndex,
       .name = "db.index.createNodeIndex",
       .signature = "db.index.createNodeIndex(index_name, label, properties, "
                    "parameter) :: ()",
       .description = "Create a node property index.",
       .argument_count = 4,
       .read_only = false},
      {.kind = BuiltinProcedureKind::kQueryNodes,
       .name = "db.index.queryNodes",
       .signature = "db.index.queryNodes(index_name, query) :: (node)",
       .description = "Query nodes by an exact property index key.",
       .yields = {{"node", SemanticVariableType::kNode}},
       .argument_count = 2},
      {.kind = BuiltinProcedureKind::kRangeQueryNodes,
       .name = "db.index.rangeQueryNodes",
       .signature = "db.index.rangeQueryNodes(index_name, lower, upper, "
                    "parameter) :: (node)",
       .description = "Query nodes by a property index range.",
       .yields = {{"node", SemanticVariableType::kNode}},
       .argument_count = 4},
      {.kind = BuiltinProcedureKind::kCreateNodeFullTextIndex,
       .name = "db.index.fulltext.createNodeIndex",
       .signature = "db.index.fulltext.createNodeIndex(index_name, labels, "
                    "properties) :: ()",
       .description = "Create a node full-text index.",
       .argument_count = 3,
       .read_only = false},
      {.kind = BuiltinProcedureKind::kQueryNodesByFullText,
       .name = "db.index.fulltext.queryNodes",
       .signature =
           "db.index.fulltext.queryNodes(index_name, query, top_n) :: (node, "
           "score)",
       .description = "Query nodes by a full-text index.",
       .yields = {{"node", SemanticVariableType::kNode},
                  {"score", SemanticVariableType::kScalar}},
       .argument_count = 3},
      {.kind = BuiltinProcedureKind::kCreateNodeVectorField,
       .name = "db.index.vector.createNodeField",
       .signature = "db.index.vector.createNodeField(label, property, "
                    "parameter) :: ()",
       .description = "Define a node vector field.",
       .argument_count = 3,
       .read_only = false},
      {.kind = BuiltinProcedureKind::kCreateNodeVectorIndex,
       .name = "db.index.vector.createNodeIndex",
       .signature = "db.index.vector.createNodeIndex(index_name, label, "
                    "property, parameter) :: ()",
       .description = "Create a node vector index.",
       .argument_count = 4,
       .read_only = false},
      {.kind = BuiltinProcedureKind::kKnnSearchNodes,
       .name = "db.index.vector.knnSearchNodes",
       .signature = "db.index.vector.knnSearchNodes(index_name, query, "
                    "parameter) :: (node, distance)",
       .description = "Query nearest nodes by a vector index.",
       .yields = {{"node", SemanticVariableType::kNode},
                  {"distance", SemanticVariableType::kScalar}},
       .argument_count = 3},
      {.kind = BuiltinProcedureKind::kRaftNodeInfos,
       .name = "dbms.graph.getRaftNodeInfos",
       .signature = "dbms.graph.getRaftNodeInfos(graph_name) :: (node_id, ip, "
                    "bolt_port, raft_port, is_leader)",
       .description = "List Raft nodes and identify the current leader.",
       .yields = {{"node_id", SemanticVariableType::kScalar},
                  {"ip", SemanticVariableType::kScalar},
                  {"bolt_port", SemanticVariableType::kScalar},
                  {"raft_port", SemanticVariableType::kScalar},
                  {"is_leader", SemanticVariableType::kScalar}},
       .argument_count = 1},
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
