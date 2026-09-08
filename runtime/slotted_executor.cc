#include "runtime/slotted_executor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
#include "ast/ast_node.h"
#include "ast/builtin_function.h"
#include "ast/builtin_procedure.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "ir/query_ir_internal.h"
#include "runtime/expression_evaluator.h"

namespace rg {
namespace {

class PullOperator {
 public:
  PullOperator() = default;
  PullOperator(const PullOperator &) = delete;
  PullOperator &operator=(const PullOperator &) = delete;
  virtual ~PullOperator() = default;

  [[nodiscard]] virtual bool Next(SlottedRow *row) = 0;
  virtual void Close() noexcept = 0;
};

class CursorRegistry final {
 public:
  [[nodiscard]] EntityIdCursor *Track(std::unique_ptr<EntityIdCursor> cursor) {
    CHECK(cursor != nullptr, common::InternalError, "entity cursor is null");
    EntityIdCursor *result = cursor.get();
    cursors_.push_back(std::move(cursor));
    return result;
  }

  void Release(EntityIdCursor *cursor) noexcept {
    if (cursor == nullptr) {
      return;
    }
    cursor->Close();
    const auto found = std::find_if(
        cursors_.begin(), cursors_.end(),
        [cursor](const auto &owned) { return owned.get() == cursor; });
    if (found != cursors_.end()) {
      cursors_.erase(found);
    }
  }

  void Close() noexcept {
    for (const auto &cursor : cursors_) {
      cursor->Close();
    }
    cursors_.clear();
  }

  ~CursorRegistry() { Close(); }

 private:
  std::vector<std::unique_ptr<EntityIdCursor>> cursors_;
};

struct RuntimeExpressionProgram {
  std::unordered_map<const ast::Variable *, Slot> variables;
  std::unordered_map<const ast::Parameter *, std::size_t> parameters;
};

class RuntimeExpressionCompiler final : public ast::ASTConstWalker {
 public:
  RuntimeExpressionCompiler(const SlotConfiguration &slots,
                            const BoundQueryParameters &parameters)
      : slots_(&slots), parameters_(&parameters) {}

  RuntimeExpressionProgram Compile(const ast::Expression &expression) {
    Walk(expression);
    return std::move(program_);
  }

 protected:
  void Visit(const ast::Variable &variable) override {
    if (const Slot *slot = slots_->Find(variable.name); slot != nullptr) {
      program_.variables.emplace(&variable, *slot);
    }
  }

  void Visit(const ast::Parameter &parameter) override {
    if (std::optional<std::size_t> offset = parameters_->Offset(parameter.name);
        offset.has_value()) {
      program_.parameters.emplace(&parameter, *offset);
    }
  }

 private:
  const SlotConfiguration *slots_ = nullptr;
  const BoundQueryParameters *parameters_ = nullptr;
  RuntimeExpressionProgram program_;
};

struct RuntimeState {
  RuntimeState(const GraphReader &reader, Storage *writable_storage,
               const QueryParameters &parameters, QueryExecutionOptions options)
      : graph_reader(&reader),
        storage(writable_storage),
        bound_parameters(parameters),
        cancellation(options.cancellation != nullptr
                         ? std::move(options.cancellation)
                         : std::make_shared<QueryCancellationToken>()),
        memory_tracker(options.memory_limit_bytes),
        context{.graph_reader = &reader,
                .parameters = nullptr,
                .bound_parameters = &bound_parameters,
                .cancellation = cancellation.get(),
                .memory_tracker = &memory_tracker,
                .clock = ExecutionClock::Start()} {}

  void CheckCancelled() const { context.CheckCancelled(); }
  [[nodiscard]] EntityIdCursor *TrackCursor(
      std::unique_ptr<EntityIdCursor> cursor) {
    return resources.Track(std::move(cursor));
  }
  void ReleaseCursor(EntityIdCursor *cursor) noexcept {
    resources.Release(cursor);
  }
  [[nodiscard]] const RuntimeExpressionProgram &ExpressionProgram(
      const ast::Expression &expression, const SlotConfiguration &slots) {
    for (const auto &cached : expressions) {
      if (cached.expression == &expression && cached.slots == &slots) {
        return cached.program;
      }
    }
    expressions.push_back(
        {.expression = &expression,
         .slots = &slots,
         .program = RuntimeExpressionCompiler(slots, bound_parameters)
                        .Compile(expression)});
    return expressions.back().program;
  }

  const GraphReader *graph_reader;
  Storage *storage;
  BoundQueryParameters bound_parameters;
  std::shared_ptr<QueryCancellationToken> cancellation;
  QueryMemoryTracker memory_tracker;
  CursorRegistry resources;
  ExecutionContext context;
  struct CachedExpression {
    const ast::Expression *expression = nullptr;
    const SlotConfiguration *slots = nullptr;
    RuntimeExpressionProgram program;
  };
  std::vector<CachedExpression> expressions;
};

class SlottedExpressionBindings final : public ExpressionBindings {
 public:
  SlottedExpressionBindings(const SlottedRow &row,
                            const GraphReader &graph_reader,
                            const BoundQueryParameters &parameters,
                            const RuntimeExpressionProgram &program)
      : row_(&row),
        graph_reader_(&graph_reader),
        parameters_(&parameters),
        program_(&program) {}

  [[nodiscard]] Value Lookup(std::string_view name) const override {
    return row_->Get(name, *graph_reader_);
  }

  [[nodiscard]] Value LookupVariable(
      const ast::Variable &variable) const override {
    const auto found = program_->variables.find(&variable);
    return found == program_->variables.end()
               ? Lookup(variable.name)
               : row_->Get(found->second, *graph_reader_);
  }

  [[nodiscard]] bool ReadProperty(std::string_view variable,
                                  std::string_view property_key,
                                  Value *value) const override {
    CHECK(value != nullptr, common::InternalError, "property output is null");
    const Slot *slot = row_->Slots()->Find(variable);
    if (slot == nullptr || slot->kind == SlotKind::kReference) {
      return false;
    }
    const std::int64_t id = row_->EntityIdAt(*slot);
    if (id < 0) {
      *value = Value::Null();
    } else if (slot->kind == SlotKind::kNode) {
      *value = graph_reader_->NodeProperty(id, property_key);
    } else {
      *value = graph_reader_->RelationshipProperty(id, property_key);
    }
    return true;
  }

  [[nodiscard]] bool ReadVariableProperty(const ast::Variable &variable,
                                          std::string_view property_key,
                                          Value *value) const override {
    const auto found = program_->variables.find(&variable);
    if (found == program_->variables.end()) {
      return ReadProperty(variable.name, property_key, value);
    }
    const Slot &slot = found->second;
    if (slot.kind == SlotKind::kReference) {
      return false;
    }
    const std::int64_t id = row_->EntityIdAt(slot);
    *value =
        id < 0 ? Value::Null()
               : (slot.kind == SlotKind::kNode
                      ? graph_reader_->NodeProperty(id, property_key)
                      : graph_reader_->RelationshipProperty(id, property_key));
    return true;
  }

  [[nodiscard]] bool ReadParameter(const ast::Parameter &parameter,
                                   Value *value) const override {
    const auto found = program_->parameters.find(&parameter);
    if (found == program_->parameters.end()) {
      return false;
    }
    *value = parameters_->At(found->second);
    return true;
  }

 private:
  const SlottedRow *row_ = nullptr;
  const GraphReader *graph_reader_ = nullptr;
  const BoundQueryParameters *parameters_ = nullptr;
  const RuntimeExpressionProgram *program_ = nullptr;
};

Value Evaluate(const ast::Expression &expression, const SlottedRow &row,
               const std::vector<ast::PrecomputedExpression> &precomputed,
               RuntimeState &state) {
  const RuntimeExpressionProgram &program =
      state.ExpressionProgram(expression, *row.Slots());
  SlottedExpressionBindings bindings(row, *state.graph_reader,
                                     state.bound_parameters, program);
  return EvaluateExpression(expression, bindings, precomputed, state.context);
}

Value Evaluate(const PhysicalExpression &expression, const SlottedRow &row,
               RuntimeState &state) {
  CHECK(expression.Expression() != nullptr, common::InternalError,
        "physical expression is null");
  return Evaluate(*expression.Expression(), row,
                  expression.PrecomputedExpressions(), state);
}

template <typename T>
const T &OperatorData(const PhysicalPlanNode &node) {
  const T *data = std::get_if<T>(&node.data);
  CHECK(data != nullptr, common::InternalError,
        "physical operator payload does not match " +
            std::string(ToString(node.kind)));
  return *data;
}

SlottedRow EmptyArgument(SlotConfigurationPtr slots) {
  return SlottedRow(std::move(slots));
}

void CopyMappings(const SlottedRow &source, SlottedRow *target,
                  const std::vector<SlotMapping> &mappings,
                  const RuntimeState &state) {
  CopySlots(source, target, mappings, *state.graph_reader);
}

bool MergeMappings(const SlottedRow &source, SlottedRow *target,
                   const std::vector<SlotMapping> &mappings,
                   const RuntimeState &state) {
  for (const auto &mapping : mappings) {
    if (!source.IsInitialized(mapping.source)) {
      continue;
    }
    if (!target->IsInitialized(mapping.target)) {
      if (mapping.source.kind == mapping.target.kind &&
          mapping.source.kind != SlotKind::kReference) {
        target->SetEntityId(mapping.target, source.EntityIdAt(mapping.source));
      } else if (mapping.source.kind == SlotKind::kReference &&
                 mapping.target.kind == SlotKind::kReference) {
        target->SetReference(mapping.target,
                             source.ReferenceAt(mapping.source));
      } else {
        target->Set(mapping.target_name,
                    source.Get(mapping.source, *state.graph_reader));
      }
      continue;
    }
    if (mapping.source.kind == mapping.target.kind &&
        mapping.source.kind != SlotKind::kReference) {
      if (source.EntityIdAt(mapping.source) !=
          target->EntityIdAt(mapping.target)) {
        return false;
      }
    } else if (!ValuesEqual(source.Get(mapping.source, *state.graph_reader),
                            target->Get(mapping.target, *state.graph_reader))) {
      return false;
    }
  }
  return true;
}

std::int64_t NodeId(const SlottedRow &row, std::string_view name,
                    const RuntimeState &state) {
  const Slot &slot = row.Slots()->At(name);
  if (slot.kind == SlotKind::kNode) {
    return row.EntityIdAt(slot);
  }
  const Value value = row.Get(name, *state.graph_reader);
  if (value.IsNull()) {
    return -1;
  }
  CHECK(value.IsNode(), common::InvalidArgumentError,
        "expected node value: " + std::string(name));
  return value.AsNode().id;
}

bool RelationshipMatchesDirection(const Relationship &relationship,
                                  std::int64_t from_id,
                                  ir::ExpandDirection direction) {
  if (direction == ir::ExpandDirection::kOutgoing) {
    return relationship.start_node_id == from_id;
  }
  if (direction == ir::ExpandDirection::kIncoming) {
    return relationship.end_node_id == from_id;
  }
  return relationship.start_node_id == from_id ||
         relationship.end_node_id == from_id;
}

std::int64_t OtherNode(const Relationship &relationship, std::int64_t from_id) {
  return relationship.start_node_id == from_id ? relationship.end_node_id
                                               : relationship.start_node_id;
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

std::unique_ptr<EntityIdCursor> ExpandCursor(const GraphReader &graph_reader,
                                             std::int64_t node_id,
                                             ir::ExpandDirection direction);

std::optional<std::int64_t> NextVarExpandNode(const Relationship &relationship,
                                              std::int64_t current_node_id,
                                              ir::ExpandDirection direction) {
  if (direction == ir::ExpandDirection::kOutgoing) {
    return relationship.start_node_id == current_node_id
               ? std::optional<std::int64_t>(relationship.end_node_id)
               : std::nullopt;
  }
  if (direction == ir::ExpandDirection::kIncoming) {
    return relationship.end_node_id == current_node_id
               ? std::optional<std::int64_t>(relationship.start_node_id)
               : std::nullopt;
  }
  if (relationship.start_node_id == current_node_id) {
    return relationship.end_node_id;
  }
  if (relationship.end_node_id == current_node_id) {
    return relationship.start_node_id;
  }
  return std::nullopt;
}

bool CanTraverse(const std::vector<GraphReader::RelationshipPtr> &relationships,
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

Value BuildPathValue(const ir::PathPattern &pattern, const SlottedRow &row,
                     RuntimeState *state) {
  CHECK(state != nullptr && !pattern.nodes.empty(),
        common::InvalidArgumentError, "path has no nodes: " + pattern.variable);
  CHECK(pattern.nodes.size() == pattern.relationships.size() + 1,
        common::InvalidArgumentError,
        "path node and relationship counts do not match: " + pattern.variable);
  auto path = std::make_shared<Path>();
  std::int64_t current = NodeId(row, pattern.nodes.front(), *state);
  CHECK(current >= 0, common::InvalidArgumentError,
        "path starts with a null node");
  path->nodes.push_back(state->graph_reader->NodeById(current));
  for (std::size_t index = 0; index < pattern.relationships.size(); ++index) {
    state->CheckCancelled();
    const Value value =
        row.Get(pattern.relationships[index], *state->graph_reader);
    std::vector<GraphReader::RelationshipPtr> relationships;
    if (value.IsList()) {
      for (const auto &item : value.AsList()) {
        CHECK(item.IsRelationship(), common::InvalidArgumentError,
              "path relationship list contains a non-relationship");
        relationships.push_back(
            state->graph_reader->RelationshipById(item.AsRelationship().id));
      }
    } else {
      CHECK(value.IsRelationship(), common::InvalidArgumentError,
            "path value is not a relationship");
      relationships.push_back(
          state->graph_reader->RelationshipById(value.AsRelationship().id));
    }
    const std::int64_t target = NodeId(row, pattern.nodes[index + 1], *state);
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
      path->nodes.push_back(state->graph_reader->NodeById(current));
    }
  }
  return Value(std::move(path));
}

using ProcedureRecord = Value::Map;

std::vector<ProcedureRecord> ExecuteProcedure(const ir::ProcedureCallPlan &plan,
                                              RuntimeState *state) {
  CHECK(state != nullptr, common::InternalError, "runtime state is null");
  const ast::BuiltinProcedure *procedure =
      ast::FindBuiltinProcedure(plan.ProcedureName());
  CHECK(procedure != nullptr, common::InvalidArgumentError,
        "unknown procedure: " + plan.ProcedureName());
  CHECK(plan.Arguments().size() == procedure->argument_count,
        common::InvalidArgumentError,
        procedure->name + "() received an invalid argument count");
  CHECK(procedure->read_only && plan.ReadOnly(), common::InvalidArgumentError,
        "write procedure calls are not supported");
  for (const auto &item : plan.YieldItems()) {
    const std::string &field =
        item.result_field.has_value() ? *item.result_field : item.variable;
    CHECK(ast::FindBuiltinProcedureYield(*procedure, field) != nullptr,
          common::InvalidArgumentError,
          "unknown yield field for " + procedure->name + ": " + field);
  }
  std::set<std::string> values;
  if (procedure->kind == ast::BuiltinProcedureKind::kLabels ||
      procedure->kind == ast::BuiltinProcedureKind::kPropertyKeys) {
    EntityIdCursor *nodes =
        state->TrackCursor(state->graph_reader->ScanNodeIds());
    while (nodes->Next()) {
      state->CheckCancelled();
      const Node &node = *state->graph_reader->NodeById(nodes->Id());
      if (procedure->kind == ast::BuiltinProcedureKind::kLabels) {
        values.insert(node.labels.begin(), node.labels.end());
      } else {
        for (const auto &[key, value] : node.properties) {
          (void)value;
          values.insert(key);
        }
      }
    }
    state->ReleaseCursor(nodes);
  }
  if (procedure->kind == ast::BuiltinProcedureKind::kRelationshipTypes ||
      procedure->kind == ast::BuiltinProcedureKind::kPropertyKeys) {
    EntityIdCursor *relationships =
        state->TrackCursor(state->graph_reader->ScanRelationshipIds());
    while (relationships->Next()) {
      state->CheckCancelled();
      const Relationship &relationship =
          *state->graph_reader->RelationshipById(relationships->Id());
      if (procedure->kind == ast::BuiltinProcedureKind::kRelationshipTypes) {
        if (!relationship.type.empty()) {
          values.insert(relationship.type);
        }
      } else {
        for (const auto &[key, value] : relationship.properties) {
          (void)value;
          values.insert(key);
        }
      }
    }
    state->ReleaseCursor(relationships);
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

Storage &RequireStorage(RuntimeState *state) {
  CHECK(state != nullptr && state->storage != nullptr,
        common::InvalidArgumentError, "write execution requires storage");
  return *state->storage;
}

Value::Map EvaluatePropertyMap(const ir::PatternPropertyMap &property_map,
                               const SlottedRow &row,
                               std::string_view operation,
                               RuntimeState *state) {
  if (property_map.parameter != nullptr) {
    Value value = Evaluate(*property_map.parameter, row, {}, *state);
    CHECK(value.IsMap(), common::InvalidArgumentError,
          std::string(operation) + " properties parameter must be a map");
    return value.AsMap();
  }
  Value::Map properties;
  for (const auto &entry : property_map.entries) {
    CHECK(entry.value != nullptr, common::InvalidArgumentError,
          std::string(operation) + " property value is null");
    properties[entry.key] = Evaluate(*entry.value, row, {}, *state);
  }
  return properties;
}

void ApplySetPattern(const ir::SetMutatingPattern &pattern, SlottedRow *row,
                     RuntimeState *state) {
  CHECK(row != nullptr, common::InternalError, "write row is null");
  Storage &storage = RequireStorage(state);
  CHECK(pattern.entity != nullptr, common::InvalidArgumentError,
        "SET entity expression is null");
  const Value entity = Evaluate(*pattern.entity, *row, {}, *state);
  if (entity.IsNull()) {
    return;
  }
  if (pattern.kind == ir::SetMutatingPatternKind::kSetLabels) {
    CHECK(entity.IsNode(), common::InvalidArgumentError,
          "SET labels target is not a node");
    storage.SetLabels(entity.AsNode().id, pattern.labels);
    return;
  }
  CHECK(pattern.value != nullptr, common::InvalidArgumentError,
        "SET value expression is null");
  Value value = Evaluate(*pattern.value, *row, {}, *state);
  if (pattern.kind == ir::SetMutatingPatternKind::kSetProperty) {
    if (entity.IsNode()) {
      storage.SetNodeProperty(entity.AsNode().id, pattern.property_key,
                              std::move(value));
    } else if (entity.IsRelationship()) {
      storage.SetRelationshipProperty(entity.AsRelationship().id,
                                      pattern.property_key, std::move(value));
    } else {
      THROW(common::InvalidArgumentError,
            "SET property target is not an entity");
    }
    return;
  }
  CHECK(value.IsMap(), common::InvalidArgumentError,
        "SET properties requires a map value");
  const bool include_existing =
      pattern.kind ==
      ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap;
  if (entity.IsNode()) {
    storage.SetNodeProperties(entity.AsNode().id, std::move(value.AsMap()),
                              include_existing);
  } else if (entity.IsRelationship()) {
    storage.SetRelationshipProperties(
        entity.AsRelationship().id, std::move(value.AsMap()), include_existing);
  } else {
    THROW(common::InvalidArgumentError,
          "SET properties target is not an entity");
  }
}

void ApplySetPatterns(const std::vector<ir::SetMutatingPattern> &patterns,
                      SlottedRow *row, RuntimeState *state) {
  for (const auto &pattern : patterns) {
    ApplySetPattern(pattern, row, state);
  }
}

void ExecuteCreatePattern(const ir::CreatePattern &pattern, SlottedRow *row,
                          RuntimeState *state, bool reject_null_properties) {
  CHECK(row != nullptr, common::InternalError, "create row is null");
  Storage &storage = RequireStorage(state);
  for (const auto &command : pattern.commands) {
    state->CheckCancelled();
    if (command.kind == ir::CreateEntityKind::kNode) {
      const auto &node_pattern = pattern.nodes.at(command.index);
      Value::Map properties = EvaluatePropertyMap(
          node_pattern.properties, *row,
          reject_null_properties ? "MERGE node" : "CREATE node", state);
      if (reject_null_properties) {
        for (const auto &[key, value] : properties) {
          (void)key;
          CHECK(!value.IsNull(), common::InvalidArgumentError,
                "MERGE node property value is null");
        }
      }
      row->Set(node_pattern.variable,
               Value(storage.CreateNode(node_pattern.labels,
                                        std::move(properties))));
      continue;
    }
    const auto &relationship = pattern.relationships.at(command.index);
    const std::int64_t left = NodeId(*row, relationship.left_node, *state);
    const std::int64_t right = NodeId(*row, relationship.right_node, *state);
    CHECK(left >= 0 && right >= 0, common::InvalidArgumentError,
          "CREATE relationship endpoints must be nodes");
    Value::Map properties = EvaluatePropertyMap(
        relationship.properties, *row,
        reject_null_properties ? "MERGE relationship" : "CREATE relationship",
        state);
    if (reject_null_properties) {
      for (const auto &[key, value] : properties) {
        (void)key;
        CHECK(!value.IsNull(), common::InvalidArgumentError,
              "MERGE relationship property value is null");
      }
    }
    row->Set(
        relationship.variable,
        Value(storage.CreateRelationship(
            left, right,
            relationship.types.empty() ? std::string() : relationship.types[0],
            std::move(properties))));
  }
}

Value EvaluateProjectionItem(const ir::LogicalProjectionItem &item,
                             const SlottedRow &row, RuntimeState *state) {
  if (item.passthrough) {
    return row.Get(item.alias, *state->graph_reader);
  }
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "projection expression is null");
  return Evaluate(*item.expression, row, item.precomputed_expressions, *state);
}

std::size_t EstimatedKeyHeapUsage(const CompositeValueKey &key) {
  std::size_t bytes = key.values.capacity() * sizeof(Value);
  for (const Value &value : key.values) {
    const std::size_t value_bytes = EstimatedValueHeapUsage(value);
    bytes += value_bytes > sizeof(Value) ? value_bytes - sizeof(Value) : 0U;
  }
  return bytes;
}

std::vector<Value> EvaluateGroupingValues(
    const std::vector<ir::LogicalProjectionItem> &items, const SlottedRow &row,
    RuntimeState *state) {
  std::vector<Value> values;
  values.reserve(items.size());
  for (const auto &item : items) {
    values.push_back(EvaluateProjectionItem(item, row, state));
  }
  return values;
}

std::int64_t CheckedCount(std::size_t size) {
  CHECK(size <=
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()),
        common::InvalidArgumentError, "aggregation count overflow");
  return static_cast<std::int64_t>(size);
}

enum class AggregateAccumulatorKind {
  kCountStar,
  kCount,
  kCollect,
  kSum,
  kAverage,
  kMinimum,
  kMaximum,
  kPercentileContinuous,
  kPercentileDiscrete,
};

struct AggregateAccumulator {
  const ir::LogicalProjectionItem *item = nullptr;
  const ast::FunctionInvocation *function = nullptr;
  const ast::Expression *argument = nullptr;
  const ast::Expression *percentile_argument = nullptr;
  AggregateAccumulatorKind kind = AggregateAccumulatorKind::kCountStar;
  bool distinct = false;
  std::size_t count = 0;
  std::int64_t integer_sum = 0;
  double floating_sum = 0.0;
  bool integral_sum = true;
  std::optional<Value> best;
  std::vector<Value> values;
  std::unordered_set<Value, ValueHash, ValueEqual> distinct_values;
  std::optional<double> percentile;
  std::optional<std::string> percentile_error;
  bool percentile_null = false;
  std::size_t values_reserved_bytes = 0;
  std::size_t distinct_reserved_bytes = 0;
};

AggregateAccumulator CreateAggregateAccumulator(
    const ir::LogicalProjectionItem &item) {
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "aggregation expression is null");
  AggregateAccumulator accumulator{.item = &item};
  if (item.expression->Is(ast::ASTNodeType::kCountStarExpression)) {
    accumulator.kind = AggregateAccumulatorKind::kCountStar;
    return accumulator;
  }

  CHECK(item.expression->Is(ast::ASTNodeType::kFunctionInvocation),
        common::InvalidArgumentError, "unsupported aggregation expression");
  const auto &function =
      ast::CastAst<ast::FunctionInvocation>(*item.expression);
  const ast::BuiltinFunction *builtin =
      ast::FindBuiltinFunction(function.function_name);
  CHECK(builtin != nullptr && builtin->aggregate, common::InvalidArgumentError,
        "function is not an aggregate: " + function.function_name);
  CHECK(!function.arguments.empty() && function.arguments[0] != nullptr,
        common::InvalidArgumentError,
        function.function_name + "() argument is null");
  accumulator.function = &function;
  accumulator.argument = function.arguments[0].get();
  accumulator.distinct = function.distinct;

  switch (builtin->kind) {
    case ast::BuiltinFunctionKind::kCollect:
      accumulator.kind = AggregateAccumulatorKind::kCollect;
      break;
    case ast::BuiltinFunctionKind::kCount:
      accumulator.kind = AggregateAccumulatorKind::kCount;
      break;
    case ast::BuiltinFunctionKind::kSum:
      accumulator.kind = AggregateAccumulatorKind::kSum;
      break;
    case ast::BuiltinFunctionKind::kAverage:
      accumulator.kind = AggregateAccumulatorKind::kAverage;
      break;
    case ast::BuiltinFunctionKind::kMinimum:
      accumulator.kind = AggregateAccumulatorKind::kMinimum;
      break;
    case ast::BuiltinFunctionKind::kMaximum:
      accumulator.kind = AggregateAccumulatorKind::kMaximum;
      break;
    case ast::BuiltinFunctionKind::kPercentileContinuous:
      accumulator.kind = AggregateAccumulatorKind::kPercentileContinuous;
      break;
    case ast::BuiltinFunctionKind::kPercentileDiscrete:
      accumulator.kind = AggregateAccumulatorKind::kPercentileDiscrete;
      break;
    default:
      THROW(common::InternalError,
            "unsupported aggregate function: " + function.function_name);
  }
  if (accumulator.kind == AggregateAccumulatorKind::kPercentileContinuous ||
      accumulator.kind == AggregateAccumulatorKind::kPercentileDiscrete) {
    CHECK(function.arguments.size() == 2 && function.arguments[1] != nullptr,
          common::InvalidArgumentError,
          function.function_name + "() percentile argument is null");
    accumulator.percentile_argument = function.arguments[1].get();
  }
  return accumulator;
}

std::size_t EstimatedStoredValueHeapUsage(const Value &value) {
  const std::size_t bytes = EstimatedValueHeapUsage(value);
  return bytes > sizeof(Value) ? bytes - sizeof(Value) : 0U;
}

void ReleaseAccumulatorMemory(std::size_t *accumulator_bytes,
                              RuntimeState *state,
                              std::size_t *reserved_bytes) {
  CHECK(accumulator_bytes != nullptr && state != nullptr &&
            reserved_bytes != nullptr && *accumulator_bytes <= *reserved_bytes,
        common::InternalError, "aggregate memory accounting is invalid");
  state->memory_tracker.Release(*accumulator_bytes);
  *reserved_bytes -= *accumulator_bytes;
  *accumulator_bytes = 0;
}

void ReplaceAccumulatorValue(AggregateAccumulator *accumulator, Value value,
                             RuntimeState *state, std::size_t *reserved_bytes) {
  CHECK(accumulator != nullptr && state != nullptr && reserved_bytes != nullptr,
        common::InternalError, "aggregate accumulator state is null");
  const std::size_t old_bytes =
      accumulator->best.has_value()
          ? EstimatedStoredValueHeapUsage(*accumulator->best)
          : 0U;
  const std::size_t new_bytes = EstimatedStoredValueHeapUsage(value);
  if (new_bytes > old_bytes) {
    state->memory_tracker.Reserve(new_bytes - old_bytes);
    *reserved_bytes += new_bytes - old_bytes;
  }
  accumulator->best.emplace(std::move(value));
  if (old_bytes > new_bytes) {
    state->memory_tracker.Release(old_bytes - new_bytes);
    *reserved_bytes -= old_bytes - new_bytes;
  }
}

const Value *DistinctAggregateValue(AggregateAccumulator *accumulator,
                                    Value *value, RuntimeState *state,
                                    std::size_t *reserved_bytes) {
  CHECK(accumulator != nullptr && value != nullptr && state != nullptr &&
            reserved_bytes != nullptr,
        common::InternalError, "aggregate distinct state is null");
  if (!accumulator->distinct) {
    return value;
  }
  auto [position, inserted] =
      accumulator->distinct_values.insert(std::move(*value));
  if (!inserted) {
    return nullptr;
  }
  const std::size_t bytes = EstimatedValueHeapUsage(*position);
  state->memory_tracker.Reserve(bytes);
  accumulator->distinct_reserved_bytes += bytes;
  *reserved_bytes += bytes;
  return &*position;
}

void AppendAccumulatorValue(AggregateAccumulator *accumulator,
                            const Value &value, RuntimeState *state,
                            std::size_t *reserved_bytes) {
  CHECK(accumulator != nullptr && state != nullptr && reserved_bytes != nullptr,
        common::InternalError, "aggregate value buffer state is null");
  const std::size_t old_capacity = accumulator->values.capacity();
  accumulator->values.push_back(value);
  const std::size_t new_capacity = accumulator->values.capacity();
  const std::size_t bytes =
      (new_capacity - old_capacity) * sizeof(Value) +
      EstimatedStoredValueHeapUsage(accumulator->values.back());
  state->memory_tracker.Reserve(bytes);
  accumulator->values_reserved_bytes += bytes;
  *reserved_bytes += bytes;
}

bool IsPercentile(const AggregateAccumulator &accumulator) {
  return accumulator.kind == AggregateAccumulatorKind::kPercentileContinuous ||
         accumulator.kind == AggregateAccumulatorKind::kPercentileDiscrete;
}

void UpdatePercentileParameter(AggregateAccumulator *accumulator,
                               const SlottedRow &row, RuntimeState *state) {
  CHECK(accumulator != nullptr && accumulator->function != nullptr &&
            accumulator->percentile_argument != nullptr && state != nullptr,
        common::InternalError, "percentile accumulator is incomplete");
  if (accumulator->percentile_null ||
      accumulator->percentile_error.has_value()) {
    return;
  }

  Value current;
  try {
    current = Evaluate(*accumulator->percentile_argument, row,
                       accumulator->item->precomputed_expressions, *state);
  } catch (const common::InvalidArgumentError &error) {
    accumulator->percentile_error = error.Message();
    return;
  }
  if (current.IsNull()) {
    accumulator->percentile_null = true;
    return;
  }
  if (!IsNumeric(current)) {
    accumulator->percentile_error = accumulator->function->function_name +
                                    "() expects a numeric percentile";
    return;
  }
  const double number = AsDoubleValue(current);
  if (!std::isfinite(number) || number < 0.0 || number > 1.0) {
    accumulator->percentile_error = accumulator->function->function_name +
                                    "() percentile must be between 0.0 and 1.0";
    return;
  }
  if (accumulator->percentile.has_value() &&
      *accumulator->percentile != number) {
    accumulator->percentile_error =
        accumulator->function->function_name +
        "() percentile must be constant within a group";
    return;
  }
  accumulator->percentile = number;
}

void UpdateAggregateAccumulator(AggregateAccumulator *accumulator,
                                const SlottedRow &row, RuntimeState *state,
                                std::size_t *reserved_bytes) {
  CHECK(accumulator != nullptr && accumulator->item != nullptr &&
            state != nullptr,
        common::InternalError, "aggregate accumulator is incomplete");
  if (accumulator->kind == AggregateAccumulatorKind::kCountStar) {
    ++accumulator->count;
    return;
  }

  CHECK(accumulator->argument != nullptr, common::InternalError,
        "aggregate accumulator argument is null");
  Value value = Evaluate(*accumulator->argument, row,
                         accumulator->item->precomputed_expressions, *state);
  if (IsPercentile(*accumulator)) {
    UpdatePercentileParameter(accumulator, row, state);
  }
  if (value.IsNull()) {
    return;
  }
  const Value *aggregate_value =
      DistinctAggregateValue(accumulator, &value, state, reserved_bytes);
  if (aggregate_value == nullptr) {
    return;
  }

  switch (accumulator->kind) {
    case AggregateAccumulatorKind::kCount:
      ++accumulator->count;
      return;
    case AggregateAccumulatorKind::kCollect:
      AppendAccumulatorValue(accumulator, *aggregate_value, state,
                             reserved_bytes);
      return;
    case AggregateAccumulatorKind::kSum: {
      CHECK(IsNumeric(*aggregate_value), common::InvalidArgumentError,
            accumulator->function->function_name + "() expects numeric values");
      if (aggregate_value->IsInteger() && accumulator->integral_sum) {
        const std::int64_t addend = aggregate_value->AsInteger();
        CHECK((addend >= 0 &&
               accumulator->integer_sum <=
                   std::numeric_limits<std::int64_t>::max() - addend) ||
                  (addend < 0 &&
                   accumulator->integer_sum >=
                       std::numeric_limits<std::int64_t>::min() - addend),
              common::InvalidArgumentError, "integer sum overflow");
        accumulator->integer_sum += addend;
        return;
      }
      if (accumulator->integral_sum) {
        accumulator->floating_sum =
            static_cast<double>(accumulator->integer_sum);
        accumulator->integral_sum = false;
      }
      accumulator->floating_sum += AsDoubleValue(*aggregate_value);
      return;
    }
    case AggregateAccumulatorKind::kAverage:
      CHECK(IsNumeric(*aggregate_value), common::InvalidArgumentError,
            accumulator->function->function_name + "() expects numeric values");
      accumulator->floating_sum += AsDoubleValue(*aggregate_value);
      ++accumulator->count;
      return;
    case AggregateAccumulatorKind::kMinimum:
    case AggregateAccumulatorKind::kMaximum: {
      const bool minimum =
          accumulator->kind == AggregateAccumulatorKind::kMinimum;
      if (!accumulator->best.has_value() ||
          (minimum && ValueLess(*aggregate_value, *accumulator->best)) ||
          (!minimum && ValueLess(*accumulator->best, *aggregate_value))) {
        ReplaceAccumulatorValue(accumulator, *aggregate_value, state,
                                reserved_bytes);
      }
      return;
    }
    case AggregateAccumulatorKind::kPercentileContinuous:
    case AggregateAccumulatorKind::kPercentileDiscrete:
      CHECK(IsNumeric(*aggregate_value), common::InvalidArgumentError,
            accumulator->function->function_name + "() expects numeric values");
      AppendAccumulatorValue(accumulator, *aggregate_value, state,
                             reserved_bytes);
      return;
    case AggregateAccumulatorKind::kCountStar:
      break;
  }
  THROW(common::InternalError, "unexpected aggregate accumulator kind");
}

void ReleaseDistinctValues(AggregateAccumulator *accumulator,
                           RuntimeState *state, std::size_t *reserved_bytes) {
  std::unordered_set<Value, ValueHash, ValueEqual> empty;
  accumulator->distinct_values.swap(empty);
  ReleaseAccumulatorMemory(&accumulator->distinct_reserved_bytes, state,
                           reserved_bytes);
}

void ReleaseAccumulatorValues(AggregateAccumulator *accumulator,
                              RuntimeState *state,
                              std::size_t *reserved_bytes) {
  accumulator->values = {};
  ReleaseAccumulatorMemory(&accumulator->values_reserved_bytes, state,
                           reserved_bytes);
}

Value TakeAccumulatorBest(AggregateAccumulator *accumulator,
                          RuntimeState *state, std::size_t *reserved_bytes) {
  if (!accumulator->best.has_value()) {
    return Value::Null();
  }
  const std::size_t bytes = EstimatedStoredValueHeapUsage(*accumulator->best);
  Value result = std::move(*accumulator->best);
  accumulator->best.reset();
  state->memory_tracker.Release(bytes);
  *reserved_bytes -= bytes;
  return result;
}

Value TakeCollectedValues(AggregateAccumulator *accumulator,
                          RuntimeState *state, std::size_t *reserved_bytes) {
  Value::List values = std::move(accumulator->values);
  accumulator->values = {};
  ReleaseAccumulatorMemory(&accumulator->values_reserved_bytes, state,
                           reserved_bytes);
  return Value(std::move(values));
}

Value FinalizePercentile(AggregateAccumulator *accumulator, RuntimeState *state,
                         std::size_t *reserved_bytes) {
  if (accumulator->values.empty()) {
    ReleaseAccumulatorValues(accumulator, state, reserved_bytes);
    return Value::Null();
  }
  if (accumulator->percentile_error.has_value()) {
    THROW(common::InvalidArgumentError, *accumulator->percentile_error);
  }
  if (accumulator->percentile_null) {
    ReleaseAccumulatorValues(accumulator, state, reserved_bytes);
    return Value::Null();
  }
  CHECK(accumulator->percentile.has_value(), common::InternalError,
        "percentile accumulator parameter is missing");

  std::sort(accumulator->values.begin(), accumulator->values.end(), ValueLess);
  Value result;
  if (accumulator->kind == AggregateAccumulatorKind::kPercentileDiscrete) {
    const double rank =
        std::ceil(*accumulator->percentile *
                  static_cast<double>(accumulator->values.size()));
    const std::size_t index =
        rank <= 1.0 ? 0 : static_cast<std::size_t>(rank) - 1;
    result =
        accumulator->values[std::min(index, accumulator->values.size() - 1)];
  } else {
    const double position = *accumulator->percentile *
                            static_cast<double>(accumulator->values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    result = Value(std::lerp(AsDoubleValue(accumulator->values[lower]),
                             AsDoubleValue(accumulator->values[upper]),
                             position - lower));
  }
  ReleaseAccumulatorValues(accumulator, state, reserved_bytes);
  return result;
}

Value FinalizeAggregateAccumulator(AggregateAccumulator *accumulator,
                                   RuntimeState *state,
                                   std::size_t *reserved_bytes) {
  CHECK(accumulator != nullptr && accumulator->item != nullptr &&
            state != nullptr && reserved_bytes != nullptr,
        common::InternalError, "aggregate accumulator is incomplete");
  Value result;
  switch (accumulator->kind) {
    case AggregateAccumulatorKind::kCountStar:
    case AggregateAccumulatorKind::kCount:
      result = Value(CheckedCount(accumulator->count));
      break;
    case AggregateAccumulatorKind::kCollect:
      result = TakeCollectedValues(accumulator, state, reserved_bytes);
      break;
    case AggregateAccumulatorKind::kSum:
      result = accumulator->integral_sum ? Value(accumulator->integer_sum)
                                         : Value(accumulator->floating_sum);
      break;
    case AggregateAccumulatorKind::kAverage:
      result = accumulator->count == 0
                   ? Value::Null()
                   : Value(accumulator->floating_sum /
                           static_cast<double>(accumulator->count));
      break;
    case AggregateAccumulatorKind::kMinimum:
    case AggregateAccumulatorKind::kMaximum:
      result = TakeAccumulatorBest(accumulator, state, reserved_bytes);
      break;
    case AggregateAccumulatorKind::kPercentileContinuous:
    case AggregateAccumulatorKind::kPercentileDiscrete:
      result = FinalizePercentile(accumulator, state, reserved_bytes);
      break;
  }
  ReleaseDistinctValues(accumulator, state, reserved_bytes);
  return result;
}

int CompareValues(const Value &left, const Value &right) {
  if (ValuesEqual(left, right)) {
    return 0;
  }
  if (IsNumeric(left) && IsNumeric(right)) {
    const bool left_nan = left.IsDouble() && std::isnan(left.AsDouble());
    const bool right_nan = right.IsDouble() && std::isnan(right.AsDouble());
    if (left_nan || right_nan) {
      return left_nan == right_nan ? 0 : (left_nan ? 1 : -1);
    }
  }
  const bool left_less = ValueLess(left, right);
  const bool right_less = ValueLess(right, left);
  if (left_less != right_less) {
    return left_less ? -1 : 1;
  }
  return 0;
}

std::unique_ptr<EntityIdCursor> ExpandCursor(const GraphReader &graph_reader,
                                             std::int64_t node_id,
                                             ir::ExpandDirection direction) {
  if (direction == ir::ExpandDirection::kOutgoing) {
    return graph_reader.OutgoingRelationshipIds(node_id);
  }
  if (direction == ir::ExpandDirection::kIncoming) {
    return graph_reader.IncomingRelationshipIds(node_id);
  }
  return graph_reader.RelationshipIdsConnectedTo(node_id);
}

class OperatorFactory;

std::optional<std::int64_t> SeekId(const Value &value) {
  if (value.IsInteger()) {
    return value.AsInteger();
  }
  if (value.IsDouble()) {
    const double number = value.AsDouble();
    if (std::isfinite(number) && std::trunc(number) == number &&
        static_cast<long double>(number) >=
            std::numeric_limits<std::int64_t>::min() &&
        static_cast<long double>(number) <=
            std::numeric_limits<std::int64_t>::max()) {
      return static_cast<std::int64_t>(number);
    }
  }
  return std::nullopt;
}

IndexRange EvaluateIndexRange(const std::vector<PhysicalExpression> &predicates,
                              std::string_view variable,
                              std::string_view property_key,
                              const SlottedRow &argument, RuntimeState &state) {
  auto is_property = [&](const ast::Expression *expression) {
    const auto *property = ir::AsPropertyExpression(expression);
    const auto *owner = property == nullptr
                            ? nullptr
                            : ir::AsVariableExpression(property->object.get());
    return owner != nullptr && owner->name == variable &&
           property->property_key == property_key;
  };
  IndexRange range;
  for (const auto &predicate : predicates) {
    const auto *expression = ir::UnwrapParenthesized(predicate.Expression());
    CHECK(expression != nullptr, common::InternalError,
          "index range predicate is null");
    if (expression->Is(ast::ASTNodeType::kStringPredicateExpression)) {
      const auto &prefix =
          *ast::CastAst<ast::StringPredicateExpression>(expression);
      CHECK(prefix.op == "STARTS WITH" && is_property(prefix.left.get()) &&
                prefix.right != nullptr,
            common::InternalError, "invalid index prefix predicate");
      range.prefix = Evaluate(*prefix.right, argument,
                              predicate.PrecomputedExpressions(), state);
      continue;
    }
    CHECK(expression->Is(ast::ASTNodeType::kComparisonExpression),
          common::InternalError, "invalid index range predicate");
    const auto &comparison =
        *ast::CastAst<ast::ComparisonExpression>(expression);
    const bool reversed = !is_property(comparison.left.get());
    CHECK((!reversed || is_property(comparison.right.get())) &&
              (comparison.op == "<" || comparison.op == "<=" ||
               comparison.op == ">" || comparison.op == ">="),
          common::InternalError, "invalid index range comparison");
    const auto *bound =
        reversed ? comparison.left.get() : comparison.right.get();
    CHECK(bound != nullptr, common::InternalError, "index bound is null");
    auto &bounds = (comparison.op.front() == '>') != reversed
                       ? range.lower_bounds
                       : range.upper_bounds;
    bounds.push_back(
        {.value = Evaluate(*bound, argument, predicate.PrecomputedExpressions(),
                           state),
         .inclusive = comparison.op.size() == 2});
  }
  return range;
}

class AllNodeScanOperator final : public PullOperator {
 public:
  AllNodeScanOperator(const PhysicalPlanNode &node, RuntimeState &state,
                      std::optional<SlottedRow> argument)
      : node_(&node),
        data_(&OperatorData<AllNodeScanOp>(node)),
        state_(&state),
        argument_(std::move(argument)) {
    if (!argument_.has_value()) {
      argument_.emplace(EmptyArgument(node.argument_slots));
    }
  }

  ~AllNodeScanOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (cursor_ == nullptr) {
      cursor_ = state_->TrackCursor(state_->graph_reader->ScanNodeIds());
    }
    while (cursor_->Next()) {
      SlottedRow output(node_->output_slots);
      CopyMappings(*argument_, &output, node_->argument_mapping, *state_);
      if (TryBindEntityId(&output, data_->variable, SlotKind::kNode,
                          cursor_->Id(), *state_->graph_reader)) {
        *row = std::move(output);
        return true;
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (cursor_ != nullptr) {
      state_->ReleaseCursor(cursor_);
      cursor_ = nullptr;
    }
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const AllNodeScanOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::optional<SlottedRow> argument_;
  EntityIdCursor *cursor_ = nullptr;
  bool closed_ = false;
};

SlottedRow LeafArgument(const PhysicalPlanNode &node,
                        std::optional<SlottedRow> argument) {
  return argument.has_value() ? std::move(*argument)
                              : EmptyArgument(node.argument_slots);
}

class ArgumentOperator final : public PullOperator {
 public:
  ArgumentOperator(const PhysicalPlanNode &node, RuntimeState &state,
                   std::optional<SlottedRow> argument)
      : node_(&node),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))) {
    (void)OperatorData<ArgumentOp>(node);
  }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    closed_ = true;
    *row = argument_.CopyTo(node_->output_slots, *state_->graph_reader);
    return true;
  }

  void Close() noexcept override { closed_ = true; }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  bool closed_ = false;
};

class NodeScanOperator : public PullOperator {
 public:
  NodeScanOperator(const PhysicalPlanNode &node, RuntimeState &state,
                   std::optional<SlottedRow> argument,
                   const std::string &variable,
                   const std::vector<std::string> *labels_to_verify,
                   const std::vector<PhysicalExpression> *predicates)
      : node_(&node),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))),
        variable_(&variable),
        labels_to_verify_(labels_to_verify),
        predicates_(predicates) {}

  ~NodeScanOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (cursor_ == nullptr) {
      cursor_ = state_->TrackCursor(OpenCursor());
    }
    while (cursor_->Next()) {
      const std::int64_t id = cursor_->Id();
      if (labels_to_verify_ != nullptr &&
          !NodeHasAllLabels(*state_->graph_reader->NodeById(id),
                            *labels_to_verify_)) {
        continue;
      }
      SlottedRow output =
          argument_.CopyTo(node_->output_slots, *state_->graph_reader);
      if (!TryBindEntityId(&output, *variable_, SlotKind::kNode, id,
                           *state_->graph_reader)) {
        continue;
      }
      if (predicates_ != nullptr &&
          !std::all_of(predicates_->begin(), predicates_->end(),
                       [&](const PhysicalExpression &predicate) {
                         return PredicateIsTrue(
                             Evaluate(predicate, output, *state_));
                       })) {
        continue;
      }
      *row = std::move(output);
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (cursor_ != nullptr) {
      state_->ReleaseCursor(cursor_);
      cursor_ = nullptr;
    }
    closed_ = true;
  }

 protected:
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> OpenCursor() = 0;
  [[nodiscard]] const SlottedRow &Argument() const noexcept {
    return argument_;
  }
  [[nodiscard]] RuntimeState &State() const noexcept { return *state_; }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  const std::string *variable_ = nullptr;
  const std::vector<std::string> *labels_to_verify_ = nullptr;
  const std::vector<PhysicalExpression> *predicates_ = nullptr;
  EntityIdCursor *cursor_ = nullptr;
  bool closed_ = false;
};

class NodeByLabelScanOperator final : public NodeScanOperator {
 public:
  NodeByLabelScanOperator(const PhysicalPlanNode &node, RuntimeState &state,
                          std::optional<SlottedRow> argument)
      : NodeScanOperator(node, state, std::move(argument),
                         OperatorData<NodeByLabelScanOp>(node).variable,
                         &OperatorData<NodeByLabelScanOp>(node).labels,
                         nullptr),
        data_(&OperatorData<NodeByLabelScanOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OpenCursor() override {
    return State().graph_reader->ScanNodeIdsByLabels(data_->labels);
  }

  const NodeByLabelScanOp *data_ = nullptr;
};

class NodeIndexSeekOperator final : public NodeScanOperator {
 public:
  NodeIndexSeekOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::optional<SlottedRow> argument)
      : NodeScanOperator(node, state, std::move(argument),
                         OperatorData<NodeIndexSeekOp>(node).variable, nullptr,
                         nullptr),
        data_(&OperatorData<NodeIndexSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OpenCursor() override {
    Value expected = Evaluate(data_->value, Argument(), State());
    return State().graph_reader->FindNodeIdsByIndex(
        data_->labels, data_->property_key, expected);
  }

  const NodeIndexSeekOp *data_ = nullptr;
};

class NodeIndexRangeSeekOperator final : public NodeScanOperator {
 public:
  NodeIndexRangeSeekOperator(const PhysicalPlanNode &node, RuntimeState &state,
                             std::optional<SlottedRow> argument)
      : NodeScanOperator(node, state, std::move(argument),
                         OperatorData<NodeIndexRangeSeekOp>(node).variable,
                         nullptr,
                         &OperatorData<NodeIndexRangeSeekOp>(node).predicates),
        data_(&OperatorData<NodeIndexRangeSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OpenCursor() override {
    const IndexRange range =
        EvaluateIndexRange(data_->predicates, data_->variable,
                           data_->property_key, Argument(), State());
    return State().graph_reader->FindNodeIdsByIndexRange(
        data_->labels, data_->property_key, range);
  }

  const NodeIndexRangeSeekOp *data_ = nullptr;
};

class IdSeekValues final {
 public:
  IdSeekValues(const PhysicalExpression &expression, bool many,
               const SlottedRow &argument, RuntimeState &state)
      : expression_(&expression),
        many_(many),
        argument_(&argument),
        state_(&state) {}

  IdSeekValues(const IdSeekValues &) = delete;
  IdSeekValues &operator=(const IdSeekValues &) = delete;
  ~IdSeekValues() { Close(); }

  [[nodiscard]] std::optional<std::int64_t> Next() {
    Initialize();
    while (next_ < values_.size()) {
      state_->CheckCancelled();
      const std::optional<std::int64_t> id = SeekId(values_[next_++]);
      if (!id.has_value() || *id < 0 || seen_.contains(*id)) {
        continue;
      }
      constexpr std::size_t kSeenIdBytes = 4 * sizeof(std::int64_t);
      state_->memory_tracker.Reserve(kSeenIdBytes);
      reserved_bytes_ += kSeenIdBytes;
      seen_.insert(*id);
      return id;
    }
    return std::nullopt;
  }

  void Close() noexcept {
    values_.clear();
    seen_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
  }

 private:
  void Initialize() {
    if (initialized_) {
      return;
    }
    initialized_ = true;
    Value value = Evaluate(*expression_, *argument_, *state_);
    const std::size_t bytes = EstimatedValueHeapUsage(value);
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    if (many_ && !value.IsNull()) {
      CHECK(value.IsList(), common::InvalidArgumentError, "IN requires a list");
      values_ = value.AsList();
    } else if (!many_) {
      values_.push_back(std::move(value));
    }
  }

  const PhysicalExpression *expression_ = nullptr;
  bool many_ = false;
  const SlottedRow *argument_ = nullptr;
  RuntimeState *state_ = nullptr;
  Value::List values_;
  std::unordered_set<std::int64_t> seen_;
  std::size_t next_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
};

class NodeByIdSeekOperator final : public PullOperator {
 public:
  NodeByIdSeekOperator(const PhysicalPlanNode &node, RuntimeState &state,
                       std::optional<SlottedRow> argument)
      : node_(&node),
        data_(&OperatorData<NodeByIdSeekOp>(node)),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))),
        values_(data_->ids, data_->many, argument_, state) {}

  ~NodeByIdSeekOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    while (const std::optional<std::int64_t> id = values_.Next()) {
      try {
        (void)state_->graph_reader->NodeById(*id);
      } catch (const common::NotFoundError &) {
        continue;
      }
      SlottedRow output =
          argument_.CopyTo(node_->output_slots, *state_->graph_reader);
      if (TryBindEntityId(&output, data_->variable, SlotKind::kNode, *id,
                          *state_->graph_reader)) {
        *row = std::move(output);
        return true;
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    values_.Close();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const NodeByIdSeekOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  IdSeekValues values_;
  bool closed_ = false;
};

bool EmitRelationship(const PhysicalPlanNode &node,
                      const PhysicalRelationshipPattern &pattern,
                      const std::vector<PhysicalExpression> *predicates,
                      const SlottedRow &argument, std::int64_t id, bool reverse,
                      std::optional<std::int64_t> *pending_reverse,
                      SlottedRow *row, RuntimeState &state) {
  const Relationship &relationship = *state.graph_reader->RelationshipById(id);
  if (!RelationshipHasType(relationship, pattern.types)) {
    return false;
  }
  if (pattern.direction == PhysicalExpandDirection::kBoth && !reverse &&
      relationship.start_node_id != relationship.end_node_id) {
    *pending_reverse = id;
  }
  const std::int64_t from_id =
      pattern.direction == PhysicalExpandDirection::kIncoming
          ? relationship.end_node_id
          : (reverse ? relationship.end_node_id : relationship.start_node_id);
  const std::int64_t to_id =
      pattern.direction == PhysicalExpandDirection::kIncoming
          ? relationship.start_node_id
          : (reverse ? relationship.start_node_id : relationship.end_node_id);
  SlottedRow output = argument.CopyTo(node.output_slots, *state.graph_reader);
  if (!TryBindEntityId(&output, pattern.from_node, SlotKind::kNode, from_id,
                       *state.graph_reader) ||
      !TryBindEntityId(&output, pattern.relationship, SlotKind::kRelationship,
                       relationship.id, *state.graph_reader) ||
      !TryBindEntityId(&output, pattern.to_node, SlotKind::kNode, to_id,
                       *state.graph_reader)) {
    return false;
  }
  if (predicates != nullptr &&
      !std::all_of(predicates->begin(), predicates->end(),
                   [&](const PhysicalExpression &predicate) {
                     return PredicateIsTrue(Evaluate(predicate, output, state));
                   })) {
    return false;
  }
  *row = std::move(output);
  return true;
}

bool EmitPendingRelationship(const PhysicalPlanNode &node,
                             const PhysicalRelationshipPattern &pattern,
                             const std::vector<PhysicalExpression> *predicates,
                             const SlottedRow &argument,
                             std::optional<std::int64_t> *pending_reverse,
                             SlottedRow *row, RuntimeState &state) {
  if (!pending_reverse->has_value()) {
    return false;
  }
  const std::int64_t id = **pending_reverse;
  pending_reverse->reset();
  return EmitRelationship(node, pattern, predicates, argument, id, true,
                          pending_reverse, row, state);
}

class RelationshipScanOperator : public PullOperator {
 public:
  RelationshipScanOperator(const PhysicalPlanNode &node, RuntimeState &state,
                           std::optional<SlottedRow> argument,
                           const PhysicalRelationshipPattern &pattern,
                           const std::vector<PhysicalExpression> *predicates)
      : node_(&node),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))),
        pattern_(&pattern),
        predicates_(predicates) {}

  ~RelationshipScanOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (cursor_ == nullptr) {
      cursor_ = state_->TrackCursor(OpenCursor());
    }
    if (EmitPendingRelationship(*node_, *pattern_, predicates_, argument_,
                                &pending_reverse_, row, *state_)) {
      return true;
    }
    while (cursor_->Next()) {
      if (EmitRelationship(*node_, *pattern_, predicates_, argument_,
                           cursor_->Id(), false, &pending_reverse_, row,
                           *state_) ||
          EmitPendingRelationship(*node_, *pattern_, predicates_, argument_,
                                  &pending_reverse_, row, *state_)) {
        return true;
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (cursor_ != nullptr) {
      state_->ReleaseCursor(cursor_);
      cursor_ = nullptr;
    }
    pending_reverse_.reset();
    closed_ = true;
  }

 protected:
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> OpenCursor() = 0;
  [[nodiscard]] const SlottedRow &Argument() const noexcept {
    return argument_;
  }
  [[nodiscard]] RuntimeState &State() const noexcept { return *state_; }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  const PhysicalRelationshipPattern *pattern_ = nullptr;
  const std::vector<PhysicalExpression> *predicates_ = nullptr;
  EntityIdCursor *cursor_ = nullptr;
  std::optional<std::int64_t> pending_reverse_;
  bool closed_ = false;
};

class RelationshipTypeScanOperator final : public RelationshipScanOperator {
 public:
  RelationshipTypeScanOperator(const PhysicalPlanNode &node,
                               RuntimeState &state,
                               std::optional<SlottedRow> argument)
      : RelationshipScanOperator(
            node, state, std::move(argument),
            OperatorData<RelationshipTypeScanOp>(node).pattern, nullptr),
        data_(&OperatorData<RelationshipTypeScanOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OpenCursor() override {
    return State().graph_reader->ScanRelationshipIdsByTypes(
        data_->pattern.types);
  }

  const RelationshipTypeScanOp *data_ = nullptr;
};

class RelationshipIndexSeekOperator final : public RelationshipScanOperator {
 public:
  RelationshipIndexSeekOperator(const PhysicalPlanNode &node,
                                RuntimeState &state,
                                std::optional<SlottedRow> argument)
      : RelationshipScanOperator(
            node, state, std::move(argument),
            OperatorData<RelationshipIndexSeekOp>(node).pattern, nullptr),
        data_(&OperatorData<RelationshipIndexSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OpenCursor() override {
    Value expected = Evaluate(data_->value, Argument(), State());
    return State().graph_reader->FindRelationshipIdsByIndex(
        data_->pattern.types, data_->property_key, expected);
  }

  const RelationshipIndexSeekOp *data_ = nullptr;
};

class RelationshipIndexRangeSeekOperator final
    : public RelationshipScanOperator {
 public:
  RelationshipIndexRangeSeekOperator(const PhysicalPlanNode &node,
                                     RuntimeState &state,
                                     std::optional<SlottedRow> argument)
      : RelationshipScanOperator(
            node, state, std::move(argument),
            OperatorData<RelationshipIndexRangeSeekOp>(node).pattern,
            &OperatorData<RelationshipIndexRangeSeekOp>(node).predicates),
        data_(&OperatorData<RelationshipIndexRangeSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OpenCursor() override {
    const IndexRange range =
        EvaluateIndexRange(data_->predicates, data_->pattern.relationship,
                           data_->property_key, Argument(), State());
    return State().graph_reader->FindRelationshipIdsByIndexRange(
        data_->pattern.types, data_->property_key, range);
  }

  const RelationshipIndexRangeSeekOp *data_ = nullptr;
};

class RelationshipByIdSeekOperator final : public PullOperator {
 public:
  RelationshipByIdSeekOperator(const PhysicalPlanNode &node,
                               RuntimeState &state,
                               std::optional<SlottedRow> argument)
      : node_(&node),
        data_(&OperatorData<RelationshipByIdSeekOp>(node)),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))),
        values_(data_->ids, data_->many, argument_, state) {}

  ~RelationshipByIdSeekOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (EmitPendingRelationship(*node_, data_->pattern, nullptr, argument_,
                                &pending_reverse_, row, *state_)) {
      return true;
    }
    while (const std::optional<std::int64_t> id = values_.Next()) {
      try {
        (void)state_->graph_reader->RelationshipById(*id);
      } catch (const common::NotFoundError &) {
        continue;
      }
      if (EmitRelationship(*node_, data_->pattern, nullptr, argument_, *id,
                           false, &pending_reverse_, row, *state_) ||
          EmitPendingRelationship(*node_, data_->pattern, nullptr, argument_,
                                  &pending_reverse_, row, *state_)) {
        return true;
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    values_.Close();
    pending_reverse_.reset();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const RelationshipByIdSeekOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  IdSeekValues values_;
  std::optional<std::int64_t> pending_reverse_;
  bool closed_ = false;
};

class VarExpandOperator final : public PullOperator {
 public:
  VarExpandOperator(const PhysicalPlanNode &node, RuntimeState &state,
                    std::unique_ptr<PullOperator> source)
      : node_(&node),
        state_(&state),
        source_(std::move(source)),
        plan_(static_cast<const ir::VarExpandPlan &>(*node.logical)) {
    CHECK(plan_.Length().min.value_or(1) >= 0 &&
              plan_.Length().max.value_or(0) >= 0,
          common::InvalidArgumentError, "negative variable path length");
  }
  ~VarExpandOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    while (!closed_) {
      state_->CheckCancelled();
      if (frames_.empty()) {
        input_.reset();
        SlottedRow input(node_->children[0]->output_slots);
        if (!source_->Next(&input)) {
          Close();
          return false;
        }
        const auto from = NodeId(input, plan_.FromNode(), *state_);
        if (from < 0) {
          continue;
        }
        bound_to_.reset();
        if (input.Slots()->Contains(plan_.ToNode()) &&
            input.IsInitialized(plan_.ToNode())) {
          const auto to = NodeId(input, plan_.ToNode(), *state_);
          if (to < 0) {
            continue;
          }
          bound_to_ = to;
        }
        min_ = static_cast<std::size_t>(plan_.Length().min.value_or(1));
        max_ = plan_.Length().max.has_value()
                   ? static_cast<std::size_t>(*plan_.Length().max)
                   : state_->graph_reader->RelationshipCount();
        if (min_ > max_) {
          continue;
        }
        input_.emplace(std::move(input));
        Push(from);
      }
      auto &frame = frames_.back();
      if (!frame.emitted) {
        frame.emitted = true;
        if (path_.size() >= min_ &&
            (!bound_to_.has_value() || frame.node == *bound_to_)) {
          Value::List relationships;
          for (const auto id : path_) {
            relationships.emplace_back(
                state_->graph_reader->RelationshipById(id));
          }
          auto output =
              input_->CopyTo(node_->output_slots, *state_->graph_reader);
          if (TryBindSlot(&output, plan_.Relationship(),
                          Value(std::move(relationships)),
                          *state_->graph_reader) &&
              TryBindEntityId(&output, plan_.ToNode(), SlotKind::kNode,
                              frame.node, *state_->graph_reader)) {
            *row = std::move(output);
            return true;
          }
        }
      }
      if (path_.size() == max_) {
        Pop();
        continue;
      }
      if (frame.cursor == nullptr) {
        frame.cursor = state_->TrackCursor(
            ExpandCursor(*state_->graph_reader, frame.node, plan_.Direction()));
      }
      bool advanced = false;
      while (frame.cursor->Next()) {
        state_->CheckCancelled();
        const auto id = frame.cursor->Id();
        if (used_.contains(id)) {
          continue;
        }
        const auto &relationship = *state_->graph_reader->RelationshipById(id);
        if (!RelationshipHasType(relationship, plan_.Types())) {
          continue;
        }
        const auto next =
            NextVarExpandNode(relationship, frame.node, plan_.Direction());
        if (next.has_value()) {
          Push(*next);
          used_.insert(id);
          path_.push_back(id);
          advanced = true;
          break;
        }
      }
      if (!advanced) {
        Pop();
      }
    }
    return false;
  }

  void Close() noexcept override {
    for (const auto &frame : frames_) {
      if (frame.cursor != nullptr) {
        state_->ReleaseCursor(frame.cursor);
      }
    }
    frames_.clear();
    path_.clear();
    used_.clear();
    input_.reset();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    source_->Close();
    closed_ = true;
  }

 private:
  struct Frame {
    std::int64_t node;
    EntityIdCursor *cursor = nullptr;
    bool emitted = false;
  };
  static constexpr std::size_t kFrameBytes =
      sizeof(Frame) + 4 * sizeof(std::int64_t);

  void Push(std::int64_t node) {
    state_->memory_tracker.Reserve(kFrameBytes);
    reserved_bytes_ += kFrameBytes;
    frames_.push_back({node});
  }
  void Pop() {
    if (frames_.back().cursor != nullptr) {
      state_->ReleaseCursor(frames_.back().cursor);
    }
    frames_.pop_back();
    if (!path_.empty()) {
      used_.erase(path_.back());
      path_.pop_back();
    }
    state_->memory_tracker.Release(kFrameBytes);
    reserved_bytes_ -= kFrameBytes;
  }
  const PhysicalPlanNode *node_;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> source_;
  const ir::VarExpandPlan &plan_;
  std::optional<SlottedRow> input_;
  std::optional<std::int64_t> bound_to_;
  std::vector<Frame> frames_;
  std::vector<std::int64_t> path_;
  std::unordered_set<std::int64_t> used_;
  std::size_t min_ = 1;
  std::size_t max_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool closed_ = false;
};

class PruningExpandOperator final : public PullOperator {
 public:
  PruningExpandOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> source)
      : node_(&node),
        state_(&state),
        source_(std::move(source)),
        pattern_(static_cast<const ir::PruningVarExpandPlan &>(*node.logical)
                     .Pattern()) {}
  ~PruningExpandOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    while (!closed_) {
      state_->CheckCancelled();
      if (!input_.has_value()) {
        SlottedRow input(node_->children[0]->output_slots);
        if (!source_->Next(&input)) {
          Close();
          return false;
        }
        start_ = NodeId(input, pattern_.left_node, *state_);
        if (start_ < 0) {
          continue;
        }
        input_.emplace(std::move(input));
        max_ = pattern_.length.max.has_value()
                   ? static_cast<std::size_t>(*pattern_.length.max)
                   : state_->graph_reader->RelationshipCount();
        Add(start_, 0);
        start_emitted_ = pattern_.length.min.value_or(1) == 0;
        if (start_emitted_ && Emit(start_, row)) {
          return true;
        }
      }
      while (head_ < queue_.size()) {
        const auto [from, depth] = queue_[head_];
        if (depth == max_) {
          ++head_;
          continue;
        }
        if (cursor_ == nullptr) {
          cursor_ = state_->TrackCursor(
              ExpandCursor(*state_->graph_reader, from,
                           pattern_.direction == ir::Direction::kOutgoing
                               ? ir::ExpandDirection::kOutgoing
                               : ir::ExpandDirection::kIncoming));
        }
        while (cursor_->Next()) {
          state_->CheckCancelled();
          const auto &relationship =
              *state_->graph_reader->RelationshipById(cursor_->Id());
          if (!RelationshipHasType(relationship, pattern_.types)) {
            continue;
          }
          const auto to = pattern_.direction == ir::Direction::kOutgoing
                              ? relationship.end_node_id
                              : relationship.start_node_id;
          if (to == start_ && !start_emitted_) {
            start_emitted_ = true;
            if (Emit(to, row)) {
              return true;
            }
          }
          if (!visited_.contains(to)) {
            Add(to, depth + 1);
            if (Emit(to, row)) {
              return true;
            }
          }
        }
        state_->ReleaseCursor(cursor_);
        cursor_ = nullptr;
        ++head_;
      }
      ClearInput();
    }
    return false;
  }

  void Close() noexcept override {
    if (cursor_ != nullptr) {
      state_->ReleaseCursor(cursor_);
      cursor_ = nullptr;
    }
    ClearInput();
    source_->Close();
    closed_ = true;
  }

 private:
  void Add(std::int64_t node, std::size_t depth) {
    constexpr std::size_t bytes = 6 * sizeof(std::int64_t);
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    visited_.insert(node);
    queue_.emplace_back(node, depth);
  }
  bool Emit(std::int64_t node, SlottedRow *row) {
    auto output = input_->CopyTo(node_->output_slots, *state_->graph_reader);
    if (!TryBindEntityId(&output, pattern_.right_node, SlotKind::kNode, node,
                         *state_->graph_reader)) {
      return false;
    }
    *row = std::move(output);
    return true;
  }
  void ClearInput() {
    input_.reset();
    visited_.clear();
    queue_.clear();
    head_ = 0;
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
  }
  const PhysicalPlanNode *node_;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> source_;
  const ir::PatternRelationship &pattern_;
  std::optional<SlottedRow> input_;
  std::vector<std::pair<std::int64_t, std::size_t>> queue_;
  std::unordered_set<std::int64_t> visited_;
  EntityIdCursor *cursor_ = nullptr;
  std::int64_t start_ = -1;
  std::size_t head_ = 0;
  std::size_t max_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool start_emitted_ = false;
  bool closed_ = false;
};

class PatternOperator final : public PullOperator {
 public:
  PatternOperator(const PhysicalPlanNode &node, RuntimeState &state,
                  std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}
  ~PatternOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    while (!closed_) {
      state_->CheckCancelled();
      if (!input_.has_value()) {
        SlottedRow input(node_->children[0]->output_slots);
        if (!source_->Next(&input)) {
          Close();
          return false;
        }
        input_.emplace(std::move(input));
        matched_ = false;
        if (node_->logical->Type() ==
            ir::LogicalPlanNodeType::kProjectEndpoints) {
          ProjectEndpoints();
        } else {
          const auto &pattern = Optional().Pattern();
          const auto from = NodeId(*input_, pattern.left_node, *state_);
          if (from >= 0) {
            cursor_ = state_->TrackCursor(ExpandCursor(
                *state_->graph_reader, from, Direction(pattern.direction)));
          }
        }
      }
      if (node_->logical->Type() ==
          ir::LogicalPlanNodeType::kProjectEndpoints) {
        const auto &pattern =
            static_cast<const ir::ProjectEndpointsPlan &>(*node_->logical)
                .Pattern();
        while (endpoint_index_ < endpoints_.size()) {
          const auto [from, to] = endpoints_[endpoint_index_++];
          auto output =
              input_->CopyTo(node_->output_slots, *state_->graph_reader);
          if (TryBindEntityId(&output, pattern.left_node, SlotKind::kNode, from,
                              *state_->graph_reader) &&
              TryBindEntityId(&output, pattern.right_node, SlotKind::kNode, to,
                              *state_->graph_reader)) {
            *row = std::move(output);
            return true;
          }
        }
      } else {
        const auto &pattern = Optional().Pattern();
        while (cursor_ != nullptr && cursor_->Next()) {
          state_->CheckCancelled();
          const auto &relationship =
              *state_->graph_reader->RelationshipById(cursor_->Id());
          const auto from = NodeId(*input_, pattern.left_node, *state_);
          const auto to = NextVarExpandNode(relationship, from,
                                            Direction(pattern.direction));
          if (!to.has_value() ||
              !RelationshipHasType(relationship, pattern.types)) {
            continue;
          }
          auto output =
              input_->CopyTo(node_->output_slots, *state_->graph_reader);
          if (!TryBindEntityId(&output, pattern.variable,
                               SlotKind::kRelationship, relationship.id,
                               *state_->graph_reader) ||
              !TryBindEntityId(&output, pattern.right_node, SlotKind::kNode,
                               *to, *state_->graph_reader)) {
            continue;
          }
          bool accepted = true;
          for (const auto *predicate : Optional().Predicates()) {
            if (!PredicateIsTrue(Evaluate(*predicate, output, {}, *state_))) {
              accepted = false;
              break;
            }
          }
          if (accepted) {
            matched_ = true;
            *row = std::move(output);
            return true;
          }
        }
        if (cursor_ != nullptr) {
          state_->ReleaseCursor(cursor_);
          cursor_ = nullptr;
        }
        if (!matched_) {
          auto output =
              input_->CopyTo(node_->output_slots, *state_->graph_reader);
          for (const auto &column : node_->output_slots->Columns()) {
            if (!output.IsInitialized(column)) {
              output.SetNull(column);
            }
          }
          input_.reset();
          *row = std::move(output);
          return true;
        }
      }
      input_.reset();
    }
    return false;
  }

  void Close() noexcept override {
    if (cursor_ != nullptr) {
      state_->ReleaseCursor(cursor_);
      cursor_ = nullptr;
    }
    source_->Close();
    input_.reset();
    closed_ = true;
  }

 private:
  static ir::ExpandDirection Direction(ir::Direction direction) {
    return direction == ir::Direction::kIncoming
               ? ir::ExpandDirection::kIncoming
           : direction == ir::Direction::kOutgoing
               ? ir::ExpandDirection::kOutgoing
               : ir::ExpandDirection::kBoth;
  }

  const ir::OptionalExpandPlan &Optional() const {
    return static_cast<const ir::OptionalExpandPlan &>(*node_->logical);
  }

  void ProjectEndpoints() {
    endpoints_.clear();
    endpoint_index_ = 0;
    const auto &pattern =
        static_cast<const ir::ProjectEndpointsPlan &>(*node_->logical)
            .Pattern();
    const Value value = input_->Get(pattern.variable, *state_->graph_reader);
    if (value.IsNull()) {
      return;
    }
    std::vector<const Relationship *> relationships;
    if (pattern.length.variable) {
      CHECK(value.IsList(), common::InvalidArgumentError,
            "expected a relationship list");
      for (const auto &item : value.AsList()) {
        if (!item.IsRelationship()) {
          return;
        }
        relationships.push_back(&item.AsRelationship());
      }
      const auto length = relationships.size();
      if (length < static_cast<std::size_t>(pattern.length.min.value_or(1)) ||
          (pattern.length.max.has_value() &&
           length > static_cast<std::size_t>(*pattern.length.max))) {
        return;
      }
    } else {
      CHECK(value.IsRelationship(), common::InvalidArgumentError,
            "expected a relationship");
      relationships.push_back(&value.AsRelationship());
    }
    std::unordered_set<std::int64_t> used;
    for (const auto &relationship : relationships) {
      state_->CheckCancelled();
      try {
        (void)state_->graph_reader->RelationshipById(relationship->id);
      } catch (const common::NotFoundError &) {
        return;
      }
      if (!RelationshipHasType(*relationship, pattern.types) ||
          !used.insert(relationship->id).second) {
        return;
      }
    }
    if (relationships.empty()) {
      for (const auto &name : {pattern.left_node, pattern.right_node}) {
        if (input_->Slots()->Contains(name) && input_->IsInitialized(name)) {
          const auto id = NodeId(*input_, name, *state_);
          if (id >= 0) {
            endpoints_.emplace_back(id, id);
          }
          return;
        }
      }
      return;
    }
    const auto &first = *relationships.front();
    std::vector<std::int64_t> starts;
    if (pattern.direction != ir::Direction::kIncoming) {
      starts.push_back(first.start_node_id);
    }
    if (pattern.direction != ir::Direction::kOutgoing &&
        (starts.empty() || starts.front() != first.end_node_id)) {
      starts.push_back(first.end_node_id);
    }
    for (const auto start : starts) {
      auto current = std::optional<std::int64_t>(start);
      for (const auto &relationship : relationships) {
        current = NextVarExpandNode(*relationship, *current,
                                    Direction(pattern.direction));
        if (!current.has_value()) {
          break;
        }
      }
      if (current.has_value()) {
        endpoints_.emplace_back(start, *current);
      }
    }
  }

  const PhysicalPlanNode *node_;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> input_;
  EntityIdCursor *cursor_ = nullptr;
  std::vector<std::pair<std::int64_t, std::int64_t>> endpoints_;
  std::size_t endpoint_index_ = 0;
  bool matched_ = false;
  bool closed_ = false;
};

SlottedRow CopyUnaryOutput(const PhysicalPlanNode &node,
                           const SlottedRow &input, const RuntimeState &state) {
  CHECK(node.child_mappings.size() == 1, common::InternalError,
        "unary physical operator mapping is missing");
  SlottedRow output(node.output_slots);
  CopyMappings(input, &output, node.child_mappings.front(), state);
  return output;
}

std::int64_t EvaluatePaginationCount(const PhysicalExpression &expression,
                                     const SlottedRow *row, RuntimeState *state,
                                     std::string_view name) {
  CHECK(state != nullptr, common::InternalError, "runtime state is null");
  Value value = row != nullptr ? Evaluate(expression, *row, *state)
                               : EvaluateExpression(*expression.Expression(),
                                                    state->context);
  CHECK(value.IsInteger() && value.AsInteger() >= 0,
        common::InvalidArgumentError,
        std::string(name) + " requires a non-negative integer");
  return value.AsInteger();
}

class FilterOperator final : public PullOperator {
 public:
  FilterOperator(const PhysicalPlanNode &node, RuntimeState &state,
                 std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<FilterOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~FilterOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      if (PredicateIsTrue(Evaluate(data_->predicate, input, *state_))) {
        *row = CopyUnaryOutput(*node_, input, *state_);
        return true;
      }
      state_->CheckCancelled();
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const FilterOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  bool closed_ = false;
};

class ProjectionOperator final : public PullOperator {
 public:
  ProjectionOperator(const PhysicalPlanNode &node, RuntimeState &state,
                     std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<ProjectionOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~ProjectionOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (!source_->Next(&input)) {
      Close();
      return false;
    }
    SlottedRow output(node_->output_slots);
    for (const auto &item : data_->items) {
      Value value = item.passthrough
                        ? input.Get(item.alias, *state_->graph_reader)
                        : Evaluate(item.expression, input, *state_);
      output.Set(item.alias, std::move(value));
    }
    *row = std::move(output);
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const ProjectionOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  bool closed_ = false;
};

class SkipOperator final : public PullOperator {
 public:
  SkipOperator(const PhysicalPlanNode &node, RuntimeState &state,
               std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<SkipOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~SkipOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_ && !Initialize()) {
      Close();
      return false;
    }
    while (seen_ < count_) {
      SlottedRow ignored(node_->children[0]->output_slots);
      if (pending_.has_value()) {
        ignored = std::move(*pending_);
        pending_.reset();
      } else if (!source_->Next(&ignored)) {
        Close();
        return false;
      }
      ++seen_;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (pending_.has_value()) {
      input = std::move(*pending_);
      pending_.reset();
    } else if (!source_->Next(&input)) {
      Close();
      return false;
    }
    *row = CopyUnaryOutput(*node_, input, *state_);
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    pending_.reset();
    closed_ = true;
  }

 private:
  bool Initialize() {
    initialized_ = true;
    const SlottedRow *expression_row = nullptr;
    if (data_->count.RequiresInputRow()) {
      SlottedRow first(node_->children[0]->output_slots);
      if (!source_->Next(&first)) {
        return false;
      }
      pending_.emplace(std::move(first));
      expression_row = &*pending_;
    }
    count_ = static_cast<std::uint64_t>(
        EvaluatePaginationCount(data_->count, expression_row, state_, "SKIP"));
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  const SkipOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> pending_;
  std::uint64_t count_ = 0;
  std::uint64_t seen_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class LimitOperator final : public PullOperator {
 public:
  LimitOperator(const PhysicalPlanNode &node, RuntimeState &state,
                std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<LimitOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~LimitOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_ && !Initialize()) {
      Close();
      return false;
    }
    if (emitted_ >= count_) {
      Close();
      return false;
    }
    if (data_->exhaust_child) {
      if (emitted_ >= buffered_rows_.size()) {
        Close();
        return false;
      }
      *row = std::move(buffered_rows_[emitted_++]);
      return true;
    }

    SlottedRow input(node_->children[0]->output_slots);
    if (pending_.has_value()) {
      input = std::move(*pending_);
      pending_.reset();
    } else if (!source_->Next(&input)) {
      Close();
      return false;
    }
    ++emitted_;
    *row = CopyUnaryOutput(*node_, input, *state_);
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    pending_.reset();
    buffered_rows_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  bool Initialize() {
    initialized_ = true;
    if (data_->exhaust_child) {
      SlottedRow input(node_->children[0]->output_slots);
      while (source_->Next(&input)) {
        SlottedRow output = CopyUnaryOutput(*node_, input, *state_);
        const std::size_t bytes = output.EstimatedHeapUsage();
        state_->memory_tracker.Reserve(bytes);
        reserved_bytes_ += bytes;
        buffered_rows_.push_back(std::move(output));
        state_->CheckCancelled();
      }
      source_->Close();
      if (buffered_rows_.empty() && data_->count.RequiresInputRow()) {
        return false;
      }
      const SlottedRow *expression_row =
          data_->count.RequiresInputRow() ? &buffered_rows_.front() : nullptr;
      count_ = static_cast<std::uint64_t>(EvaluatePaginationCount(
          data_->count, expression_row, state_, "LIMIT"));
      return true;
    }

    const SlottedRow *expression_row = nullptr;
    if (data_->count.RequiresInputRow()) {
      SlottedRow first(node_->children[0]->output_slots);
      if (!source_->Next(&first)) {
        return false;
      }
      pending_.emplace(std::move(first));
      expression_row = &*pending_;
    }
    count_ = static_cast<std::uint64_t>(
        EvaluatePaginationCount(data_->count, expression_row, state_, "LIMIT"));
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  const LimitOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> pending_;
  std::vector<SlottedRow> buffered_rows_;
  std::uint64_t count_ = 0;
  std::uint64_t emitted_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class ProduceResultsOperator final : public PullOperator {
 public:
  ProduceResultsOperator(const PhysicalPlanNode &node, RuntimeState &state,
                         std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<ProduceResultsOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~ProduceResultsOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (!source_->Next(&input)) {
      Close();
      return false;
    }
    (void)data_;
    *row = CopyUnaryOutput(*node_, input, *state_);
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const ProduceResultsOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  bool closed_ = false;
};

class StreamingUnaryOperator final : public PullOperator {
 public:
  StreamingUnaryOperator(const PhysicalPlanNode &node, RuntimeState &state,
                         std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~StreamingUnaryOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kExpand:
      case ir::LogicalPlanNodeType::kExpandInto:
        return NextExpand(row);
      case ir::LogicalPlanNodeType::kPathBuild:
      case ir::LogicalPlanNodeType::kProcedureCall:
        return NextBufferedPerInput(row);
      case ir::LogicalPlanNodeType::kUnwind:
        return NextUnwind(row);
      case ir::LogicalPlanNodeType::kAssertIsNode:
        return NextAssertIsNode(row);
      case ir::LogicalPlanNodeType::kSetProperty:
      case ir::LogicalPlanNodeType::kSetProperties:
      case ir::LogicalPlanNodeType::kSetLabels:
      case ir::LogicalPlanNodeType::kRemoveProperty:
      case ir::LogicalPlanNodeType::kRemoveLabels:
      case ir::LogicalPlanNodeType::kCreateNode:
      case ir::LogicalPlanNodeType::kCreateRelationship:
        return NextWrite(row);
      default:
        THROW(common::InternalError, "unsupported streaming operator: " +
                                         std::string(node_->logical->Name()));
    }
  }

  void Close() noexcept override {
    if (relationship_cursor_ != nullptr) {
      state_->ReleaseCursor(relationship_cursor_);
      relationship_cursor_ = nullptr;
    }
    if (source_ != nullptr) {
      source_->Close();
    }
    state_->memory_tracker.Release(buffer_reserved_bytes_);
    state_->memory_tracker.Release(unwind_reserved_bytes_);
    buffer_reserved_bytes_ = 0;
    unwind_reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  bool PullInput(SlottedRow *input) {
    if (!source_->Next(input)) {
      Close();
      return false;
    }
    return true;
  }

  bool NextExpand(SlottedRow *row) {
    while (true) {
      if (relationship_cursor_ != nullptr) {
        while (relationship_cursor_->Next()) {
          const Relationship &relationship =
              *state_->graph_reader->RelationshipById(
                  relationship_cursor_->Id());
          std::string rel_name;
          std::string to_name;
          ir::ExpandDirection direction;
          const std::vector<std::string> *types = nullptr;
          bool into = false;
          if (node_->logical->Type() == ir::LogicalPlanNodeType::kExpand) {
            const auto &expand =
                static_cast<const ir::ExpandPlan &>(*node_->logical);
            rel_name = expand.Relationship();
            to_name = expand.ToNode();
            direction = expand.Direction();
            types = &expand.Types();
          } else {
            const auto &expand =
                static_cast<const ir::ExpandIntoPlan &>(*node_->logical);
            rel_name = expand.Relationship();
            to_name = expand.ToNode();
            direction = expand.Direction();
            types = &expand.Types();
            into = true;
          }
          if (!RelationshipHasType(relationship, *types) ||
              !RelationshipMatchesDirection(relationship, current_from_id_,
                                            direction)) {
            continue;
          }
          const std::int64_t other = OtherNode(relationship, current_from_id_);
          if (into && other != current_to_id_) {
            continue;
          }
          SlottedRow output = current_input_->CopyTo(node_->output_slots,
                                                     *state_->graph_reader);
          if (!TryBindEntityId(&output, rel_name, SlotKind::kRelationship,
                               relationship.id, *state_->graph_reader) ||
              (!into && !TryBindEntityId(&output, to_name, SlotKind::kNode,
                                         other, *state_->graph_reader))) {
            continue;
          }
          *row = std::move(output);
          return true;
        }
        state_->ReleaseCursor(relationship_cursor_);
        relationship_cursor_ = nullptr;
        current_input_.reset();
      }

      SlottedRow input(node_->children[0]->output_slots);
      if (!PullInput(&input)) {
        return false;
      }
      std::string from_name;
      ir::ExpandDirection direction;
      if (node_->logical->Type() == ir::LogicalPlanNodeType::kExpand) {
        const auto &expand =
            static_cast<const ir::ExpandPlan &>(*node_->logical);
        from_name = expand.FromNode();
        direction = expand.Direction();
        current_to_id_ = -1;
      } else {
        const auto &expand =
            static_cast<const ir::ExpandIntoPlan &>(*node_->logical);
        from_name = expand.FromNode();
        direction = expand.Direction();
        current_to_id_ = NodeId(input, expand.ToNode(), *state_);
      }
      current_from_id_ = NodeId(input, from_name, *state_);
      if (current_from_id_ < 0 ||
          (node_->logical->Type() == ir::LogicalPlanNodeType::kExpandInto &&
           current_to_id_ < 0)) {
        continue;
      }
      current_input_.emplace(std::move(input));
      relationship_cursor_ = state_->TrackCursor(
          ExpandCursor(*state_->graph_reader, current_from_id_, direction));
    }
  }

  bool NextBufferedPerInput(SlottedRow *row) {
    while (buffer_index_ >= buffer_.size()) {
      state_->memory_tracker.Release(buffer_reserved_bytes_);
      buffer_reserved_bytes_ = 0;
      buffer_.clear();
      buffer_index_ = 0;
      SlottedRow input(node_->children[0]->output_slots);
      if (!PullInput(&input)) {
        return false;
      }
      if (node_->logical->Type() == ir::LogicalPlanNodeType::kPathBuild) {
        const auto &plan =
            static_cast<const ir::PathBuildPlan &>(*node_->logical);
        SlottedRow output =
            input.CopyTo(node_->output_slots, *state_->graph_reader);
        if (TryBindSlot(&output, plan.PathVariable(),
                        BuildPathValue(plan.Path(), input, state_),
                        *state_->graph_reader)) {
          const std::size_t bytes = output.EstimatedHeapUsage();
          state_->memory_tracker.Reserve(bytes);
          buffer_reserved_bytes_ += bytes;
          buffer_.push_back(std::move(output));
        }
      } else {
        const auto &plan =
            static_cast<const ir::ProcedureCallPlan &>(*node_->logical);
        for (const auto &record : ExecuteProcedure(plan, state_)) {
          SlottedRow output =
              input.CopyTo(node_->output_slots, *state_->graph_reader);
          for (const auto &item : plan.YieldItems()) {
            const std::string &field = item.result_field.has_value()
                                           ? *item.result_field
                                           : item.variable;
            const auto found = record.find(field);
            CHECK(found != record.end(), common::InvalidArgumentError,
                  "unknown yield field for " + plan.ProcedureName() + ": " +
                      field);
            output.Set(item.variable, found->second);
          }
          const std::size_t bytes = output.EstimatedHeapUsage();
          state_->memory_tracker.Reserve(bytes);
          buffer_reserved_bytes_ += bytes;
          buffer_.push_back(std::move(output));
        }
      }
    }
    *row = std::move(buffer_[buffer_index_++]);
    return true;
  }

  bool NextUnwind(SlottedRow *row) {
    const auto &unwind = static_cast<const ir::UnwindPlan &>(*node_->logical);
    while (true) {
      if (unwind_input_.has_value() && unwind_index_ < unwind_values_.size()) {
        SlottedRow output =
            unwind_input_->CopyTo(node_->output_slots, *state_->graph_reader);
        output.Set(unwind.Alias(), unwind_values_[unwind_index_++]);
        *row = std::move(output);
        return true;
      }
      unwind_values_.clear();
      state_->memory_tracker.Release(unwind_reserved_bytes_);
      unwind_reserved_bytes_ = 0;
      unwind_index_ = 0;
      SlottedRow input(node_->children[0]->output_slots);
      if (!PullInput(&input)) {
        return false;
      }
      Value value = Evaluate(*unwind.Expression(), input, {}, *state_);
      if (value.IsNull()) {
        continue;
      }
      if (value.IsList()) {
        unwind_values_ = value.AsList();
      } else {
        unwind_values_.push_back(std::move(value));
      }
      for (const auto &item : unwind_values_) {
        unwind_reserved_bytes_ += EstimatedValueHeapUsage(item);
      }
      state_->memory_tracker.Reserve(unwind_reserved_bytes_);
      unwind_input_.emplace(std::move(input));
    }
  }

  bool NextAssertIsNode(SlottedRow *row) {
    const auto &assertion =
        static_cast<const ir::AssertIsNodePlan &>(*node_->logical);
    SlottedRow input(node_->children[0]->output_slots);
    if (!PullInput(&input)) {
      return false;
    }
    for (const auto &variable : assertion.Variables()) {
      const Value value = input.Get(variable, *state_->graph_reader);
      CHECK(value.IsNull() || value.IsNode(), common::InvalidArgumentError,
            "expected node value: " + variable);
    }
    *row = input.CopyTo(node_->output_slots, *state_->graph_reader);
    return true;
  }

  bool NextWrite(SlottedRow *row) {
    SlottedRow input(node_->children[0]->output_slots);
    if (!PullInput(&input)) {
      return false;
    }
    Storage &storage = RequireStorage(state_);
    SlottedRow output =
        input.CopyTo(node_->output_slots, *state_->graph_reader);
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kCreateNode: {
        const auto &plan =
            static_cast<const ir::CreateNodePlan &>(*node_->logical);
        output.Set(plan.Node().variable,
                   Value(storage.CreateNode(
                       plan.Node().labels,
                       EvaluatePropertyMap(plan.Node().properties, input,
                                           "CREATE node", state_))));
        break;
      }
      case ir::LogicalPlanNodeType::kCreateRelationship: {
        const auto &plan =
            static_cast<const ir::CreateRelationshipPlan &>(*node_->logical);
        const auto &pattern = plan.Relationship();
        const std::int64_t left = NodeId(input, pattern.left_node, *state_);
        const std::int64_t right = NodeId(input, pattern.right_node, *state_);
        CHECK(left >= 0 && right >= 0, common::InvalidArgumentError,
              "CREATE relationship endpoints must be nodes");
        output.Set(pattern.variable,
                   Value(storage.CreateRelationship(
                       left, right,
                       pattern.types.empty() ? std::string() : pattern.types[0],
                       EvaluatePropertyMap(pattern.properties, input,
                                           "CREATE relationship", state_))));
        break;
      }
      case ir::LogicalPlanNodeType::kSetProperty: {
        const auto &plan =
            static_cast<const ir::SetPropertyPlan &>(*node_->logical);
        ApplySetPattern({.kind = ir::SetMutatingPatternKind::kSetProperty,
                         .entity = plan.Entity(),
                         .property_key = plan.PropertyKey(),
                         .value = plan.Value()},
                        &output, state_);
        break;
      }
      case ir::LogicalPlanNodeType::kSetProperties: {
        const auto &plan =
            static_cast<const ir::SetPropertiesPlan &>(*node_->logical);
        ApplySetPattern(
            {.kind =
                 plan.IncludeExisting()
                     ? ir::SetMutatingPatternKind::
                           kSetIncludingPropertiesFromMap
                     : ir::SetMutatingPatternKind::kSetExactPropertiesFromMap,
             .entity = plan.Entity(),
             .value = plan.Value()},
            &output, state_);
        break;
      }
      case ir::LogicalPlanNodeType::kSetLabels: {
        const auto &plan =
            static_cast<const ir::SetLabelsPlan &>(*node_->logical);
        ApplySetPattern({.kind = ir::SetMutatingPatternKind::kSetLabels,
                         .entity = plan.Entity(),
                         .labels = plan.Labels()},
                        &output, state_);
        break;
      }
      case ir::LogicalPlanNodeType::kRemoveProperty: {
        const auto &plan =
            static_cast<const ir::RemovePropertyPlan &>(*node_->logical);
        CHECK(plan.Entity() != nullptr, common::InvalidArgumentError,
              "REMOVE property expression is null");
        const Value entity = Evaluate(*plan.Entity(), output, {}, *state_);
        if (entity.IsNode()) {
          storage.RemoveNodeProperty(entity.AsNode().id, plan.PropertyKey());
        } else if (entity.IsRelationship()) {
          storage.RemoveRelationshipProperty(entity.AsRelationship().id,
                                             plan.PropertyKey());
        } else {
          CHECK(entity.IsNull(), common::InvalidArgumentError,
                "REMOVE property target is not an entity");
        }
        break;
      }
      case ir::LogicalPlanNodeType::kRemoveLabels: {
        const auto &plan =
            static_cast<const ir::RemoveLabelsPlan &>(*node_->logical);
        CHECK(plan.Entity() != nullptr, common::InvalidArgumentError,
              "REMOVE labels expression is null");
        const Value entity = Evaluate(*plan.Entity(), output, {}, *state_);
        if (!entity.IsNull()) {
          CHECK(entity.IsNode(), common::InvalidArgumentError,
                "REMOVE labels target is not a node");
          storage.RemoveLabels(entity.AsNode().id, plan.Labels());
        }
        break;
      }
      default:
        THROW(common::InternalError, "unexpected streaming write operator");
    }
    *row = std::move(output);
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  EntityIdCursor *relationship_cursor_ = nullptr;
  std::optional<SlottedRow> current_input_;
  std::optional<SlottedRow> unwind_input_;
  std::vector<Value> unwind_values_;
  std::vector<SlottedRow> buffer_;
  std::size_t unwind_index_ = 0;
  std::size_t buffer_index_ = 0;
  std::size_t buffer_reserved_bytes_ = 0;
  std::size_t unwind_reserved_bytes_ = 0;
  std::int64_t current_from_id_ = -1;
  std::int64_t current_to_id_ = -1;
  bool closed_ = false;
};

class OrderedDistinctOperator final : public PullOperator {
 public:
  OrderedDistinctOperator(const PhysicalPlanNode &node, RuntimeState &state,
                          std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~OrderedDistinctOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }

    const auto &plan = static_cast<const ir::DistinctPlan &>(*node_->logical);
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      std::vector<Value> values =
          EvaluateGroupingValues(plan.GroupingItems(), input, state_);
      CompositeValueKey key{.values = values};
      if (last_key_.has_value() && ValueEqual{}(*last_key_, key)) {
        continue;
      }
      ReplaceLastKey(std::move(key));

      SlottedRow output(node_->output_slots);
      for (std::size_t index = 0; index < values.size(); ++index) {
        output.Set(plan.GroupingItems()[index].alias, std::move(values[index]));
      }
      *row = std::move(output);
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (source_ != nullptr) {
      source_->Close();
    }
    last_key_.reset();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  void ReplaceLastKey(CompositeValueKey key) {
    const std::size_t bytes = EstimatedKeyHeapUsage(key);
    if (bytes > reserved_bytes_) {
      state_->memory_tracker.Reserve(bytes - reserved_bytes_);
    }
    last_key_ = std::move(key);
    if (reserved_bytes_ > bytes) {
      state_->memory_tracker.Release(reserved_bytes_ - bytes);
    }
    reserved_bytes_ = bytes;
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<CompositeValueKey> last_key_;
  std::size_t reserved_bytes_ = 0;
  bool closed_ = false;
};

class OrderedAggregationOperator final : public PullOperator {
 public:
  OrderedAggregationOperator(const PhysicalPlanNode &node, RuntimeState &state,
                             std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~OrderedAggregationOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_ || finished_) {
      return false;
    }

    const auto &plan =
        static_cast<const ir::AggregationPlan &>(*node_->logical);
    SlottedRow input(node_->children[0]->output_slots);
    CompositeValueKey key;
    if (!TakeFirstInput(plan, &input, &key)) {
      finished_ = true;
      Close();
      return false;
    }
    const std::size_t key_bytes = EstimatedKeyHeapUsage(key);
    state_->memory_tracker.Reserve(key_bytes);
    reserved_bytes_ += key_bytes;

    std::vector<AggregateAccumulator> accumulators;
    accumulators.reserve(plan.AggregationItems().size());
    for (const auto &item : plan.AggregationItems()) {
      accumulators.push_back(CreateAggregateAccumulator(item));
    }

    SlottedRow output(node_->output_slots);
    for (std::size_t index = 0; index < key.values.size(); ++index) {
      output.Set(plan.GroupingItems()[index].alias, key.values[index]);
    }

    while (true) {
      state_->CheckCancelled();
      for (auto &accumulator : accumulators) {
        UpdateAggregateAccumulator(&accumulator, input, state_,
                                   &reserved_bytes_);
      }

      SlottedRow next(node_->children[0]->output_slots);
      if (!source_->Next(&next)) {
        source_exhausted_ = true;
        break;
      }
      CompositeValueKey next_key{
          .values = EvaluateGroupingValues(plan.GroupingItems(), next, state_)};
      if (!ValueEqual{}(key, next_key)) {
        BufferPending(std::move(next), std::move(next_key));
        break;
      }
      input = std::move(next);
    }

    for (auto &accumulator : accumulators) {
      output.Set(
          accumulator.item->alias,
          FinalizeAggregateAccumulator(&accumulator, state_, &reserved_bytes_));
    }
    state_->memory_tracker.Release(key_bytes);
    reserved_bytes_ -= key_bytes;
    *row = std::move(output);
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (source_ != nullptr) {
      source_->Close();
    }
    pending_.reset();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  struct PendingInput {
    SlottedRow row;
    CompositeValueKey key;
    std::size_t reserved_bytes = 0;
  };

  bool TakeFirstInput(const ir::AggregationPlan &plan, SlottedRow *row,
                      CompositeValueKey *key) {
    CHECK(row != nullptr && key != nullptr, common::InternalError,
          "ordered aggregation input is null");
    if (pending_.has_value()) {
      *row = std::move(pending_->row);
      *key = std::move(pending_->key);
      state_->memory_tracker.Release(pending_->reserved_bytes);
      reserved_bytes_ -= pending_->reserved_bytes;
      pending_.reset();
      return true;
    }
    if (source_exhausted_ || !source_->Next(row)) {
      source_exhausted_ = true;
      return false;
    }
    key->values = EvaluateGroupingValues(plan.GroupingItems(), *row, state_);
    return true;
  }

  void BufferPending(SlottedRow row, CompositeValueKey key) {
    const std::size_t bytes =
        row.EstimatedHeapUsage() + EstimatedKeyHeapUsage(key);
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    pending_.emplace(PendingInput{
        .row = std::move(row), .key = std::move(key), .reserved_bytes = bytes});
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<PendingInput> pending_;
  std::size_t reserved_bytes_ = 0;
  bool source_exhausted_ = false;
  bool finished_ = false;
  bool closed_ = false;
};

class PartialSortOperator final : public PullOperator {
 public:
  PartialSortOperator(const PhysicalPlanNode &node, RuntimeState &state,
                      std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~PartialSortOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (next_ >= entries_.size() && !LoadRun()) {
      Close();
      return false;
    }

    Entry &entry = entries_[next_++];
    *row = std::move(entry.row);
    entry.keys = {};
    state_->memory_tracker.Release(entry.reserved_bytes);
    reserved_bytes_ -= entry.reserved_bytes;
    entry.reserved_bytes = 0;
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (source_ != nullptr) {
      source_->Close();
    }
    entries_.clear();
    pending_.reset();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  struct Entry {
    SlottedRow row;
    std::vector<Value> keys;
    std::uint64_t sequence = 0;
    std::size_t reserved_bytes = 0;
  };

  [[nodiscard]] Entry ReadEntry(const SlottedRow &input) {
    const auto &sort = static_cast<const ir::SortPlan &>(*node_->logical);
    std::vector<Value> keys;
    keys.reserve(sort.Items().size());
    for (const auto &item : sort.Items()) {
      CHECK(item.expression != nullptr, common::InvalidArgumentError,
            "sort expression is null");
      keys.push_back(Evaluate(*item.expression, input,
                              item.precomputed_expressions, *state_));
    }
    return {.row = input.CopyTo(node_->output_slots, *state_->graph_reader),
            .keys = std::move(keys),
            .sequence = sequence_++};
  }

  void Retain(Entry *entry) {
    CHECK(entry != nullptr, common::InternalError,
          "partial sort entry is null");
    entry->reserved_bytes = entry->row.EstimatedHeapUsage() +
                            entry->keys.capacity() * sizeof(Value);
    for (const Value &key : entry->keys) {
      entry->reserved_bytes += EstimatedStoredValueHeapUsage(key);
    }
    state_->memory_tracker.Reserve(entry->reserved_bytes);
    reserved_bytes_ += entry->reserved_bytes;
  }

  [[nodiscard]] bool SamePrefix(const Entry &left, const Entry &right) const {
    CHECK(node_->partial_sort_prefix > 0 &&
              node_->partial_sort_prefix <= left.keys.size() &&
              node_->partial_sort_prefix <= right.keys.size(),
          common::InternalError, "partial sort prefix is invalid");
    for (std::size_t index = 0; index < node_->partial_sort_prefix; ++index) {
      if (CompareValues(left.keys[index], right.keys[index]) != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool ComesBefore(const Entry &left, const Entry &right) const {
    const auto &items =
        static_cast<const ir::SortPlan &>(*node_->logical).Items();
    CHECK(left.keys.size() == items.size() && right.keys.size() == items.size(),
          common::InternalError, "partial sort keys are incomplete");
    for (std::size_t index = 0; index < items.size(); ++index) {
      int order = CompareValues(left.keys[index], right.keys[index]);
      if (items[index].direction == ir::LogicalOrderDirection::kDescending) {
        order = -order;
      }
      if (order != 0) {
        return order < 0;
      }
    }
    return left.sequence < right.sequence;
  }

  [[nodiscard]] bool LoadRun() {
    entries_.clear();
    next_ = 0;
    if (pending_.has_value()) {
      entries_.push_back(std::move(*pending_));
      pending_.reset();
    } else {
      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        return false;
      }
      Entry first = ReadEntry(input);
      Retain(&first);
      entries_.push_back(std::move(first));
    }

    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      Entry entry = ReadEntry(input);
      Retain(&entry);
      if (!SamePrefix(entries_.front(), entry)) {
        pending_.emplace(std::move(entry));
        break;
      }
      entries_.push_back(std::move(entry));
    }
    std::stable_sort(entries_.begin(), entries_.end(),
                     [this](const Entry &left, const Entry &right) {
                       return ComesBefore(left, right);
                     });
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<Entry> entries_;
  std::optional<Entry> pending_;
  std::uint64_t sequence_ = 0;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool closed_ = false;
};

class PartialTopNOperator final : public PullOperator {
 public:
  PartialTopNOperator(const PhysicalPlanNode &node, RuntimeState &state,
                      std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~PartialTopNOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }
    if (remaining_ == 0 || (next_ >= entries_.size() && !LoadRun())) {
      Close();
      return false;
    }

    Entry &entry = entries_[next_++];
    *row = std::move(entry.row);
    entry.keys = {};
    state_->memory_tracker.Release(entry.reserved_bytes);
    reserved_bytes_ -= entry.reserved_bytes;
    entry.reserved_bytes = 0;
    --remaining_;
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (source_ != nullptr) {
      source_->Close();
    }
    entries_.clear();
    pending_.reset();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  struct Entry {
    SlottedRow row;
    std::vector<Value> keys;
    std::uint64_t sequence = 0;
    std::size_t reserved_bytes = 0;
  };

  [[nodiscard]] Entry ReadEntry(const SlottedRow &input) {
    std::vector<Value> keys;
    keys.reserve(node_->top_n->Items().size());
    for (const auto &item : node_->top_n->Items()) {
      CHECK(item.expression != nullptr, common::InvalidArgumentError,
            "sort expression is null");
      keys.push_back(Evaluate(*item.expression, input,
                              item.precomputed_expressions, *state_));
    }
    return {.row = input.CopyTo(node_->output_slots, *state_->graph_reader),
            .keys = std::move(keys),
            .sequence = sequence_++};
  }

  [[nodiscard]] std::size_t EstimatedEntryHeapUsage(const Entry &entry) const {
    std::size_t bytes =
        entry.row.EstimatedHeapUsage() + entry.keys.capacity() * sizeof(Value);
    for (const Value &key : entry.keys) {
      bytes += EstimatedStoredValueHeapUsage(key);
    }
    return bytes;
  }

  void ReserveEntry(Entry *entry) {
    CHECK(entry != nullptr, common::InternalError,
          "partial Top-N entry is null");
    entry->reserved_bytes = EstimatedEntryHeapUsage(*entry);
    state_->memory_tracker.Reserve(entry->reserved_bytes);
    reserved_bytes_ += entry->reserved_bytes;
  }

  void ReplaceEntry(Entry entry, Entry *replaced) {
    CHECK(replaced != nullptr, common::InternalError,
          "partial Top-N replacement entry is null");
    const std::size_t old_bytes = replaced->reserved_bytes;
    if (entry.reserved_bytes == 0) {
      entry.reserved_bytes = EstimatedEntryHeapUsage(entry);
    }
    if (entry.reserved_bytes > old_bytes) {
      state_->memory_tracker.Reserve(entry.reserved_bytes - old_bytes);
      reserved_bytes_ += entry.reserved_bytes - old_bytes;
    }
    *replaced = std::move(entry);
    if (old_bytes > replaced->reserved_bytes) {
      state_->memory_tracker.Release(old_bytes - replaced->reserved_bytes);
      reserved_bytes_ -= old_bytes - replaced->reserved_bytes;
    }
  }

  [[nodiscard]] bool SamePrefix(const Entry &left, const Entry &right) const {
    CHECK(node_->partial_top_n_prefix > 0 &&
              node_->partial_top_n_prefix <= left.keys.size() &&
              node_->partial_top_n_prefix <= right.keys.size(),
          common::InternalError, "partial Top-N prefix is invalid");
    for (std::size_t index = 0; index < node_->partial_top_n_prefix; ++index) {
      if (CompareValues(left.keys[index], right.keys[index]) != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool ComesBefore(const Entry &left, const Entry &right) const {
    const auto &items = node_->top_n->Items();
    CHECK(left.keys.size() == items.size() && right.keys.size() == items.size(),
          common::InternalError, "partial Top-N sort keys are incomplete");
    for (std::size_t index = 0; index < items.size(); ++index) {
      int order = CompareValues(left.keys[index], right.keys[index]);
      if (items[index].direction == ir::LogicalOrderDirection::kDescending) {
        order = -order;
      }
      if (order != 0) {
        return order < 0;
      }
    }
    return left.sequence < right.sequence;
  }

  void Retain(Entry entry) {
    const auto comparator = [this](const Entry &left, const Entry &right) {
      return ComesBefore(left, right);
    };
    if (entries_.size() < run_limit_) {
      ReserveEntry(&entry);
      entries_.push_back(std::move(entry));
      return;
    }
    if (!heapified_) {
      std::make_heap(entries_.begin(), entries_.end(), comparator);
      heapified_ = true;
    }
    if (!ComesBefore(entry, entries_.front())) {
      return;
    }
    std::pop_heap(entries_.begin(), entries_.end(), comparator);
    ReplaceEntry(std::move(entry), &entries_.back());
    std::push_heap(entries_.begin(), entries_.end(), comparator);
  }

  [[nodiscard]] bool LoadRun() {
    entries_.clear();
    next_ = 0;
    heapified_ = false;
    run_limit_ = static_cast<std::size_t>(std::min<std::uint64_t>(
        remaining_, std::numeric_limits<std::size_t>::max()));
    CHECK(run_limit_ > 0, common::InternalError,
          "partial Top-N run limit is zero");

    if (pending_.has_value()) {
      entries_.push_back(std::move(*pending_));
      pending_.reset();
    } else {
      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        return false;
      }
      Entry first = ReadEntry(input);
      ReserveEntry(&first);
      entries_.push_back(std::move(first));
    }

    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      Entry entry = ReadEntry(input);
      if (!SamePrefix(entries_.front(), entry)) {
        ReserveEntry(&entry);
        pending_.emplace(std::move(entry));
        break;
      }
      Retain(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry &left, const Entry &right) {
                return ComesBefore(left, right);
              });
    return true;
  }

  void Initialize() {
    initialized_ = true;
    CHECK(node_->kind == PhysicalOperatorKind::kPartialTopN &&
              node_->top_n != nullptr && node_->children.size() == 1 &&
              node_->partial_top_n_prefix > 0,
          common::InternalError, "partial Top-N physical node is incomplete");
    CHECK(node_->top_n->Limit() != nullptr, common::InvalidArgumentError,
          "LIMIT expression is null");
    CHECK(
        node_->top_n->PrecomputedExpressions().empty() &&
            ast::CollectExpressionDependencies(*node_->top_n->Limit()).empty(),
        common::InternalError,
        "partial Top-N LIMIT must not depend on an input row");
    const Value count =
        EvaluateExpression(*node_->top_n->Limit(), state_->context);
    CHECK(count.IsInteger() && count.AsInteger() >= 0,
          common::InvalidArgumentError,
          "LIMIT requires a non-negative integer");
    remaining_ = static_cast<std::uint64_t>(count.AsInteger());
    if (remaining_ == 0) {
      source_->Close();
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<Entry> entries_;
  std::optional<Entry> pending_;
  std::uint64_t remaining_ = 0;
  std::uint64_t sequence_ = 0;
  std::size_t run_limit_ = 0;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool heapified_ = false;
  bool initialized_ = false;
  bool closed_ = false;
};

class BlockingUnaryOperator final : public PullOperator {
 public:
  BlockingUnaryOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~BlockingUnaryOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (!initialized_) {
      Initialize();
    }
    if (next_ >= rows_.size()) {
      Close();
      return false;
    }
    *row = std::move(rows_[next_++]);
    return true;
  }

  void Close() noexcept override {
    if (source_ != nullptr) {
      source_->Close();
    }
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  void BufferRow(SlottedRow row) {
    const std::size_t bytes = row.EstimatedHeapUsage();
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    rows_.push_back(std::move(row));
  }

  void Initialize() {
    initialized_ = true;
    if (node_->logical->Type() == ir::LogicalPlanNodeType::kWriteBarrier) {
      SlottedRow input(node_->children[0]->output_slots);
      while (source_->Next(&input)) {
        BufferRow(input.CopyTo(node_->output_slots, *state_->graph_reader));
      }
      return;
    }

    if (node_->logical->Type() == ir::LogicalPlanNodeType::kDelete ||
        node_->logical->Type() == ir::LogicalPlanNodeType::kDetachDelete) {
      Storage &storage = RequireStorage(state_);
      const bool detach =
          node_->logical->Type() == ir::LogicalPlanNodeType::kDetachDelete;
      const auto &expressions =
          detach ? static_cast<const ir::DetachDeletePlan &>(*node_->logical)
                       .Expressions()
                 : static_cast<const ir::DeletePlan &>(*node_->logical)
                       .Expressions();
      std::set<std::int64_t> node_ids;
      std::set<std::int64_t> relationship_ids;
      SlottedRow input(node_->children[0]->output_slots);
      while (source_->Next(&input)) {
        state_->CheckCancelled();
        BufferRow(input.CopyTo(node_->output_slots, *state_->graph_reader));
        for (const ast::Expression *expression : expressions) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                "DELETE expression is null");
          const Value entity = Evaluate(*expression, input, {}, *state_);
          if (entity.IsNull()) {
            continue;
          }
          CHECK(entity.IsNode() || entity.IsRelationship(),
                common::InvalidArgumentError,
                "DELETE expression is not a graph entity");
          if (entity.IsNode()) {
            node_ids.insert(entity.AsNode().id);
          } else {
            relationship_ids.insert(entity.AsRelationship().id);
          }
        }
      }
      for (std::int64_t node_id : node_ids) {
        EntityIdCursor *relationships =
            state_->TrackCursor(storage.RelationshipIdsConnectedTo(node_id));
        while (relationships->Next()) {
          if (detach) {
            relationship_ids.insert(relationships->Id());
          } else {
            CHECK(relationship_ids.contains(relationships->Id()),
                  common::InvalidArgumentError,
                  "DELETE node still has relationships");
          }
        }
        state_->ReleaseCursor(relationships);
      }
      for (std::int64_t relationship_id : relationship_ids) {
        storage.DeleteRelationship(relationship_id);
      }
      for (std::int64_t node_id : node_ids) {
        storage.DeleteNode(node_id);
      }
      return;
    }

    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kDistinct: {
        const auto &plan =
            static_cast<const ir::DistinctPlan &>(*node_->logical);
        std::unordered_set<CompositeValueKey, ValueHash, ValueEqual> seen;
        SlottedRow input(node_->children[0]->output_slots);
        while (source_->Next(&input)) {
          std::vector<Value> values =
              EvaluateGroupingValues(plan.GroupingItems(), input, state_);
          auto [key, inserted] =
              seen.insert(CompositeValueKey{.values = values});
          if (!inserted) {
            continue;
          }
          const std::size_t key_bytes = EstimatedKeyHeapUsage(*key);
          state_->memory_tracker.Reserve(key_bytes);
          reserved_bytes_ += key_bytes;
          SlottedRow output(node_->output_slots);
          for (std::size_t index = 0; index < values.size(); ++index) {
            output.Set(plan.GroupingItems()[index].alias,
                       std::move(values[index]));
          }
          BufferRow(std::move(output));
        }
        break;
      }
      case ir::LogicalPlanNodeType::kAggregation: {
        const auto &plan =
            static_cast<const ir::AggregationPlan &>(*node_->logical);
        std::vector<AggregateAccumulator> accumulator_templates;
        accumulator_templates.reserve(plan.AggregationItems().size());
        for (const auto &item : plan.AggregationItems()) {
          accumulator_templates.push_back(CreateAggregateAccumulator(item));
        }
        struct Group {
          Group(SlottedRow projected,
                const std::vector<AggregateAccumulator> &templates)
              : output(std::move(projected)), accumulators(templates) {}
          SlottedRow output;
          std::vector<AggregateAccumulator> accumulators;
        };
        std::vector<std::unique_ptr<Group>> groups;
        std::unordered_map<CompositeValueKey, std::size_t, ValueHash,
                           ValueEqual>
            group_indexes;
        SlottedRow input(node_->children[0]->output_slots);
        while (source_->Next(&input)) {
          std::vector<Value> values =
              EvaluateGroupingValues(plan.GroupingItems(), input, state_);
          auto [group, inserted] = group_indexes.emplace(
              CompositeValueKey{.values = values}, groups.size());
          if (inserted) {
            SlottedRow projected(node_->output_slots);
            for (std::size_t index = 0; index < values.size(); ++index) {
              projected.Set(plan.GroupingItems()[index].alias,
                            std::move(values[index]));
            }
            groups.push_back(std::make_unique<Group>(std::move(projected),
                                                     accumulator_templates));
            const std::size_t key_bytes = EstimatedKeyHeapUsage(group->first);
            state_->memory_tracker.Reserve(key_bytes);
            reserved_bytes_ += key_bytes;
          }
          Group *state = groups[group->second].get();
          for (auto &accumulator : state->accumulators) {
            UpdateAggregateAccumulator(&accumulator, input, state_,
                                       &reserved_bytes_);
          }
        }
        if (plan.GroupingItems().empty() && groups.empty()) {
          group_indexes.emplace(CompositeValueKey{}, 0U);
          groups.push_back(std::make_unique<Group>(
              SlottedRow(node_->output_slots), accumulator_templates));
        }
        for (auto &group : groups) {
          for (auto &accumulator : group->accumulators) {
            group->output.Set(accumulator.item->alias,
                              FinalizeAggregateAccumulator(&accumulator, state_,
                                                           &reserved_bytes_));
          }
          BufferRow(std::move(group->output));
        }
        break;
      }
      case ir::LogicalPlanNodeType::kSort: {
        const auto &plan = static_cast<const ir::SortPlan &>(*node_->logical);
        SlottedRow input(node_->children[0]->output_slots);
        while (source_->Next(&input)) {
          BufferRow(input.CopyTo(node_->output_slots, *state_->graph_reader));
        }
        std::stable_sort(
            rows_.begin(), rows_.end(),
            [this, &plan](const SlottedRow &left, const SlottedRow &right) {
              for (const auto &item : plan.Items()) {
                CHECK(item.expression != nullptr, common::InvalidArgumentError,
                      "sort expression is null");
                int order = CompareValues(
                    Evaluate(*item.expression, left,
                             item.precomputed_expressions, *state_),
                    Evaluate(*item.expression, right,
                             item.precomputed_expressions, *state_));
                if (item.direction == ir::LogicalOrderDirection::kDescending) {
                  order = -order;
                }
                if (order != 0) {
                  return order < 0;
                }
              }
              return false;
            });
        break;
      }
      default:
        THROW(common::InternalError, "unsupported blocking unary operator: " +
                                         std::string(node_->logical->Name()));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class TopNOperator final : public PullOperator {
 public:
  TopNOperator(const PhysicalPlanNode &node, RuntimeState &state,
               std::unique_ptr<PullOperator> source)
      : node_(&node), state_(&state), source_(std::move(source)) {}

  ~TopNOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }
    if (next_ >= entries_.size()) {
      Close();
      return false;
    }
    *row = std::move(entries_[next_++].row);
    return true;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (source_ != nullptr) {
      source_->Close();
    }
    entries_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  struct Entry {
    SlottedRow row;
    std::vector<Value> keys;
    std::uint64_t sequence = 0;
    std::size_t reserved_bytes = 0;
  };

  [[nodiscard]] bool ComesBefore(const Entry &left, const Entry &right) const {
    const auto &items = node_->top_n->Items();
    CHECK(left.keys.size() == items.size() && right.keys.size() == items.size(),
          common::InternalError, "Top-N sort keys are incomplete");
    for (std::size_t index = 0; index < items.size(); ++index) {
      int order = CompareValues(left.keys[index], right.keys[index]);
      if (items[index].direction == ir::LogicalOrderDirection::kDescending) {
        order = -order;
      }
      if (order != 0) {
        return order < 0;
      }
    }
    return left.sequence < right.sequence;
  }

  [[nodiscard]] std::size_t EstimatedEntryHeapUsage(const Entry &entry) const {
    std::size_t bytes =
        entry.row.EstimatedHeapUsage() + entry.keys.capacity() * sizeof(Value);
    for (const Value &key : entry.keys) {
      bytes += EstimatedStoredValueHeapUsage(key);
    }
    return bytes;
  }

  [[nodiscard]] Entry CreateEntry(const SlottedRow &input) {
    std::vector<Value> keys;
    keys.reserve(node_->top_n->Items().size());
    for (const auto &item : node_->top_n->Items()) {
      CHECK(item.expression != nullptr, common::InvalidArgumentError,
            "sort expression is null");
      keys.push_back(Evaluate(*item.expression, input,
                              item.precomputed_expressions, *state_));
    }
    Entry entry{.row = input.CopyTo(node_->output_slots, *state_->graph_reader),
                .keys = std::move(keys),
                .sequence = sequence_++};
    entry.reserved_bytes = EstimatedEntryHeapUsage(entry);
    return entry;
  }

  void ReserveEntry(const Entry &entry) {
    state_->memory_tracker.Reserve(entry.reserved_bytes);
    reserved_bytes_ += entry.reserved_bytes;
  }

  void ReplaceEntry(Entry entry, Entry *replaced) {
    CHECK(replaced != nullptr, common::InternalError,
          "Top-N replacement entry is null");
    const std::size_t old_bytes = replaced->reserved_bytes;
    if (entry.reserved_bytes > old_bytes) {
      state_->memory_tracker.Reserve(entry.reserved_bytes - old_bytes);
      reserved_bytes_ += entry.reserved_bytes - old_bytes;
    }
    *replaced = std::move(entry);
    if (old_bytes > replaced->reserved_bytes) {
      state_->memory_tracker.Release(old_bytes - replaced->reserved_bytes);
      reserved_bytes_ -= old_bytes - replaced->reserved_bytes;
    }
  }

  void RetainTopOne(Entry entry) {
    if (entries_.empty()) {
      ReserveEntry(entry);
      entries_.push_back(std::move(entry));
      return;
    }
    if (ComesBefore(entry, entries_.front())) {
      ReplaceEntry(std::move(entry), &entries_.front());
    }
  }

  void Retain(Entry entry) {
    const auto comparator = [this](const Entry &left, const Entry &right) {
      return ComesBefore(left, right);
    };
    if (entries_.size() < limit_) {
      ReserveEntry(entry);
      entries_.push_back(std::move(entry));
      return;
    }
    if (!heapified_) {
      std::make_heap(entries_.begin(), entries_.end(), comparator);
      heapified_ = true;
    }
    if (!ComesBefore(entry, entries_.front())) {
      return;
    }

    std::pop_heap(entries_.begin(), entries_.end(), comparator);
    ReplaceEntry(std::move(entry), &entries_.back());
    std::push_heap(entries_.begin(), entries_.end(), comparator);
  }

  void Initialize() {
    initialized_ = true;
    CHECK(node_->kind == PhysicalOperatorKind::kTopN &&
              node_->top_n != nullptr && node_->children.size() == 1,
          common::InternalError, "Top-N physical node is incomplete");
    const ir::TopNPlan &top_n = *node_->top_n;
    CHECK(top_n.Limit() != nullptr, common::InvalidArgumentError,
          "LIMIT expression is null");
    CHECK(top_n.PrecomputedExpressions().empty() &&
              ast::CollectExpressionDependencies(*top_n.Limit()).empty(),
          common::InternalError, "Top-N LIMIT must not depend on an input row");
    const Value count = EvaluateExpression(*top_n.Limit(), state_->context);
    CHECK(count.IsInteger() && count.AsInteger() >= 0,
          common::InvalidArgumentError,
          "LIMIT requires a non-negative integer");
    limit_ = static_cast<std::uint64_t>(count.AsInteger());
    if (limit_ == 0) {
      source_->Close();
      return;
    }

    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      Entry entry = CreateEntry(input);
      if (limit_ == 1) {
        RetainTopOne(std::move(entry));
      } else {
        Retain(std::move(entry));
      }
    }
    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry &left, const Entry &right) {
                return ComesBefore(left, right);
              });
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<Entry> entries_;
  std::uint64_t limit_ = 0;
  std::uint64_t sequence_ = 0;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool heapified_ = false;
  bool initialized_ = false;
  bool closed_ = false;
};

class LeftOuterHashJoinOperator final : public PullOperator {
 public:
  LeftOuterHashJoinOperator(const PhysicalPlanNode &node, RuntimeState &state,
                            std::unique_ptr<PullOperator> lhs,
                            std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}
  ~LeftOuterHashJoinOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      initialized_ = true;
      SlottedRow right(node_->children[1]->output_slots);
      while (rhs_->Next(&right)) {
        state_->CheckCancelled();
        auto key = Key(right);
        if (key.has_value()) {
          const auto bytes = right.EstimatedHeapUsage() + sizeof(SlottedRow) +
                             key->values.size() * sizeof(Value) +
                             sizeof(CompositeValueKey);
          state_->memory_tracker.Reserve(bytes);
          reserved_bytes_ += bytes;
          buckets_[std::move(*key)].push_back(right);
        }
      }
      rhs_->Close();
    }
    while (true) {
      state_->CheckCancelled();
      if (!left_.has_value()) {
        SlottedRow left(node_->children[0]->output_slots);
        if (!lhs_->Next(&left)) {
          Close();
          return false;
        }
        left_.emplace(std::move(left));
        matches_ = nullptr;
        index_ = 0;
        matched_ = false;
        if (auto key = Key(*left_); key.has_value()) {
          const auto found = buckets_.find(*key);
          if (found != buckets_.end()) {
            matches_ = &found->second;
          }
        }
      }
      while (matches_ != nullptr && index_ < matches_->size()) {
        SlottedRow output(node_->output_slots);
        CopyMappings(*left_, &output, node_->child_mappings[0], *state_);
        if (MergeMappings((*matches_)[index_++], &output,
                          node_->child_mappings[1], *state_)) {
          matched_ = true;
          *row = std::move(output);
          return true;
        }
      }
      if (!matched_) {
        SlottedRow output(node_->output_slots);
        CopyMappings(*left_, &output, node_->child_mappings[0], *state_);
        for (const auto &column : node_->output_slots->Columns()) {
          if (!output.IsInitialized(column)) {
            output.SetNull(column);
          }
        }
        left_.reset();
        *row = std::move(output);
        return true;
      }
      left_.reset();
    }
  }

  void Close() noexcept override {
    lhs_->Close();
    rhs_->Close();
    buckets_.clear();
    left_.reset();
    matches_ = nullptr;
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  std::optional<CompositeValueKey> Key(const SlottedRow &row) const {
    CompositeValueKey key;
    for (const auto &column :
         static_cast<const ir::LeftOuterHashJoinPlan &>(*node_->logical)
             .JoinKeys()) {
      auto value = row.Get(column, *state_->graph_reader);
      if (value.IsNull()) {
        return std::nullopt;
      }
      key.values.push_back(std::move(value));
    }
    return key;
  }
  const PhysicalPlanNode *node_;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::unordered_map<CompositeValueKey, std::vector<SlottedRow>, ValueHash,
                     ValueEqual>
      buckets_;
  std::optional<SlottedRow> left_;
  const std::vector<SlottedRow> *matches_ = nullptr;
  std::size_t index_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool matched_ = false;
  bool initialized_ = false;
  bool closed_ = false;
};

class BlockingBinaryOperator final : public PullOperator {
 public:
  BlockingBinaryOperator(const PhysicalPlanNode &node, RuntimeState &state,
                         std::unique_ptr<PullOperator> lhs,
                         std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}

  ~BlockingBinaryOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    state_->CheckCancelled();
    if (!initialized_) {
      Initialize();
    }
    if (next_ >= rows_.size()) {
      Close();
      return false;
    }
    *row = std::move(rows_[next_++]);
    return true;
  }

  void Close() noexcept override {
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
  }

 private:
  std::vector<SlottedRow> Collect(PullOperator *source,
                                  SlotConfigurationPtr source_slots) {
    std::vector<SlottedRow> rows;
    SlottedRow row(std::move(source_slots));
    while (source->Next(&row)) {
      const std::size_t bytes = row.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(bytes);
      reserved_bytes_ += bytes;
      rows.push_back(row);
    }
    return rows;
  }

  void BufferRow(SlottedRow row) {
    const std::size_t bytes = row.EstimatedHeapUsage();
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    rows_.push_back(std::move(row));
  }

  std::optional<CompositeValueKey> NodeJoinKey(
      const SlottedRow &row, const std::vector<std::string> &keys) const {
    CompositeValueKey result;
    result.values.reserve(keys.size());
    for (const auto &key : keys) {
      Value value = row.Get(key, *state_->graph_reader);
      if (value.IsNull()) {
        return std::nullopt;
      }
      result.values.push_back(std::move(value));
    }
    return result;
  }

  bool EmitJoined(const SlottedRow &lhs, const SlottedRow &rhs,
                  const std::vector<const ast::Expression *> *predicates) {
    SlottedRow output(node_->output_slots);
    if (!MergeMappings(lhs, &output, node_->child_mappings[0], *state_) ||
        !MergeMappings(rhs, &output, node_->child_mappings[1], *state_)) {
      return false;
    }
    if (predicates != nullptr) {
      for (const ast::Expression *predicate : *predicates) {
        CHECK(predicate != nullptr, common::InvalidArgumentError,
              "join predicate is null");
        if (!PredicateIsTrue(Evaluate(*predicate, output, {}, *state_))) {
          return false;
        }
      }
    }
    BufferRow(std::move(output));
    return true;
  }

  void Initialize() {
    initialized_ = true;
    std::vector<SlottedRow> lhs_rows =
        Collect(lhs_.get(), node_->children[0]->output_slots);
    std::vector<SlottedRow> rhs_rows =
        Collect(rhs_.get(), node_->children[1]->output_slots);
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kCartesianProduct:
        for (const auto &lhs : lhs_rows) {
          state_->CheckCancelled();
          for (const auto &rhs : rhs_rows) {
            (void)EmitJoined(lhs, rhs, nullptr);
          }
        }
        break;
      case ir::LogicalPlanNodeType::kNodeHashJoin: {
        const auto &plan =
            static_cast<const ir::NodeHashJoinPlan &>(*node_->logical);
        std::unordered_map<CompositeValueKey, std::vector<const SlottedRow *>,
                           ValueHash, ValueEqual>
            buckets;
        for (const auto &rhs : rhs_rows) {
          if (std::optional<CompositeValueKey> key =
                  NodeJoinKey(rhs, plan.JoinKeys());
              key.has_value()) {
            buckets[*key].push_back(&rhs);
          }
        }
        for (const auto &lhs : lhs_rows) {
          state_->CheckCancelled();
          const std::optional<CompositeValueKey> key =
              NodeJoinKey(lhs, plan.JoinKeys());
          if (!key.has_value()) {
            continue;
          }
          const auto found = buckets.find(*key);
          if (found == buckets.end()) {
            continue;
          }
          for (const SlottedRow *rhs : found->second) {
            (void)EmitJoined(lhs, *rhs, nullptr);
          }
        }
        break;
      }
      case ir::LogicalPlanNodeType::kPredicateJoin: {
        const auto &predicates =
            static_cast<const ir::PredicateJoinPlan &>(*node_->logical)
                .Predicates();
        for (const auto &lhs : lhs_rows) {
          state_->CheckCancelled();
          for (const auto &rhs : rhs_rows) {
            (void)EmitJoined(lhs, rhs, &predicates);
          }
        }
        break;
      }
      default:
        THROW(common::InternalError, "unexpected blocking binary operator");
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::vector<SlottedRow> rows_;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool initialized_ = false;
};

class ValueHashJoinOperator final : public PullOperator {
 public:
  ValueHashJoinOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> lhs,
                        std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}

  ~ValueHashJoinOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (!initialized_) {
      Initialize();
    }

    while (true) {
      while (bucket_ != nullptr && bucket_offset_ < bucket_->size()) {
        state_->CheckCancelled();
        const SlottedRow &build_row = build_rows_[(*bucket_)[bucket_offset_++]];
        const SlottedRow &lhs = BuildChild() == 0 ? build_row : *probe_row_;
        const SlottedRow &rhs = BuildChild() == 0 ? *probe_row_ : build_row;
        SlottedRow output(node_->output_slots);
        if (!MergeMappings(lhs, &output, node_->child_mappings[0], *state_) ||
            !MergeMappings(rhs, &output, node_->child_mappings[1], *state_)) {
          continue;
        }
        if (!PredicatesMatch(output)) {
          continue;
        }
        *row = std::move(output);
        return true;
      }

      bucket_ = nullptr;
      bucket_offset_ = 0;
      probe_row_.reset();
      SlottedRow probe(node_->children[ProbeChild()]->output_slots);
      if (!Source(ProbeChild())->Next(&probe)) {
        Close();
        return false;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key = JoinKey(probe, ProbeChild());
      if (!key.has_value()) {
        continue;
      }
      const auto found = buckets_.find(*key);
      if (found == buckets_.end()) {
        continue;
      }
      probe_row_.emplace(std::move(probe));
      bucket_ = &found->second;
    }
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    probe_row_.reset();
    bucket_ = nullptr;
    buckets_.clear();
    build_rows_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  using Bucket = std::vector<std::size_t>;

  [[nodiscard]] std::size_t BuildChild() const {
    return node_->value_hash_join_build_child;
  }

  [[nodiscard]] std::size_t ProbeChild() const { return 1U - BuildChild(); }

  PullOperator *Source(std::size_t child) const {
    return child == 0 ? lhs_.get() : rhs_.get();
  }

  std::optional<CompositeValueKey> JoinKey(const SlottedRow &row,
                                           std::size_t child) const {
    const auto &plan =
        static_cast<const ir::ValueHashJoinPlan &>(*node_->logical);
    CompositeValueKey result;
    result.values.reserve(plan.JoinKeys().size());
    for (const ir::ValueHashJoinKey &key : plan.JoinKeys()) {
      const ast::Expression *expression = child == 0 ? key.left : key.right;
      CHECK(expression != nullptr, common::InvalidArgumentError,
            "value hash join key expression is null");
      Value value = Evaluate(*expression, row, {}, *state_);
      if (value.IsNull()) {
        return std::nullopt;
      }
      result.values.push_back(std::move(value));
    }
    return result;
  }

  bool PredicatesMatch(const SlottedRow &row) const {
    const auto &predicates =
        static_cast<const ir::ValueHashJoinPlan &>(*node_->logical)
            .Predicates();
    for (const ast::Expression *predicate : predicates) {
      CHECK(predicate != nullptr, common::InvalidArgumentError,
            "join predicate is null");
      if (!PredicateIsTrue(Evaluate(*predicate, row, {}, *state_))) {
        return false;
      }
    }
    return true;
  }

  void Initialize() {
    initialized_ = true;
    CHECK(BuildChild() < 2, common::InternalError,
          "invalid value hash join build child");
    PullOperator *source = Source(BuildChild());
    while (true) {
      SlottedRow input(node_->children[BuildChild()]->output_slots);
      if (!source->Next(&input)) {
        source->Close();
        return;
      }
      state_->CheckCancelled();
      std::optional<CompositeValueKey> key = JoinKey(input, BuildChild());
      if (!key.has_value()) {
        continue;
      }

      const std::size_t row_bytes = input.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(row_bytes);
      reserved_bytes_ += row_bytes;
      const std::size_t row_index = build_rows_.size();
      build_rows_.push_back(std::move(input));

      auto [found, inserted] = buckets_.try_emplace(std::move(*key));
      if (inserted) {
        const std::size_t key_bytes = EstimatedKeyHeapUsage(found->first);
        state_->memory_tracker.Reserve(key_bytes);
        reserved_bytes_ += key_bytes;
      }
      const std::size_t old_capacity = found->second.capacity();
      found->second.push_back(row_index);
      const std::size_t new_capacity = found->second.capacity();
      if (new_capacity > old_capacity) {
        const std::size_t bucket_bytes =
            (new_capacity - old_capacity) * sizeof(std::size_t);
        state_->memory_tracker.Reserve(bucket_bytes);
        reserved_bytes_ += bucket_bytes;
      }
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::unordered_map<CompositeValueKey, Bucket, ValueHash, ValueEqual> buckets_;
  std::vector<SlottedRow> build_rows_;
  std::optional<SlottedRow> probe_row_;
  const Bucket *bucket_ = nullptr;
  std::size_t bucket_offset_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class UnionOperator final : public PullOperator {
 public:
  UnionOperator(const PhysicalPlanNode &node, RuntimeState &state,
                std::unique_ptr<PullOperator> lhs,
                std::unique_ptr<PullOperator> rhs)
      : node_(&node),
        state_(&state),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)) {}

  ~UnionOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    state_->CheckCancelled();
    const auto &plan = static_cast<const ir::UnionPlan &>(*node_->logical);
    while (side_ < 2) {
      PullOperator *source = side_ == 0 ? lhs_.get() : rhs_.get();
      const PhysicalPlanNode &child = *node_->children[side_];
      SlottedRow input(child.output_slots);
      if (!source->Next(&input)) {
        source->Close();
        ++side_;
        continue;
      }
      SlottedRow output(node_->output_slots);
      CopyMappings(input, &output, node_->child_mappings[side_], *state_);
      if (!plan.All()) {
        CompositeValueKey key;
        key.values.reserve(node_->output_slots->Columns().size());
        for (const auto &column : node_->output_slots->Columns()) {
          key.values.push_back(output.Get(column, *state_->graph_reader));
        }
        auto [seen, inserted] = seen_.insert(std::move(key));
        if (!inserted) {
          continue;
        }
        const std::size_t key_bytes = EstimatedKeyHeapUsage(*seen);
        state_->memory_tracker.Reserve(key_bytes);
        reserved_bytes_ += key_bytes;
      }
      *row = std::move(output);
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (lhs_ != nullptr) {
      lhs_->Close();
    }
    if (rhs_ != nullptr) {
      rhs_->Close();
    }
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::unordered_set<CompositeValueKey, ValueHash, ValueEqual> seen_;
  std::size_t side_ = 0;
  std::size_t reserved_bytes_ = 0;
};

class ApplyOperator final : public PullOperator {
 public:
  ApplyOperator(const PhysicalPlanNode &node, RuntimeState &state,
                OperatorFactory &factory, std::unique_ptr<PullOperator> lhs)
      : node_(&node),
        state_(&state),
        factory_(&factory),
        lhs_(std::move(lhs)) {}

  ~ApplyOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override;

 private:
  bool NextLeft();
  bool Combine(const SlottedRow &rhs, SlottedRow *row);

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  OperatorFactory *factory_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::optional<SlottedRow> current_left_;
  bool rhs_produced_ = false;
};

class MergeOperator final : public PullOperator {
 public:
  MergeOperator(const PhysicalPlanNode &node, RuntimeState &state,
                OperatorFactory &factory, std::unique_ptr<PullOperator> source)
      : node_(&node),
        state_(&state),
        factory_(&factory),
        source_(std::move(source)) {}

  ~MergeOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override;
  void Close() noexcept override;

 private:
  void Initialize();
  void BufferRow(SlottedRow row) {
    const std::size_t bytes = row.EstimatedHeapUsage();
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    rows_.push_back(std::move(row));
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  OperatorFactory *factory_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t next_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool initialized_ = false;
};

class OperatorFactory final {
 public:
  explicit OperatorFactory(RuntimeState &state) : state_(&state) {}

  std::unique_ptr<PullOperator> Build(
      const PhysicalPlanNode &node,
      std::optional<SlottedRow> argument = std::nullopt) {
    switch (node.kind) {
      case PhysicalOperatorKind::kArgument:
        CHECK(node.children.empty(), common::InternalError,
              "argument physical node must not have children");
        return std::make_unique<ArgumentOperator>(node, *state_,
                                                  std::move(argument));
      case PhysicalOperatorKind::kAllNodeScan:
        CHECK(node.children.empty(), common::InternalError,
              "all-node scan physical node must not have children");
        return std::make_unique<AllNodeScanOperator>(node, *state_,
                                                     std::move(argument));
      case PhysicalOperatorKind::kNodeByLabelScan:
        CHECK(node.children.empty(), common::InternalError,
              "node-by-label scan physical node must not have children");
        return std::make_unique<NodeByLabelScanOperator>(node, *state_,
                                                         std::move(argument));
      case PhysicalOperatorKind::kNodeIndexSeek:
        CHECK(node.children.empty(), common::InternalError,
              "node index seek physical node must not have children");
        return std::make_unique<NodeIndexSeekOperator>(node, *state_,
                                                       std::move(argument));
      case PhysicalOperatorKind::kNodeIndexRangeSeek:
        CHECK(node.children.empty(), common::InternalError,
              "node index range seek physical node must not have children");
        return std::make_unique<NodeIndexRangeSeekOperator>(
            node, *state_, std::move(argument));
      case PhysicalOperatorKind::kRelationshipTypeScan:
        CHECK(node.children.empty(), common::InternalError,
              "relationship type scan physical node must not have children");
        return std::make_unique<RelationshipTypeScanOperator>(
            node, *state_, std::move(argument));
      case PhysicalOperatorKind::kRelationshipIndexSeek:
        CHECK(node.children.empty(), common::InternalError,
              "relationship index seek physical node must not have children");
        return std::make_unique<RelationshipIndexSeekOperator>(
            node, *state_, std::move(argument));
      case PhysicalOperatorKind::kRelationshipIndexRangeSeek:
        CHECK(node.children.empty(), common::InternalError,
              "relationship index range seek physical node must not have "
              "children");
        return std::make_unique<RelationshipIndexRangeSeekOperator>(
            node, *state_, std::move(argument));
      case PhysicalOperatorKind::kNodeByIdSeek:
        CHECK(node.children.empty(), common::InternalError,
              "node-by-id seek physical node must not have children");
        return std::make_unique<NodeByIdSeekOperator>(node, *state_,
                                                      std::move(argument));
      case PhysicalOperatorKind::kRelationshipByIdSeek:
        CHECK(node.children.empty(), common::InternalError,
              "relationship-by-id seek physical node must not have children");
        return std::make_unique<RelationshipByIdSeekOperator>(
            node, *state_, std::move(argument));
      case PhysicalOperatorKind::kFilter:
        CHECK(node.children.size() == 1, common::InternalError,
              "filter physical node must have one child");
        return std::make_unique<FilterOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kProjection:
        CHECK(node.children.size() == 1, common::InternalError,
              "projection physical node must have one child");
        return std::make_unique<ProjectionOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kSkip:
        CHECK(node.children.size() == 1, common::InternalError,
              "skip physical node must have one child");
        return std::make_unique<SkipOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kLimit:
        CHECK(node.children.size() == 1, common::InternalError,
              "limit physical node must have one child");
        return std::make_unique<LimitOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kProduceResults:
        CHECK(node.children.size() == 1, common::InternalError,
              "produce-results physical node must have one child");
        return std::make_unique<ProduceResultsOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kTopN:
        CHECK(node.children.size() == 1, common::InternalError,
              "Top-N physical node must have one child");
        return std::make_unique<TopNOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kPartialTopN:
        CHECK(node.children.size() == 1, common::InternalError,
              "partial Top-N physical node must have one child");
        return std::make_unique<PartialTopNOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kApply:
      case PhysicalOperatorKind::kSemiApply:
      case PhysicalOperatorKind::kAntiSemiApply:
      case PhysicalOperatorKind::kLetSemiApply:
      case PhysicalOperatorKind::kSelectOrSemiApply:
      case PhysicalOperatorKind::kRollUpApply:
      case PhysicalOperatorKind::kOptionalApply:
        CHECK(node.children.size() == 2, common::InternalError,
              "apply physical node must have two children");
        return std::make_unique<ApplyOperator>(
            node, *state_, *this,
            Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kMerge:
        CHECK(node.children.size() == 2, common::InternalError,
              "merge physical node must have two children");
        return std::make_unique<MergeOperator>(
            node, *state_, *this,
            Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kUnion: {
        CHECK(node.children.size() == 2, common::InternalError,
              "union physical node must have two children");
        auto lhs = Build(*node.children[0], argument);
        auto rhs = Build(*node.children[1], std::move(argument));
        return std::make_unique<UnionOperator>(node, *state_, std::move(lhs),
                                               std::move(rhs));
      }
      case PhysicalOperatorKind::kValueHashJoin: {
        CHECK(node.children.size() == 2, common::InternalError,
              "value hash join physical node must have two children");
        auto lhs = Build(*node.children[0], argument);
        auto rhs = Build(*node.children[1], std::move(argument));
        return std::make_unique<ValueHashJoinOperator>(
            node, *state_, std::move(lhs), std::move(rhs));
      }
      case PhysicalOperatorKind::kLeftOuterHashJoin: {
        CHECK(node.children.size() == 2, common::InternalError,
              "left outer hash join physical node must have two children");
        auto lhs = Build(*node.children[0], argument);
        auto rhs = Build(*node.children[1], std::move(argument));
        return std::make_unique<LeftOuterHashJoinOperator>(
            node, *state_, std::move(lhs), std::move(rhs));
      }
      case PhysicalOperatorKind::kCartesianProduct:
      case PhysicalOperatorKind::kNodeHashJoin:
      case PhysicalOperatorKind::kPredicateJoin: {
        CHECK(node.children.size() == 2, common::InternalError,
              "blocking binary physical node must have two children");
        auto lhs = Build(*node.children[0], argument);
        auto rhs = Build(*node.children[1], std::move(argument));
        return std::make_unique<BlockingBinaryOperator>(
            node, *state_, std::move(lhs), std::move(rhs));
      }
      case PhysicalOperatorKind::kVarExpand:
        CHECK(node.children.size() == 1, common::InternalError,
              "variable expand physical node must have one child");
        return std::make_unique<VarExpandOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kPruningVarExpand:
        CHECK(node.children.size() == 1, common::InternalError,
              "pruning expand physical node must have one child");
        return std::make_unique<PruningExpandOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kProjectEndpoints:
      case PhysicalOperatorKind::kOptionalExpand:
        CHECK(node.children.size() == 1, common::InternalError,
              "pattern physical node must have one child");
        return std::make_unique<PatternOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kOrderedDistinct:
        CHECK(node.children.size() == 1, common::InternalError,
              "ordered distinct physical node must have one child");
        return std::make_unique<OrderedDistinctOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kOrderedAggregation:
        CHECK(node.children.size() == 1, common::InternalError,
              "ordered aggregation physical node must have one child");
        return std::make_unique<OrderedAggregationOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kPartialSort:
        CHECK(node.children.size() == 1, common::InternalError,
              "partial sort physical node must have one child");
        return std::make_unique<PartialSortOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kFullSort:
      case PhysicalOperatorKind::kHashDistinct:
      case PhysicalOperatorKind::kHashAggregation:
      case PhysicalOperatorKind::kWriteBarrier:
      case PhysicalOperatorKind::kDelete:
      case PhysicalOperatorKind::kDetachDelete:
        CHECK(node.children.size() == 1, common::InternalError,
              "blocking physical node must have one child");
        return std::make_unique<BlockingUnaryOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
      case PhysicalOperatorKind::kExpand:
      case PhysicalOperatorKind::kExpandInto:
      case PhysicalOperatorKind::kPathBuild:
      case PhysicalOperatorKind::kProcedureCall:
      case PhysicalOperatorKind::kUnwind:
      case PhysicalOperatorKind::kAssertIsNode:
      case PhysicalOperatorKind::kSetProperty:
      case PhysicalOperatorKind::kSetProperties:
      case PhysicalOperatorKind::kSetLabels:
      case PhysicalOperatorKind::kRemoveProperty:
      case PhysicalOperatorKind::kRemoveLabels:
      case PhysicalOperatorKind::kCreateNode:
      case PhysicalOperatorKind::kCreateRelationship:
        CHECK(node.children.size() == 1, common::InternalError,
              "streaming physical node must have one child");
        return std::make_unique<StreamingUnaryOperator>(
            node, *state_, Build(*node.children[0], std::move(argument)));
    }
    THROW(common::InternalError, "unsupported physical operator kind: " +
                                     std::string(ToString(node.kind)));
  }

 private:
  RuntimeState *state_ = nullptr;
};

bool ApplyOperator::NextLeft() {
  SlottedRow lhs_row(node_->children[0]->output_slots);
  if (!lhs_->Next(&lhs_row)) {
    Close();
    return false;
  }
  current_left_.emplace(std::move(lhs_row));
  const bool skip =
      node_->logical->Type() == ir::LogicalPlanNodeType::kSelectOrSemiApply &&
      PredicateIsTrue(Evaluate(
          *static_cast<const ir::SelectOrSemiApplyPlan &>(*node_->logical)
               .Predicate(),
          *current_left_, {}, *state_));
  if (!skip) {
    rhs_ = factory_->Build(*node_->children[1], *current_left_);
  }
  rhs_produced_ = false;
  return true;
}

bool ApplyOperator::Combine(const SlottedRow &rhs, SlottedRow *row) {
  SlottedRow output(node_->output_slots);
  if (!MergeMappings(*current_left_, &output, node_->child_mappings[0],
                     *state_) ||
      !MergeMappings(rhs, &output, node_->child_mappings[1], *state_)) {
    return false;
  }
  *row = std::move(output);
  return true;
}

bool ApplyOperator::Next(SlottedRow *row) {
  CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
  state_->CheckCancelled();
  const ir::LogicalPlanNodeType type = node_->logical->Type();
  while (true) {
    if (!current_left_.has_value() && !NextLeft()) {
      return false;
    }

    if (type == ir::LogicalPlanNodeType::kSelectOrSemiApply &&
        rhs_ == nullptr) {
      *row = current_left_->CopyTo(node_->output_slots, *state_->graph_reader);
      current_left_.reset();
      return true;
    }

    if (type == ir::LogicalPlanNodeType::kApply ||
        type == ir::LogicalPlanNodeType::kOptionalApply) {
      SlottedRow rhs_row(node_->children[1]->output_slots);
      while (rhs_->Next(&rhs_row)) {
        rhs_produced_ = true;
        if (Combine(rhs_row, row)) {
          return true;
        }
      }
      rhs_->Close();
      rhs_.reset();
      if (type == ir::LogicalPlanNodeType::kOptionalApply && !rhs_produced_) {
        SlottedRow output(node_->output_slots);
        CopyMappings(*current_left_, &output, node_->child_mappings[0],
                     *state_);
        for (const auto &column : node_->output_slots->Columns()) {
          if (!output.IsInitialized(column)) {
            output.SetNull(column);
          }
        }
        current_left_.reset();
        *row = std::move(output);
        return true;
      }
      current_left_.reset();
      continue;
    }

    if (type == ir::LogicalPlanNodeType::kRollUpApply) {
      const auto &rollup =
          static_cast<const ir::RollUpApplyPlan &>(*node_->logical);
      Value::List values;
      std::size_t reserved_bytes = 0;
      SlottedRow rhs_row(node_->children[1]->output_slots);
      while (rhs_->Next(&rhs_row)) {
        Value value =
            rhs_row.Get(rollup.ValueVariable(), *state_->graph_reader);
        const std::size_t bytes = EstimatedValueHeapUsage(value);
        state_->memory_tracker.Reserve(bytes);
        reserved_bytes += bytes;
        values.push_back(std::move(value));
      }
      SlottedRow output =
          current_left_->CopyTo(node_->output_slots, *state_->graph_reader);
      output.Set(rollup.CollectionVariable(), Value(std::move(values)));
      state_->memory_tracker.Release(reserved_bytes);
      rhs_->Close();
      rhs_.reset();
      current_left_.reset();
      *row = std::move(output);
      return true;
    }

    SlottedRow rhs_row(node_->children[1]->output_slots);
    const bool exists = rhs_->Next(&rhs_row);
    rhs_->Close();
    rhs_.reset();
    const bool emit =
        type == ir::LogicalPlanNodeType::kSelectOrSemiApply
            ? exists != static_cast<const ir::SelectOrSemiApplyPlan &>(
                            *node_->logical)
                            .Anti()
        : type == ir::LogicalPlanNodeType::kSemiApply
            ? exists
            : (type == ir::LogicalPlanNodeType::kAntiSemiApply ? !exists
                                                               : true);
    SlottedRow output =
        current_left_->CopyTo(node_->output_slots, *state_->graph_reader);
    if (type == ir::LogicalPlanNodeType::kLetSemiApply) {
      output.Set(static_cast<const ir::LetSemiApplyPlan &>(*node_->logical)
                     .ValueVariable(),
                 Value(exists));
    }
    current_left_.reset();
    if (emit) {
      *row = std::move(output);
      return true;
    }
  }
}

void ApplyOperator::Close() noexcept {
  if (rhs_ != nullptr) {
    rhs_->Close();
  }
  if (lhs_ != nullptr) {
    lhs_->Close();
  }
}

void MergeOperator::Initialize() {
  initialized_ = true;
  SlottedRow input(node_->children[0]->output_slots);
  while (source_->Next(&input)) {
    state_->CheckCancelled();
    std::unique_ptr<PullOperator> rhs =
        factory_->Build(*node_->children[1], input);
    bool matched = false;
    SlottedRow rhs_row(node_->children[1]->output_slots);
    while (rhs->Next(&rhs_row)) {
      SlottedRow output(node_->output_slots);
      if (!MergeMappings(input, &output, node_->child_mappings[0], *state_) ||
          !MergeMappings(rhs_row, &output, node_->child_mappings[1], *state_)) {
        continue;
      }
      matched = true;
      for (const auto &action :
           static_cast<const ir::MergePlan &>(*node_->logical)
               .Merge()
               .actions) {
        if (action.on_match) {
          ApplySetPatterns(action.set_patterns, &output, state_);
        }
      }
      BufferRow(std::move(output));
    }
    rhs->Close();
    if (matched) {
      continue;
    }
    const auto &merge =
        static_cast<const ir::MergePlan &>(*node_->logical).Merge();
    SlottedRow output =
        input.CopyTo(node_->output_slots, *state_->graph_reader);
    ExecuteCreatePattern(merge.create_pattern, &output, state_, true);
    for (const auto &action : merge.actions) {
      if (!action.on_match) {
        ApplySetPatterns(action.set_patterns, &output, state_);
      }
    }
    BufferRow(std::move(output));
  }
}

bool MergeOperator::Next(SlottedRow *row) {
  state_->CheckCancelled();
  if (!initialized_) {
    Initialize();
  }
  if (next_ >= rows_.size()) {
    Close();
    return false;
  }
  *row = std::move(rows_[next_++]);
  return true;
}

void MergeOperator::Close() noexcept {
  if (source_ != nullptr) {
    source_->Close();
  }
  state_->memory_tracker.Release(reserved_bytes_);
  reserved_bytes_ = 0;
}

}  // namespace

namespace {

class PhysicalResultCursorImpl final : public PhysicalResultCursor {
 public:
  PhysicalResultCursorImpl(const PhysicalPlan &plan,
                           const GraphReader &graph_reader, Storage *storage,
                           const QueryParameters &parameters,
                           std::vector<std::string> result_columns,
                           QueryExecutionOptions options)
      : state_(graph_reader, storage, parameters, std::move(options)),
        factory_(state_),
        root_(factory_.Build(plan.Root())),
        output_slots_(plan.Root().output_slots),
        result_columns_(std::move(result_columns)) {}

  ~PhysicalResultCursorImpl() override { Close(); }

  [[nodiscard]] bool Next(std::vector<Value> *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "result row is null");
    if (closed_) {
      return false;
    }
    try {
      state_.CheckCancelled();
      SlottedRow slotted(output_slots_);
      if (!root_->Next(&slotted)) {
        Close();
        return false;
      }
      row->clear();
      row->reserve(result_columns_.size());
      for (const auto &column : result_columns_) {
        row->push_back(slotted.IsInitialized(column)
                           ? slotted.Get(column, *state_.graph_reader)
                           : Value::Null());
      }
      return true;
    } catch (...) {
      Close();
      throw;
    }
  }

  void Cancel() noexcept override { state_.cancellation->Cancel(); }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    if (root_ != nullptr) {
      root_->Close();
    }
    state_.resources.Close();
    peak_memory_bytes_ = state_.memory_tracker.PeakBytes();
    closed_ = true;
  }

  [[nodiscard]] std::size_t PeakMemoryBytes() const noexcept override {
    return closed_ ? peak_memory_bytes_ : state_.memory_tracker.PeakBytes();
  }

 private:
  RuntimeState state_;
  OperatorFactory factory_;
  std::unique_ptr<PullOperator> root_;
  SlotConfigurationPtr output_slots_;
  std::vector<std::string> result_columns_;
  std::size_t peak_memory_bytes_ = 0;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<PhysicalResultCursor> StartPhysicalPlan(
    const PhysicalPlan &plan, const GraphReader &graph_reader, Storage *storage,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns,
    QueryExecutionOptions options) {
  return std::make_unique<PhysicalResultCursorImpl>(plan, graph_reader, storage,
                                                    parameters, result_columns,
                                                    std::move(options));
}

}  // namespace rg
