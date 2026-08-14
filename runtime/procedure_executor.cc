#include "runtime/procedure_executor.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/builtin_procedure.h"
#include "common/exception.h"
#include "value/value.h"

namespace rg {
namespace {

using ProcedureRecord = std::map<std::string, Value>;

std::set<std::string> CollectLabels(const GraphReader &graph_reader) {
  std::set<std::string> labels;
  for (const auto &node : graph_reader.ScanNodes()) {
    for (const auto &label : node->labels) {
      labels.insert(label);
    }
  }
  return labels;
}

std::set<std::string> CollectRelationshipTypes(
    const GraphReader &graph_reader) {
  std::set<std::string> types;
  for (const auto &relationship : graph_reader.ScanRelationships()) {
    if (!relationship->type.empty()) {
      types.insert(relationship->type);
    }
  }
  return types;
}

std::set<std::string> CollectPropertyKeys(const GraphReader &graph_reader) {
  std::set<std::string> keys;
  for (const auto &node : graph_reader.ScanNodes()) {
    for (const auto &[key, value] : node->properties) {
      (void)value;
      keys.insert(key);
    }
  }
  for (const auto &relationship : graph_reader.ScanRelationships()) {
    for (const auto &[key, value] : relationship->properties) {
      (void)value;
      keys.insert(key);
    }
  }
  return keys;
}

std::vector<ProcedureRecord> SingleFieldRecords(
    std::string_view field, const std::set<std::string> &values) {
  std::vector<ProcedureRecord> records;
  records.reserve(values.size());
  for (const auto &value : values) {
    records.push_back({{std::string(field), Value(value)}});
  }
  return records;
}

std::vector<ProcedureRecord> ProcedureMetadataRecords() {
  std::vector<ProcedureRecord> records;
  records.reserve(ast::BuiltinProcedures().size());
  for (const auto &procedure : ast::BuiltinProcedures()) {
    records.push_back({
        {"name", Value(procedure.name)},
        {"signature", Value(procedure.signature)},
        {"description", Value(procedure.description)},
        {"mode", Value(procedure.read_only ? "READ" : "WRITE")},
        {"worksOnSystem", Value(procedure.works_on_system)},
    });
  }
  return records;
}

std::vector<ProcedureRecord> ExecuteBuiltinProcedure(
    const ast::BuiltinProcedure &procedure, const GraphReader &graph_reader) {
  switch (procedure.kind) {
    case ast::BuiltinProcedureKind::kLabels:
      return SingleFieldRecords("label", CollectLabels(graph_reader));
    case ast::BuiltinProcedureKind::kPropertyKeys:
      return SingleFieldRecords("propertyKey",
                                CollectPropertyKeys(graph_reader));
    case ast::BuiltinProcedureKind::kRelationshipTypes:
      return SingleFieldRecords("relationshipType",
                                CollectRelationshipTypes(graph_reader));
    case ast::BuiltinProcedureKind::kProcedures:
      return ProcedureMetadataRecords();
  }
  THROW(common::InternalError, "unknown built-in procedure kind");
}

void ValidateProcedureCall(const ir::ProcedureCallPlan &plan,
                           const ast::BuiltinProcedure &procedure) {
  CHECK(plan.Arguments().size() == procedure.argument_count,
        common::InvalidArgumentError,
        procedure.name + "() expects " +
            std::to_string(procedure.argument_count) + " arguments");
  CHECK(procedure.read_only && plan.ReadOnly(), common::InvalidArgumentError,
        "write procedure calls are not supported");
  for (const auto &item : plan.YieldItems()) {
    const std::string &field =
        item.result_field.has_value() ? *item.result_field : item.variable;
    CHECK(ast::FindBuiltinProcedureYield(procedure, field) != nullptr,
          common::InvalidArgumentError,
          "unknown yield field for " + procedure.name + ": " + field);
  }
}

}  // namespace

QueryRows ProcedureExecutor::Execute(const ir::ProcedureCallPlan &plan,
                                     const QueryRows &input) const {
  CHECK(graph_reader_ != nullptr, common::InternalError, "access path is null");
  const ast::BuiltinProcedure *procedure =
      ast::FindBuiltinProcedure(plan.ProcedureName());
  CHECK(procedure != nullptr, common::InvalidArgumentError,
        "unknown procedure: " + plan.ProcedureName());
  ValidateProcedureCall(plan, *procedure);

  const std::vector<ProcedureRecord> records =
      ExecuteBuiltinProcedure(*procedure, *graph_reader_);
  QueryRows out;
  out.reserve(input.size() * records.size());
  for (const auto &row : input) {
    for (const auto &record : records) {
      QueryRow next = row;
      for (const auto &item : plan.YieldItems()) {
        const std::string &field =
            item.result_field.has_value() ? *item.result_field : item.variable;
        next[item.variable] = record.at(field);
      }
      out.push_back(std::move(next));
    }
  }
  return out;
}

}  // namespace rg
