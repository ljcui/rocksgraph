#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>

#include "ast/ast_const_walker.h"
#include "ast/builtin_function.h"
#include "ast/builtin_procedure.h"
#include "common/exceptions.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graphdb_access.h"
#include "runtime/slotted_executor_internal.h"

namespace rg::slotted {

class RuntimeExpressionCompiler final : public ast::ASTConstWalker {
 public:
  RuntimeExpressionCompiler(const SlotConfiguration &slots,
                            const BoundQueryParameters &parameters)
      : slots_(&slots), parameters_(&parameters) {}

  RuntimeExpressionProgram Compile(const ast::Expression &expression) {
    program_.named_variables.reserve(slots_->SlotCount());
    for (std::size_t offset = 0; offset < slots_->SlotCount(); ++offset) {
      program_.named_variables.emplace_back(slots_->Columns()[offset], offset);
    }
    Walk(expression);
    return std::move(program_);
  }

 protected:
  void Visit(const ast::Variable &variable) override {
    if (const std::optional<std::size_t> offset = slots_->Find(variable.name);
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
  const SlotConfiguration *slots_ = nullptr;
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
    const ast::Expression &expression, const SlotConfiguration &slots) {
  if (last_expression_index_.has_value()) {
    const auto &cached = expressions[*last_expression_index_];
    if (cached.expression == &expression && cached.slots == &slots) {
      return cached.program;
    }
  }
  for (const auto &cached : expressions) {
    if (cached.expression == &expression && cached.slots == &slots) {
      last_expression_index_ = &cached - expressions.data();
      return cached.program;
    }
  }
  expressions.push_back(
      {.expression = &expression,
       .slots = &slots,
       .program = RuntimeExpressionCompiler(slots, bound_parameters)
                      .Compile(expression)});
  last_expression_index_ = expressions.size() - 1;
  return expressions.back().program;
}

class SlottedExpressionBindings final : public ExpressionBindings {
 public:
  SlottedExpressionBindings(const SlottedRow &row,
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
  const SlottedRow *row_ = nullptr;
  const BoundQueryParameters *parameters_ = nullptr;
  const RuntimeExpressionProgram *program_ = nullptr;
};

Value Evaluate(const ast::Expression &expression, const SlottedRow &row,
               const std::vector<ast::PrecomputedExpression> &precomputed,
               RuntimeState &state) {
  const RuntimeExpressionProgram &program =
      state.ExpressionProgram(expression, *row.Slots());
  SlottedExpressionBindings bindings(row, state.bound_parameters, program);
  return EvaluateExpression(expression, bindings, precomputed, state.context);
}

Value Evaluate(const PhysicalExpression &expression, const SlottedRow &row,
               RuntimeState &state) {
  CHECK(expression.Expression() != nullptr, common::InternalError,
        "physical expression is null");
  return Evaluate(*expression.Expression(), row,
                  expression.PrecomputedExpressions(), state);
}
SlottedRow EmptyArgument(SlotConfigurationPtr slots) {
  return SlottedRow(std::move(slots));
}

void CopyMappings(const SlottedRow &source, SlottedRow *target,
                  const std::vector<SlotMapping> &mappings) {
  CopySlots(source, target, mappings);
}

SlottedRow CopyMappedRow(const SlottedRow &source, SlotConfigurationPtr target,
                         const std::vector<SlotMapping> &mappings) {
  return source.CopyTo(std::move(target), mappings);
}

Value ReadRowValue(const SlottedRow &row, std::size_t offset) {
  return row.Get(offset);
}

void StoreEvaluatedValue(SlottedRow *row, std::size_t offset, Value value,
                         RuntimeState &state) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
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
  } catch (const RocksGraphException &error) {
    const bool entity_was_deleted =
        (value.IsNode() && error.code() == ErrorCode::VertexIdNotFound) ||
        (value.IsRelationship() && error.code() == ErrorCode::EdgeIdNotFound);
    if (!entity_was_deleted) {
      throw;
    }
  }
  row->Set(offset, std::move(value));
}

bool TryBindNode(SlottedRow *row, std::size_t offset, graphdb::Vertex vertex) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (!row->IsInitialized(offset)) {
    row->Set(offset, std::move(vertex));
    return true;
  }
  const Value existing = row->Get(offset);
  return existing.IsNode() && existing.AsNode().id == vertex.GetNativeId();
}

bool TryBindNode(SlottedRow *row, std::optional<std::size_t> offset,
                 graphdb::Vertex vertex) {
  if (!offset.has_value()) {
    return true;
  }
  return TryBindNode(row, *offset, std::move(vertex));
}

bool TryBindOptionalEdge(SlottedRow *row, std::optional<std::size_t> offset,
                         graphdb::Edge edge) {
  if (!offset.has_value()) {
    return true;
  }
  return TryBindEdge(row, *offset, std::move(edge));
}

bool TryBindNode(SlottedRow *row, std::size_t offset, std::int64_t id,
                 RuntimeState &state) {
  if (id < 0) {
    return TryBindSlot(row, offset, Value::Null());
  }
  return TryBindNode(row, offset, GraphDBVertexById(*state.transaction, id));
}

bool TryBindNode(SlottedRow *row, std::optional<std::size_t> offset,
                 std::int64_t id, RuntimeState &state) {
  if (!offset.has_value()) {
    return true;
  }
  return TryBindNode(row, *offset, id, state);
}

void SetScannedNode(SlottedRow *row, std::size_t offset,
                    graphdb::Vertex vertex) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  CHECK(!row->IsInitialized(offset), common::InternalError,
        "node scan slot is already initialized");
  row->Set(offset, std::move(vertex));
}

graphdb::Vertex Endpoint(const graphdb::Edge &edge, std::int64_t id) {
  if (edge.GetNativeStartId() == id) {
    return edge.GetStart();
  }
  CHECK(edge.GetNativeEndId() == id, common::InternalError,
        "node is not an endpoint of the relationship");
  return edge.GetEnd();
}

bool MergeMappings(const SlottedRow &source, SlottedRow *target,
                   const std::vector<SlotMapping> &mappings) {
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source_offset)) {
      continue;
    }
    if (!target->IsInitialized(mapping.target_offset)) {
      target->CopySlotFrom(source, mapping.source_offset,
                           mapping.target_offset);
      continue;
    }
    if (!ValuesEqual(ReadRowValue(source, mapping.source_offset),
                     ReadRowValue(*target, mapping.target_offset))) {
      return false;
    }
  }
  return true;
}

std::int64_t NodeId(const SlottedRow &row, std::size_t offset) {
  const Value value = ReadRowValue(row, offset);
  if (value.IsNull()) {
    return -1;
  }
  CHECK(value.IsNode(), common::InvalidArgumentError, "expected node value");
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

Value BuildPathValue(const PathBuildOp &data, const SlottedRow &row,
                     RuntimeState *state) {
  const PhysicalPathPattern &pattern = data.path;
  CHECK(state != nullptr && !pattern.nodes.empty(),
        common::InvalidArgumentError, "path has no nodes: " + pattern.variable);
  CHECK(pattern.nodes.size() == pattern.relationships.size() + 1,
        common::InvalidArgumentError,
        "path node and relationship counts do not match: " + pattern.variable);
  CHECK(
      data.node_input_slots.size() == pattern.nodes.size() &&
          data.relationship_input_slots.size() == pattern.relationships.size(),
      common::InternalError, "path input slot bindings do not match pattern");
  auto path = std::make_shared<Path>();
  std::int64_t current = NodeId(row, data.node_input_slots.front());
  CHECK(current >= 0, common::InvalidArgumentError,
        "path starts with a null node");
  path->nodes.push_back(MaterializePathNode(*state, current));
  for (std::size_t index = 0; index < pattern.relationships.size(); ++index) {
    state->CheckCancelled();
    const Value value = ReadRowValue(row, data.relationship_input_slots[index]);
    std::vector<Value::RelationshipPtr> relationships;
    if (value.IsList()) {
      for (const auto &item : value.AsList()) {
        CHECK(item.IsRelationship(), common::InvalidArgumentError,
              "path relationship list contains a non-relationship");
        relationships.push_back(
            MaterializePathRelationship(*state, item.AsRelationship()));
      }
    } else {
      CHECK(value.IsRelationship(), common::InvalidArgumentError,
            "path value is not a relationship");
      relationships.push_back(
          MaterializePathRelationship(*state, value.AsRelationship()));
    }
    const std::int64_t target = NodeId(row, data.node_input_slots[index + 1]);
    if (!CanTraverse(relationships, current, target)) {
      std::reverse(relationships.begin(), relationships.end());
      CHECK(CanTraverse(relationships, current, target),
            common::InvalidArgumentError,
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

std::vector<ProcedureRecord> ExecuteProcedure(const ProcedureCallOp &data,
                                              RuntimeState *state) {
  CHECK(state != nullptr, common::InternalError, "runtime state is null");
  const ast::BuiltinProcedure *procedure =
      ast::FindBuiltinProcedure(data.procedure_name);
  CHECK(procedure != nullptr, common::InvalidArgumentError,
        "unknown procedure: " + data.procedure_name);
  CHECK(data.arguments.size() == procedure->argument_count,
        common::InvalidArgumentError,
        procedure->name + "() received an invalid argument count");
  CHECK(procedure->read_only && data.read_only, common::InvalidArgumentError,
        "write procedure calls are not supported");
  for (const auto &item : data.yields) {
    CHECK(ast::FindBuiltinProcedureYield(*procedure, item.result_field) !=
              nullptr,
          common::InvalidArgumentError,
          "unknown yield field for " + procedure->name + ": " +
              item.result_field);
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
                               const SlottedRow &row,
                               std::string_view operation,
                               RuntimeState *state) {
  if (property_map.parameter.has_value()) {
    Value value = Evaluate(*property_map.parameter, row, *state);
    CHECK(value.IsMap(), common::InvalidArgumentError,
          std::string(operation) + " properties parameter must be a map");
    return value.AsMap();
  }
  Value::Map properties;
  for (const auto &entry : property_map.entries) {
    properties[entry.key] = Evaluate(entry.value, row, *state);
  }
  return properties;
}

void ExecuteStreamingWrite(const CreateNodeOp &data, const SlottedRow &input,
                           SlottedRow *output, RuntimeState *state) {
  Value::Map properties =
      EvaluatePropertyMap(data.properties, input, "CREATE node", state);
  const Value::NodePtr node = CreateGraphDBVertex(
      *state->transaction, data.labels, std::move(properties));
  CHECK(node != nullptr, common::InternalError,
        "storage returned a null created node");
  output->Set(data.node_slot, GraphDBVertexById(*state->transaction, node->id));
}

void ExecuteStreamingWrite(const CreateRelationshipOp &data,
                           const SlottedRow &input, SlottedRow *output,
                           RuntimeState *state) {
  const std::int64_t left = NodeId(input, data.left_node_slot);
  const std::int64_t right = NodeId(input, data.right_node_slot);
  CHECK(left >= 0 && right >= 0, common::InvalidArgumentError,
        "CREATE relationship endpoints must be nodes");
  Value::Map properties =
      EvaluatePropertyMap(data.properties, input, "CREATE relationship", state);
  const Value::RelationshipPtr relationship = CreateGraphDBEdge(
      *state->transaction, left, right, data.type, std::move(properties));
  CHECK(relationship != nullptr, common::InternalError,
        "storage returned a null created relationship");
  output->Set(
      data.relationship_slot,
      GraphDBEdgeById(*state->transaction, {.id = relationship->id,
                                            .type_id = relationship->type_id}));
}

Value::Map EvaluateMergeProperties(const PhysicalPropertyMap &properties,
                                   const SlottedRow &row,
                                   std::string_view entity,
                                   RuntimeState *state) {
  const std::string operation = "MERGE " + std::string(entity);
  Value::Map values = EvaluatePropertyMap(properties, row, operation, state);
  for (const auto &[key, value] : values) {
    (void)key;
    CHECK(!value.IsNull(), common::InvalidArgumentError,
          operation + " property value is null");
  }
  return values;
}

void ExecuteMergeCreate(const CreateNodeOp &data, SlottedRow *row,
                        RuntimeState *state) {
  Value::Map properties =
      EvaluateMergeProperties(data.properties, *row, "node", state);
  const Value::NodePtr node = CreateGraphDBVertex(
      *state->transaction, data.labels, std::move(properties));
  CHECK(node != nullptr, common::InternalError,
        "storage returned a null created node");
  row->Set(data.node_slot, GraphDBVertexById(*state->transaction, node->id));
}

void ExecuteMergeCreate(const CreateRelationshipOp &data, SlottedRow *row,
                        RuntimeState *state) {
  const std::int64_t left = NodeId(*row, data.left_node_slot);
  const std::int64_t right = NodeId(*row, data.right_node_slot);
  CHECK(left >= 0 && right >= 0, common::InvalidArgumentError,
        "MERGE relationship endpoints must be nodes");
  Value::Map properties =
      EvaluateMergeProperties(data.properties, *row, "relationship", state);
  const Value::RelationshipPtr relationship = CreateGraphDBEdge(
      *state->transaction, left, right, data.type, std::move(properties));
  CHECK(relationship != nullptr, common::InternalError,
        "storage returned a null created relationship");
  row->Set(
      data.relationship_slot,
      GraphDBEdgeById(*state->transaction, {.id = relationship->id,
                                            .type_id = relationship->type_id}));
}

void ExecuteStreamingWrite(const SetPropertyOp &data, const SlottedRow &,
                           SlottedRow *output, RuntimeState *state) {
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
    THROW(common::InvalidArgumentError, "SET property target is not an entity");
  }
}

void ExecuteStreamingWrite(const SetPropertiesOp &data, const SlottedRow &,
                           SlottedRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (entity.IsNull()) {
    return;
  }
  Value value = Evaluate(data.value, *output, *state);
  CHECK(value.IsMap(), common::InvalidArgumentError,
        "SET properties requires a map value");
  if (entity.IsNode()) {
    SetGraphDBVertexProperties(*state->transaction, entity.AsNode().id,
                               std::move(value.AsMap()), data.include_existing);
  } else if (entity.IsRelationship()) {
    SetGraphDBEdgeProperties(*state->transaction,
                             {.id = entity.AsRelationship().id,
                              .type_id = entity.AsRelationship().type_id},
                             std::move(value.AsMap()), data.include_existing);
  } else {
    THROW(common::InvalidArgumentError,
          "SET properties target is not an entity");
  }
}

void ExecuteStreamingWrite(const SetLabelsOp &data, const SlottedRow &,
                           SlottedRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (entity.IsNull()) {
    return;
  }
  CHECK(entity.IsNode(), common::InvalidArgumentError,
        "SET labels target is not a node");
  AddGraphDBVertexLabels(*state->transaction, entity.AsNode().id, data.labels);
}

void ExecuteMergeActions(const MergeOp &data, bool on_match, SlottedRow *row,
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

void ExecuteStreamingWrite(const RemovePropertyOp &data, const SlottedRow &,
                           SlottedRow *output, RuntimeState *state) {
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
    CHECK(entity.IsNull(), common::InvalidArgumentError,
          "REMOVE property target is not an entity");
  }
}

void ExecuteStreamingWrite(const RemoveLabelsOp &data, const SlottedRow &,
                           SlottedRow *output, RuntimeState *state) {
  const Value entity = Evaluate(data.entity, *output, *state);
  if (!entity.IsNull()) {
    CHECK(entity.IsNode(), common::InvalidArgumentError,
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
    const SlottedRow &row, const std::vector<std::size_t> &key_slots) {
  CompositeValueKey result;
  result.values.reserve(key_slots.size());
  for (const std::size_t slot : key_slots) {
    Value value = ReadRowValue(row, slot);
    if (value.IsNull()) {
      return std::nullopt;
    }
    result.values.push_back(std::move(value));
  }
  return result;
}

}  // namespace rg::slotted
