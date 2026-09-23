#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>

#include "ast/ast_const_walker.h"
#include "ast/builtin_function.h"
#include "ast/builtin_procedure.h"
#include "common/exception.h"
#include "graphdb/graph_db.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graphdb_access.h"
#include "runtime/physical_executor_internal.h"

namespace rg::execution {

class RuntimeExpressionCompiler final : public ast::ASTConstWalker {
 public:
  RuntimeExpressionCompiler(const RowLayout &layout,
                            const BoundQueryParameters &parameters)
      : layout_(&layout), parameters_(&parameters) {}

  RuntimeExpressionProgram Compile(const ast::Expression &expression) {
    program_.named_variables.reserve(layout_->CellCount());
    for (std::size_t offset = 0; offset < layout_->CellCount(); ++offset) {
      program_.named_variables.emplace_back(layout_->Columns()[offset], offset);
    }
    Walk(expression);
    return std::move(program_);
  }

 protected:
  void Visit(const ast::Variable &variable) override {
    if (const std::optional<std::size_t> offset = layout_->Find(variable.name);
        offset.has_value()) {
      program_.variables.emplace_back(&variable, *offset);
    }
  }

  void Visit(const ast::Parameter &parameter) override {
    if (std::optional<std::size_t> offset = parameters_->Offset(parameter.name);
        offset.has_value()) {
      program_.parameters.emplace_back(&parameter, *offset);
    }
  }

 private:
  const RowLayout *layout_ = nullptr;
  const BoundQueryParameters *parameters_ = nullptr;
  RuntimeExpressionProgram program_;
};

RuntimeState::RuntimeState(graphdb::Transaction &graphdb_transaction,
                           const QueryParameters &parameters,
                           QueryExecutionOptions options)
    : transaction(&graphdb_transaction),
      bound_parameters(parameters),
      cancellation(options.cancellation != nullptr
                       ? std::move(options.cancellation)
                       : std::make_shared<QueryCancellationToken>()),
      memory_tracker(options.memory_limit_bytes),
      context{.transaction = &graphdb_transaction,
              .parameters = nullptr,
              .bound_parameters = &bound_parameters,
              .cancellation = cancellation.get(),
              .memory_tracker = &memory_tracker,
              .clock = ExecutionClock::Start()} {}

void RuntimeState::CheckCancelled() const { context.CheckCancelled(); }

const RuntimeExpressionProgram &RuntimeState::ExpressionProgram(
    const ast::Expression &expression, const RowLayout &layout) {
  if (last_expression_index_.has_value()) {
    const auto &cached = expressions[*last_expression_index_];
    if (cached.expression == &expression && cached.layout == &layout) {
      return cached.program;
    }
  }
  for (const auto &cached : expressions) {
    if (cached.expression == &expression && cached.layout == &layout) {
      last_expression_index_ = &cached - expressions.data();
      return cached.program;
    }
  }
  expressions.push_back(
      {.expression = &expression,
       .layout = &layout,
       .program = RuntimeExpressionCompiler(layout, bound_parameters)
                      .Compile(expression)});
  last_expression_index_ = expressions.size() - 1;
  return expressions.back().program;
}

class ExecutionExpressionBindings final : public ExpressionBindings {
 public:
  ExecutionExpressionBindings(const ExecutionRow &row,
                              const BoundQueryParameters &parameters,
                              const RuntimeExpressionProgram &program)
      : row_(&row), parameters_(&parameters), program_(&program) {}

  [[nodiscard]] Value Lookup(std::string_view name) const override {
    const auto found = std::find_if(
        program_->named_variables.begin(), program_->named_variables.end(),
        [name](const auto &entry) { return entry.first == name; });
    if (found != program_->named_variables.end()) {
      return row_->Get(found->second);
    }
    return ExpressionBindings::Lookup(name);
  }

  [[nodiscard]] Value LookupVariable(
      const ast::Variable &variable) const override {
    const auto found = std::find_if(
        program_->variables.begin(), program_->variables.end(),
        [&variable](const auto &entry) { return entry.first == &variable; });
    return found == program_->variables.end() ? Lookup(variable.name)
                                              : row_->Get(found->second);
  }

  [[nodiscard]] bool ReadProperty(std::string_view variable,
                                  std::string_view property_key,
                                  Value *value) const override {
    // Every row variable in a physical expression is compiled into the
    // pointer-keyed program below. Unbound/scoped variables are handled by
    // the generic expression fallback after this fast path returns false.
    (void)variable;
    (void)property_key;
    (void)value;
    return false;
  }

  [[nodiscard]] bool ReadVariableProperty(const ast::Variable &variable,
                                          std::string_view property_key,
                                          Value *value) const override {
    const auto found = std::find_if(
        program_->variables.begin(), program_->variables.end(),
        [&variable](const auto &entry) { return entry.first == &variable; });
    if (found == program_->variables.end()) {
      return ReadProperty(variable.name, property_key, value);
    }
    return row_->ReadProperty(found->second, property_key, value);
  }

  [[nodiscard]] bool ReadParameter(const ast::Parameter &parameter,
                                   Value *value) const override {
    const auto found = std::find_if(
        program_->parameters.begin(), program_->parameters.end(),
        [&parameter](const auto &entry) { return entry.first == &parameter; });
    if (found == program_->parameters.end()) {
      return false;
    }
    *value = parameters_->At(found->second);
    return true;
  }

 private:
  const ExecutionRow *row_ = nullptr;
  const BoundQueryParameters *parameters_ = nullptr;
  const RuntimeExpressionProgram *program_ = nullptr;
};

Value Evaluate(const ast::Expression &expression, const ExecutionRow &row,
               const std::vector<ast::PrecomputedExpression> &precomputed,
               RuntimeState &state) {
  const RuntimeExpressionProgram &program =
      state.ExpressionProgram(expression, *row.Layout());
  ExecutionExpressionBindings bindings(row, state.bound_parameters, program);
  return EvaluateExpression(expression, bindings, precomputed, state.context);
}

Value Evaluate(const PhysicalExpression &expression, const ExecutionRow &row,
               RuntimeState &state) {
  RG_CHECK(expression.Expression() != nullptr, common::ErrorCode::InternalError,
           "physical expression is null");
  return Evaluate(*expression.Expression(), row,
                  expression.PrecomputedExpressions(), state);
}
ExecutionRow EmptyArgument(RowLayoutPtr layout) {
  return ExecutionRow(std::move(layout));
}

void CopyMappings(const ExecutionRow &source, ExecutionRow *target,
                  const std::vector<RowMapping> &mappings) {
  CopyCells(source, target, mappings);
}

ExecutionRow CopyMappedRow(const ExecutionRow &source, RowLayoutPtr target,
                           const std::vector<RowMapping> &mappings) {
  return source.CopyTo(std::move(target), mappings);
}

Value ReadRowValue(const ExecutionRow &row, std::size_t offset) {
  return row.Get(offset);
}

void StoreEvaluatedValue(ExecutionRow *row, std::size_t offset, Value value,
                         RuntimeState &state) {
  RG_CHECK(row != nullptr, common::ErrorCode::InternalError,
           "query row is null");
  try {
    if (value.IsNode()) {
      row->Set(offset,
               GraphDBVertexById(*state.transaction, value.AsNode().id));
      return;
    }
    if (value.IsRelationship()) {
      row->Set(offset,
               GraphDBEdgeById(*state.transaction,
                               {.id = value.AsRelationship().id,
                                .type_id = value.AsRelationship().type_id}));
      return;
    }
  } catch (const common::Exception &error) {
    const bool entity_was_deleted =
        (value.IsNode() &&
         error.code() == common::ErrorCode::VertexIdNotFound) ||
        (value.IsRelationship() &&
         error.code() == common::ErrorCode::EdgeIdNotFound);
    if (!entity_was_deleted) {
      throw;
    }
  }
  row->Set(offset, std::move(value));
}

bool TryBindNode(ExecutionRow *row, std::size_t offset,
                 graphdb::Vertex vertex) {
  RG_CHECK(row != nullptr, common::ErrorCode::InternalError,
           "query row is null");
  if (!row->IsInitialized(offset)) {
    row->Set(offset, std::move(vertex));
    return true;
  }
  const Value existing = row->Get(offset);
  return existing.IsNode() && existing.AsNode().id == vertex.GetId();
}

bool TryBindNode(ExecutionRow *row, std::optional<std::size_t> offset,
                 graphdb::Vertex vertex) {
  if (!offset.has_value()) {
    return true;
  }
  return TryBindNode(row, *offset, std::move(vertex));
}

bool TryBindOptionalEdge(ExecutionRow *row, std::optional<std::size_t> offset,
                         graphdb::Edge edge) {
  if (!offset.has_value()) {
    return true;
  }
  return TryBindEdge(row, *offset, std::move(edge));
}

bool TryBindNode(ExecutionRow *row, std::size_t offset, std::int64_t id,
                 RuntimeState &state) {
  if (id < 0) {
    return TryBindAt(row, offset, Value::Null());
  }
  return TryBindNode(row, offset, GraphDBVertexById(*state.transaction, id));
}

bool TryBindNode(ExecutionRow *row, std::optional<std::size_t> offset,
                 std::int64_t id, RuntimeState &state) {
  if (!offset.has_value()) {
    return true;
  }
  return TryBindNode(row, *offset, id, state);
}

void SetScannedNode(ExecutionRow *row, std::size_t offset,
                    graphdb::Vertex vertex) {
  RG_CHECK(row != nullptr, common::ErrorCode::InternalError,
           "query row is null");
  RG_CHECK(!row->IsInitialized(offset), common::ErrorCode::InternalError,
           "node scan offset is already initialized");
  row->Set(offset, std::move(vertex));
}

graphdb::Vertex Endpoint(const graphdb::Edge &edge, std::int64_t id) {
  if (edge.GetStartId() == id) {
    return edge.GetStart();
  }
  RG_CHECK(edge.GetEndId() == id, common::ErrorCode::InternalError,
           "node is not an endpoint of the relationship");
  return edge.GetEnd();
}

bool MergeMappings(const ExecutionRow &source, ExecutionRow *target,
                   const std::vector<RowMapping> &mappings) {
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source_offset)) {
      continue;
    }
    if (!target->IsInitialized(mapping.target_offset)) {
      target->CopyCellFrom(source, mapping.source_offset,
                           mapping.target_offset);
      continue;
    }
    if (!source.CellEquals(mapping.source_offset, *target,
                           mapping.target_offset)) {
      return false;
    }
  }
  return true;
}

std::int64_t NodeId(const ExecutionRow &row, std::size_t offset) {
  const Value value = ReadRowValue(row, offset);
  if (value.IsNull()) {
    return -1;
  }
  RG_CHECK(value.IsNode(), common::ErrorCode::InvalidParameter,
           "expected node value");
  return value.AsNode().id;
}

bool NodeHasAllLabels(const Node &node,
                      const std::vector<std::string> &labels) {
  for (const auto &label : labels) {
    if (std::find(node.labels.begin(), node.labels.end(), label) ==
        node.labels.end()) {
      return false;
    }
  }
  return true;
}

bool RelationshipHasType(const Relationship &relationship,
                         const std::vector<std::string> &types) {
  return types.empty() || std::find(types.begin(), types.end(),
                                    relationship.type) != types.end();
}

Value::NodePtr MaterializePathNode(RuntimeState &state, std::int64_t id) {
  return MaterializeGraphDBVertex(*state.transaction, id);
}

Value::RelationshipPtr MaterializePathRelationship(
    RuntimeState &state, const Relationship &relationship) {
  return MaterializeGraphDBEdge(
      *state.transaction,
      {.id = relationship.id, .type_id = relationship.type_id});
}

bool CanTraverse(const std::vector<Value::RelationshipPtr> &relationships,
                 std::int64_t from, std::int64_t target) {
  for (const auto &relationship : relationships) {
    if (relationship->start_node_id == from) {
      from = relationship->end_node_id;
    } else if (relationship->end_node_id == from) {
      from = relationship->start_node_id;
    } else {
      return false;
    }
  }
  return from == target;
}

Value BuildPathValue(const PathBuildOp &data, const ExecutionRow &row,
                     RuntimeState *state) {
  const PhysicalPathPattern &pattern = data.path;
  RG_CHECK(state != nullptr && !pattern.nodes.empty(),
           common::ErrorCode::InvalidParameter,
           "path has no nodes: " + pattern.variable);
  RG_CHECK(
      pattern.nodes.size() == pattern.relationships.size() + 1,
      common::ErrorCode::InvalidParameter,
      "path node and relationship counts do not match: " + pattern.variable);
  RG_CHECK(data.node_input_offsets.size() == pattern.nodes.size() &&
               data.relationship_input_offsets.size() ==
                   pattern.relationships.size(),
           common::ErrorCode::InternalError,
           "path input offset bindings do not match pattern");
  auto path = std::make_shared<Path>();
  std::int64_t current = NodeId(row, data.node_input_offsets.front());
  RG_CHECK(current >= 0, common::ErrorCode::InvalidParameter,
           "path starts with a null node");
  path->nodes.push_back(MaterializePathNode(*state, current));
  for (std::size_t index = 0; index < pattern.relationships.size(); ++index) {
    state->CheckCancelled();
    const Value value =
        ReadRowValue(row, data.relationship_input_offsets[index]);
    if (pattern.shortest_path_kind != PhysicalShortestPathKind::kNone &&
        value.IsNull()) {
      return Value::Null();
    }
    std::vector<Value::RelationshipPtr> relationships;
    if (value.IsList()) {
      for (const auto &item : value.AsList()) {
        RG_CHECK(item.IsRelationship(), common::ErrorCode::InvalidParameter,
                 "path relationship list contains a non-relationship");
        relationships.push_back(
            MaterializePathRelationship(*state, item.AsRelationship()));
      }
    } else {
      RG_CHECK(value.IsRelationship(), common::ErrorCode::InvalidParameter,
               "path value is not a relationship");
      relationships.push_back(
          MaterializePathRelationship(*state, value.AsRelationship()));
    }
    const std::int64_t target = NodeId(row, data.node_input_offsets[index + 1]);
    if (!CanTraverse(relationships, current, target)) {
      std::reverse(relationships.begin(), relationships.end());
      RG_CHECK(CanTraverse(relationships, current, target),
               common::ErrorCode::InvalidParameter,
               "path relationship sequence does not connect nodes");
    }
    for (const auto &relationship : relationships) {
      path->relationships.push_back(relationship);
      current = relationship->start_node_id == current
                    ? relationship->end_node_id
                    : relationship->start_node_id;
      path->nodes.push_back(MaterializePathNode(*state, current));
    }
  }
  return Value(std::move(path));
}

namespace {

std::string ProcedureArgumentName(const ast::BuiltinProcedure &procedure,
                                  std::string_view argument) {
  return procedure.name + "() argument " + std::string(argument);
}

const std::string &RequireProcedureString(
    const Value &value, const ast::BuiltinProcedure &procedure,
    std::string_view argument) {
  RG_CHECK(value.IsString(), common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, argument) + " must be a string");
  return value.AsString();
}

const Value::Map &RequireProcedureMap(const Value &value,
                                      const ast::BuiltinProcedure &procedure,
                                      std::string_view argument) {
  RG_CHECK(value.IsMap(), common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, argument) + " must be a map");
  return value.AsMap();
}

std::int64_t RequireProcedureInteger(const Value &value,
                                     const ast::BuiltinProcedure &procedure,
                                     std::string_view argument) {
  RG_CHECK(value.IsInteger(), common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, argument) + " must be an integer");
  return value.AsInteger();
}

int CheckedProcedureInt(std::int64_t value,
                        const ast::BuiltinProcedure &procedure,
                        std::string_view argument) {
  RG_CHECK(value >= std::numeric_limits<int>::min() &&
               value <= std::numeric_limits<int>::max(),
           common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, argument) + " is out of range");
  return static_cast<int>(value);
}

std::vector<std::string> RequireProcedureStringList(
    const Value &value, const ast::BuiltinProcedure &procedure,
    std::string_view argument) {
  RG_CHECK(value.IsList(), common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, argument) + " must be a list");
  std::vector<std::string> result;
  result.reserve(value.AsList().size());
  for (const auto &item : value.AsList()) {
    RG_CHECK(item.IsString(), common::ErrorCode::InvalidParameter,
             ProcedureArgumentName(procedure, argument) +
                 " must contain only strings");
    result.push_back(item.AsString());
  }
  return result;
}

const Value *FindProcedureOption(const Value::Map &options,
                                 std::string_view name) {
  const auto found = options.find(std::string(name));
  return found == options.end() ? nullptr : &found->second;
}

bool ProcedureBoolOption(const Value::Map &options, std::string_view name,
                         bool default_value,
                         const ast::BuiltinProcedure &procedure) {
  const Value *value = FindProcedureOption(options, name);
  if (value == nullptr) {
    return default_value;
  }
  RG_CHECK(value->IsBool(), common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, name) + " must be a boolean");
  return value->AsBool();
}

int ProcedureIntOption(const Value::Map &options, std::string_view name,
                       std::optional<int> default_value,
                       const ast::BuiltinProcedure &procedure) {
  const Value *value = FindProcedureOption(options, name);
  RG_CHECK(value != nullptr || default_value.has_value(),
           common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, name) + " is required");
  if (value == nullptr) {
    return *default_value;
  }
  return CheckedProcedureInt(RequireProcedureInteger(*value, procedure, name),
                             procedure, name);
}

std::string ProcedureStringOption(const Value::Map &options,
                                  std::string_view name,
                                  std::string default_value,
                                  const ast::BuiltinProcedure &procedure) {
  const Value *value = FindProcedureOption(options, name);
  return value == nullptr ? std::move(default_value)
                          : RequireProcedureString(*value, procedure, name);
}

std::vector<float> RequireProcedureFloatList(
    const Value &value, const ast::BuiltinProcedure &procedure,
    std::string_view argument) {
  RG_CHECK(value.IsList(), common::ErrorCode::InvalidParameter,
           ProcedureArgumentName(procedure, argument) + " must be a list");
  std::vector<float> result;
  result.reserve(value.AsList().size());
  for (const auto &item : value.AsList()) {
    RG_CHECK(item.IsInteger() || item.IsDouble(),
             common::ErrorCode::InvalidParameter,
             ProcedureArgumentName(procedure, argument) +
                 " must contain only numbers");
    const double number = item.IsInteger()
                              ? static_cast<double>(item.AsInteger())
                              : item.AsDouble();
    RG_CHECK(std::isfinite(number) &&
                 number >= -std::numeric_limits<float>::max() &&
                 number <= std::numeric_limits<float>::max(),
             common::ErrorCode::InvalidParameter,
             ProcedureArgumentName(procedure, argument) +
                 " contains a non-finite or out-of-range number");
    result.push_back(static_cast<float>(number));
  }
  return result;
}

ProcedureRecord NodeRecord(graphdb::Vertex vertex) {
  return {{"node", Value(MaterializeGraphDBVertex(std::move(vertex)))}};
}

}  // namespace

std::vector<ProcedureRecord> ExecuteProcedure(const ProcedureCallOp &data,
                                              const ExecutionRow &row,
                                              RuntimeState *state) {
  RG_CHECK(state != nullptr, common::ErrorCode::InternalError,
           "runtime state is null");
  const ast::BuiltinProcedure *procedure =
      ast::FindBuiltinProcedure(data.procedure_name);
  RG_CHECK(procedure != nullptr, common::ErrorCode::InvalidParameter,
           "unknown procedure: " + data.procedure_name);
  RG_CHECK(data.arguments.size() == procedure->argument_count,
           common::ErrorCode::InvalidParameter,
           procedure->name + "() received an invalid argument count");
  RG_CHECK(procedure->read_only == data.read_only,
           common::ErrorCode::InternalError,
           "procedure access mode does not match its registry metadata");
  for (const auto &item : data.yields) {
    RG_CHECK(ast::FindBuiltinProcedureYield(*procedure, item.result_field) !=
                 nullptr,
             common::ErrorCode::InvalidParameter,
             "unknown yield field for " + procedure->name + ": " +
                 item.result_field);
  }

  std::vector<Value> arguments;
  arguments.reserve(data.arguments.size());
  for (const auto &argument : data.arguments) {
    arguments.push_back(Evaluate(argument, row, *state));
  }

  graphdb::Transaction &transaction = *state->transaction;
  graphdb::GraphDB *graph = transaction.db();
  RG_CHECK(graph != nullptr, common::ErrorCode::InternalError,
           "procedure transaction has no graph database");

  switch (procedure->kind) {
    case ast::BuiltinProcedureKind::kCreateNodeIndex: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      const auto &label =
          RequireProcedureString(arguments[1], *procedure, "label");
      std::vector<std::string> properties =
          RequireProcedureStringList(arguments[2], *procedure, "properties");
      const Value::Map &options =
          RequireProcedureMap(arguments[3], *procedure, "parameter");
      graph->AddVertexPropertyIndex(
          index_name, ProcedureBoolOption(options, "unique", false, *procedure),
          label, properties);
      return {ProcedureRecord{}};
    }
    case ast::BuiltinProcedureKind::kQueryNodes: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      std::vector<ProcedureRecord> records;
      auto vertices =
          transaction.QueryVertexByPropertyIndex(index_name, arguments[1]);
      while (vertices->Valid()) {
        state->CheckCancelled();
        records.push_back(NodeRecord(vertices->GetVertex()));
        vertices->Next();
      }
      return records;
    }
    case ast::BuiltinProcedureKind::kRangeQueryNodes: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      const std::optional<Value> lower =
          arguments[1].IsNull() ? std::nullopt
                                : std::optional<Value>(arguments[1]);
      const std::optional<Value> upper =
          arguments[2].IsNull() ? std::nullopt
                                : std::optional<Value>(arguments[2]);
      const Value::Map &options =
          RequireProcedureMap(arguments[3], *procedure, "parameter");
      auto vertices = transaction.QueryVertexByPropertyRange(
          index_name, lower, upper,
          ProcedureBoolOption(options, "left_closed", true, *procedure),
          ProcedureBoolOption(options, "right_closed", true, *procedure));
      std::vector<ProcedureRecord> records;
      while (vertices->Valid()) {
        state->CheckCancelled();
        records.push_back(NodeRecord(vertices->GetVertex()));
        vertices->Next();
      }
      return records;
    }
    case ast::BuiltinProcedureKind::kCreateNodeFullTextIndex: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      graph->AddVertexFullTextIndex(
          index_name,
          RequireProcedureStringList(arguments[1], *procedure, "labels"),
          RequireProcedureStringList(arguments[2], *procedure, "properties"));
      return {ProcedureRecord{}};
    }
    case ast::BuiltinProcedureKind::kQueryNodesByFullText: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      const auto &query =
          RequireProcedureString(arguments[1], *procedure, "query");
      const std::int64_t top_n =
          RequireProcedureInteger(arguments[2], *procedure, "top_n");
      RG_CHECK(top_n > 0, common::ErrorCode::InvalidParameter,
               ProcedureArgumentName(*procedure, "top_n") +
                   " must be greater than zero");
      auto vertices = transaction.QueryVertexByFTIndex(
          index_name, query, static_cast<std::size_t>(top_n));
      std::vector<ProcedureRecord> records;
      while (vertices->Valid()) {
        state->CheckCancelled();
        auto &scored = vertices->GetVertexScore();
        records.push_back(
            {{"node", Value(MaterializeGraphDBVertex(scored.vertex))},
             {"score", Value(static_cast<double>(scored.score))}});
        vertices->Next();
      }
      return records;
    }
    case ast::BuiltinProcedureKind::kCreateNodeVectorField: {
      const auto &label =
          RequireProcedureString(arguments[0], *procedure, "label");
      const auto &property =
          RequireProcedureString(arguments[1], *procedure, "property");
      const Value::Map &options =
          RequireProcedureMap(arguments[2], *procedure, "parameter");
      graph->AddVertexVectorField(
          label, property,
          ProcedureIntOption(options, "dimension", std::nullopt, *procedure));
      return {ProcedureRecord{}};
    }
    case ast::BuiltinProcedureKind::kCreateNodeVectorIndex: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      const auto &label =
          RequireProcedureString(arguments[1], *procedure, "label");
      const auto &property =
          RequireProcedureString(arguments[2], *procedure, "property");
      const Value::Map &options =
          RequireProcedureMap(arguments[3], *procedure, "parameter");
      graph->AddVertexVectorIndex(
          index_name, label, property,
          ProcedureIntOption(options, "dimension", std::nullopt, *procedure),
          ProcedureStringOption(options, "distance_type", "l2", *procedure),
          ProcedureIntOption(options, "hnsw_m", 16, *procedure),
          ProcedureIntOption(options, "hnsw_ef_construction", 100, *procedure));
      return {ProcedureRecord{}};
    }
    case ast::BuiltinProcedureKind::kKnnSearchNodes: {
      const auto &index_name =
          RequireProcedureString(arguments[0], *procedure, "index_name");
      std::vector<float> query =
          RequireProcedureFloatList(arguments[1], *procedure, "query");
      const Value::Map &options =
          RequireProcedureMap(arguments[2], *procedure, "parameter");
      const int top_k = ProcedureIntOption(options, "top_k", 10, *procedure);
      const int ef_search =
          ProcedureIntOption(options, "ef_search", 200, *procedure);
      RG_CHECK(top_k > 0, common::ErrorCode::InvalidParameter,
               ProcedureArgumentName(*procedure, "top_k") +
                   " must be greater than zero");
      RG_CHECK(ef_search > 0, common::ErrorCode::InvalidParameter,
               ProcedureArgumentName(*procedure, "ef_search") +
                   " must be greater than zero");
      auto vertices = transaction.QueryVertexByKnnSearch(index_name, query,
                                                         top_k, ef_search);
      std::vector<ProcedureRecord> records;
      while (vertices->Valid()) {
        state->CheckCancelled();
        auto &scored = vertices->GetVertexScore();
        records.push_back(
            {{"node", Value(MaterializeGraphDBVertex(scored.vertex))},
             {"distance", Value(static_cast<double>(scored.score))}});
        vertices->Next();
      }
      return records;
    }
    case ast::BuiltinProcedureKind::kRaftNodeInfos: {
      const auto &graph_name =
          RequireProcedureString(arguments[0], *procedure, "graph_name");
      RG_CHECK(graph_name == graph->db_meta().graph_name(),
               common::ErrorCode::InvalidParameter,
               procedure->name +
                   "() can only inspect the graph bound to the current "
                   "transaction");
      raft::RaftDriver *driver = graph->raft_driver();
      RG_CHECK(driver != nullptr, common::ErrorCode::InvalidParameter,
               "graph [" + graph_name + "] does not enable raft");
      const meta::RaftNodeInfos node_infos = driver->GetNodeInfosWithLeader();
      std::vector<std::uint64_t> node_ids;
      node_ids.reserve(node_infos.nodes().size());
      for (const auto &[node_id, node_info] : node_infos.nodes()) {
        (void)node_info;
        node_ids.push_back(node_id);
      }
      std::sort(node_ids.begin(), node_ids.end());
      std::vector<ProcedureRecord> records;
      records.reserve(node_ids.size());
      for (const std::uint64_t node_id : node_ids) {
        RG_CHECK(node_id <= static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()),
                 common::ErrorCode::InvalidParameter,
                 "Raft node id cannot be represented as a Cypher integer");
        const auto &node_info = node_infos.nodes().at(node_id);
        records.push_back(
            {{"node_id", Value(static_cast<std::int64_t>(node_id))},
             {"ip", Value(node_info.ip())},
             {"bolt_port", Value(node_info.bolt_port())},
             {"raft_port", Value(node_info.raft_poft())},
             {"is_leader", Value(node_info.is_leader())}});
      }
      return records;
    }
    case ast::BuiltinProcedureKind::kLabels:
    case ast::BuiltinProcedureKind::kPropertyKeys:
    case ast::BuiltinProcedureKind::kRelationshipTypes:
    case ast::BuiltinProcedureKind::kProcedures:
      break;
  }

  std::set<std::string> values;
  if (procedure->kind == ast::BuiltinProcedureKind::kLabels ||
      procedure->kind == ast::BuiltinProcedureKind::kPropertyKeys) {
    auto vertices = state->transaction->NewVertexIterator();
    while (vertices->Valid()) {
      state->CheckCancelled();
      graphdb::Vertex &vertex = vertices->GetVertex();
      if (procedure->kind == ast::BuiltinProcedureKind::kLabels) {
        const auto labels = vertex.GetLabels();
        values.insert(labels.begin(), labels.end());
      } else {
        for (const auto &[key, value] : vertex.GetAllProperty()) {
          (void)value;
          values.insert(key);
        }
      }
      vertices->Next();
    }
  }
  if (procedure->kind == ast::BuiltinProcedureKind::kRelationshipTypes ||
      procedure->kind == ast::BuiltinProcedureKind::kPropertyKeys) {
    auto edges = state->transaction->NewEdgeIterator();
    while (edges->Valid()) {
      state->CheckCancelled();
      graphdb::Edge &edge = edges->GetEdge();
      if (procedure->kind == ast::BuiltinProcedureKind::kRelationshipTypes) {
        const std::string type = edge.GetType();
        if (!type.empty()) {
          values.insert(type);
        }
      } else {
        for (const auto &[key, value] : edge.GetAllProperty()) {
          (void)value;
          values.insert(key);
        }
      }
      edges->Next();
    }
  }

  std::vector<ProcedureRecord> records;
  if (procedure->kind == ast::BuiltinProcedureKind::kProcedures) {
    for (const auto &metadata : ast::BuiltinProcedures()) {
      records.push_back({{"name", Value(metadata.name)},
                         {"signature", Value(metadata.signature)},
                         {"description", Value(metadata.description)},
                         {"mode", Value(metadata.read_only ? "READ" : "WRITE")},
                         {"worksOnSystem", Value(metadata.works_on_system)}});
    }
    return records;
  }
  const char *field =
      procedure->kind == ast::BuiltinProcedureKind::kLabels
          ? "label"
          : (procedure->kind == ast::BuiltinProcedureKind::kPropertyKeys
                 ? "propertyKey"
                 : "relationshipType");
  for (const auto &value : values) {
    records.push_back({{field, Value(value)}});
  }
  return records;
}

Value::Map EvaluatePropertyMap(const PhysicalPropertyMap &property_map,
                               const ExecutionRow &row,
                               std::string_view operation,
                               RuntimeState *state) {
  if (property_map.parameter.has_value()) {
    Value value = Evaluate(*property_map.parameter, row, *state);
    RG_CHECK(value.IsMap(), common::ErrorCode::InvalidParameter,
             std::string(operation) + " properties parameter must be a map");
    return value.AsMap();
  }
  Value::Map properties;
  for (const auto &entry : property_map.entries) {
    properties[entry.key] = Evaluate(entry.value, row, *state);
  }
  return properties;
}

void ExecuteStreamingWrite(const CreateNodeOp &data, const ExecutionRow &input,
                           ExecutionRow *output, RuntimeState *state) {
  Value::Map properties =
      EvaluatePropertyMap(data.properties, input, "CREATE node", state);
  const Value::NodePtr node = CreateGraphDBVertex(
      *state->transaction, data.labels, std::move(properties));
  RG_CHECK(node != nullptr, common::ErrorCode::InternalError,
           "storage returned a null created node");
  output->Set(data.node_offset,
              GraphDBVertexById(*state->transaction, node->id));
}

void ExecuteStreamingWrite(const CreateRelationshipOp &data,
                           const ExecutionRow &input, ExecutionRow *output,
                           RuntimeState *state) {
  const std::int64_t left = NodeId(input, data.left_node_offset);
  const std::int64_t right = NodeId(input, data.right_node_offset);
  RG_CHECK(left >= 0 && right >= 0, common::ErrorCode::InvalidParameter,
           "CREATE relationship endpoints must be nodes");
  Value::Map properties =
      EvaluatePropertyMap(data.properties, input, "CREATE relationship", state);
  const Value::RelationshipPtr relationship = CreateGraphDBEdge(
      *state->transaction, left, right, data.type, std::move(properties));
  RG_CHECK(relationship != nullptr, common::ErrorCode::InternalError,
           "storage returned a null created relationship");
  output->Set(
      data.relationship_offset,
      GraphDBEdgeById(*state->transaction, {.id = relationship->id,
                                            .type_id = relationship->type_id}));
}

Value::Map EvaluateMergeProperties(const PhysicalPropertyMap &properties,
                                   const ExecutionRow &row,
                                   std::string_view entity,
                                   RuntimeState *state) {
  const std::string operation = "MERGE " + std::string(entity);
  Value::Map values = EvaluatePropertyMap(properties, row, operation, state);
  for (const auto &[key, value] : values) {
    (void)key;
    RG_CHECK(!value.IsNull(), common::ErrorCode::InvalidParameter,
             operation + " property value is null");
  }
  return values;
}

void ExecuteMergeCreate(const CreateNodeOp &data, ExecutionRow *row,
                        RuntimeState *state) {
  Value::Map properties =
      EvaluateMergeProperties(data.properties, *row, "node", state);
  const Value::NodePtr node = CreateGraphDBVertex(
      *state->transaction, data.labels, std::move(properties));
  RG_CHECK(node != nullptr, common::ErrorCode::InternalError,
           "storage returned a null created node");
  row->Set(data.node_offset, GraphDBVertexById(*state->transaction, node->id));
}

void ExecuteMergeCreate(const CreateRelationshipOp &data, ExecutionRow *row,
                        RuntimeState *state) {
  const std::int64_t left = NodeId(*row, data.left_node_offset);
  const std::int64_t right = NodeId(*row, data.right_node_offset);
  RG_CHECK(left >= 0 && right >= 0, common::ErrorCode::InvalidParameter,
           "MERGE relationship endpoints must be nodes");
  Value::Map properties =
      EvaluateMergeProperties(data.properties, *row, "relationship", state);
  const Value::RelationshipPtr relationship = CreateGraphDBEdge(
      *state->transaction, left, right, data.type, std::move(properties));
  RG_CHECK(relationship != nullptr, common::ErrorCode::InternalError,
           "storage returned a null created relationship");
  row->Set(
      data.relationship_offset,
      GraphDBEdgeById(*state->transaction, {.id = relationship->id,
                                            .type_id = relationship->type_id}));
}

void ExecuteStreamingWrite(const SetPropertyOp &data, const ExecutionRow &,
                           ExecutionRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (entity.IsNull()) {
    return;
  }
  Value value = Evaluate(data.value, *output, *state);
  if (entity.IsNode()) {
    SetGraphDBVertexProperty(*state->transaction, entity.AsNode().id,
                             data.property_key, std::move(value));
  } else if (entity.IsRelationship()) {
    SetGraphDBEdgeProperty(*state->transaction,
                           {.id = entity.AsRelationship().id,
                            .type_id = entity.AsRelationship().type_id},
                           data.property_key, std::move(value));
  } else {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "SET property target is not an entity");
  }
}

void ExecuteStreamingWrite(const SetPropertiesOp &data, const ExecutionRow &,
                           ExecutionRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (entity.IsNull()) {
    return;
  }
  Value value = Evaluate(data.value, *output, *state);
  Value::Map properties;
  if (value.IsMap()) {
    properties = std::move(value.AsMap());
  } else if (value.IsNode()) {
    properties = value.AsNode().properties;
  } else if (value.IsRelationship()) {
    properties = value.AsRelationship().properties;
  } else {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "SET properties requires a map or graph entity value");
  }
  if (entity.IsNode()) {
    SetGraphDBVertexProperties(*state->transaction, entity.AsNode().id,
                               std::move(properties), data.include_existing);
  } else if (entity.IsRelationship()) {
    SetGraphDBEdgeProperties(*state->transaction,
                             {.id = entity.AsRelationship().id,
                              .type_id = entity.AsRelationship().type_id},
                             std::move(properties), data.include_existing);
  } else {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "SET properties target is not an entity");
  }
}

void ExecuteStreamingWrite(const SetLabelsOp &data, const ExecutionRow &,
                           ExecutionRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (entity.IsNull()) {
    return;
  }
  RG_CHECK(entity.IsNode(), common::ErrorCode::InvalidParameter,
           "SET labels target is not a node");
  AddGraphDBVertexLabels(*state->transaction, entity.AsNode().id, data.labels);
}

void ExecuteMergeActions(const MergeOp &data, bool on_match, ExecutionRow *row,
                         RuntimeState *state) {
  for (const auto &action : data.actions) {
    if (action.on_match != on_match) {
      continue;
    }
    for (const auto &operation : action.set_operations) {
      std::visit(
          [row, state](const auto &set_operation) {
            ExecuteStreamingWrite(set_operation, *row, row, state);
          },
          operation);
    }
  }
}

void ExecuteStreamingWrite(const RemovePropertyOp &data, const ExecutionRow &,
                           ExecutionRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (entity.IsNode()) {
    RemoveGraphDBVertexProperty(*state->transaction, entity.AsNode().id,
                                data.property_key);
  } else if (entity.IsRelationship()) {
    RemoveGraphDBEdgeProperty(*state->transaction,
                              {.id = entity.AsRelationship().id,
                               .type_id = entity.AsRelationship().type_id},
                              data.property_key);
  } else {
    RG_CHECK(entity.IsNull(), common::ErrorCode::InvalidParameter,
             "REMOVE property target is not an entity");
  }
}

void ExecuteStreamingWrite(const RemoveLabelsOp &data, const ExecutionRow &,
                           ExecutionRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (!entity.IsNull()) {
    RG_CHECK(entity.IsNode(), common::ErrorCode::InvalidParameter,
             "REMOVE labels target is not a node");
    RemoveGraphDBVertexLabels(*state->transaction, entity.AsNode().id,
                              data.labels);
  }
}

std::size_t EstimatedKeyHeapUsage(const CompositeValueKey &key) {
  std::size_t bytes = key.values.capacity() * sizeof(Value);
  for (const Value &value : key.values) {
    const std::size_t value_bytes = EstimatedValueHeapUsage(value);
    bytes += value_bytes > sizeof(Value) ? value_bytes - sizeof(Value) : 0U;
  }
  return bytes;
}

std::optional<CompositeValueKey> NodeJoinKey(
    const ExecutionRow &row, const std::vector<std::size_t> &key_offsets) {
  CompositeValueKey result;
  result.values.reserve(key_offsets.size());
  for (const std::size_t offset : key_offsets) {
    Value value = ReadRowValue(row, offset);
    if (value.IsNull()) {
      return std::nullopt;
    }
    result.values.push_back(std::move(value));
  }
  return result;
}

}  // namespace rg::execution
