#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "ast/builtin_function.h"
#include "common/exceptions.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graphdb_access.h"
#include "runtime/slotted_executor_internal.h"

namespace rg::slotted {

std::vector<Value> EvaluateGroupingValues(
    const std::vector<PhysicalGroupingItem> &items, const SlottedRow &row,
    RuntimeState *state) {
  CHECK(state != nullptr, common::InternalError, "runtime state is null");
  std::vector<Value> values;
  values.reserve(items.size());
  for (const auto &item : items) {
    values.push_back(Evaluate(item.expression, row, *state));
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
  const PhysicalAggregationItem *item = nullptr;
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
    const PhysicalAggregationItem &item) {
  const ast::Expression *expression = item.expression.Expression();
  CHECK(expression != nullptr, common::InvalidArgumentError,
        "aggregation expression is null");
  AggregateAccumulator accumulator{.item = &item};
  if (expression->Is(ast::ASTNodeType::kCountStarExpression)) {
    accumulator.kind = AggregateAccumulatorKind::kCountStar;
    return accumulator;
  }

  CHECK(expression->Is(ast::ASTNodeType::kFunctionInvocation),
        common::InvalidArgumentError, "unsupported aggregation expression");
  const auto &function = ast::CastAst<ast::FunctionInvocation>(*expression);
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

std::vector<AggregateAccumulator> CreateAggregateAccumulators(
    const std::vector<PhysicalAggregationItem> &items) {
  std::vector<AggregateAccumulator> accumulators;
  accumulators.reserve(items.size());
  for (const auto &item : items) {
    accumulators.push_back(CreateAggregateAccumulator(item));
  }
  return accumulators;
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
                       accumulator->item->expression.PrecomputedExpressions(),
                       *state);
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
  Value value =
      Evaluate(*accumulator->argument, row,
               accumulator->item->expression.PrecomputedExpressions(), *state);
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

std::vector<Value> EvaluateSortKeys(const std::vector<PhysicalSortItem> &items,
                                    const SlottedRow &row,
                                    RuntimeState *state) {
  CHECK(state != nullptr, common::InternalError, "runtime state is null");
  std::vector<Value> keys;
  keys.reserve(items.size());
  for (const auto &item : items) {
    keys.push_back(Evaluate(item.expression, row, *state));
  }
  return keys;
}

bool SortKeysHaveSamePrefix(const std::vector<Value> &left,
                            const std::vector<Value> &right,
                            std::size_t prefix) {
  CHECK(prefix > 0 && prefix <= left.size() && prefix <= right.size(),
        common::InternalError, "sort prefix is invalid");
  for (std::size_t index = 0; index < prefix; ++index) {
    if (CompareValues(left[index], right[index]) != 0) {
      return false;
    }
  }
  return true;
}

bool SortKeysComeBefore(const std::vector<PhysicalSortItem> &items,
                        const std::vector<Value> &left,
                        std::uint64_t left_sequence,
                        const std::vector<Value> &right,
                        std::uint64_t right_sequence) {
  CHECK(left.size() == items.size() && right.size() == items.size(),
        common::InternalError, "sort keys are incomplete");
  for (std::size_t index = 0; index < items.size(); ++index) {
    int order = CompareValues(left[index], right[index]);
    if (items[index].direction == PhysicalSortDirection::kDescending) {
      order = -order;
    }
    if (order != 0) {
      return order < 0;
    }
  }
  return left_sequence < right_sequence;
}

std::size_t EstimatedSortEntryHeapUsage(const SlottedRow &row,
                                        const std::vector<Value> &keys) {
  std::size_t bytes =
      row.EstimatedHeapUsage() + keys.capacity() * sizeof(Value);
  for (const Value &key : keys) {
    bytes += EstimatedStoredValueHeapUsage(key);
  }
  return bytes;
}

SlottedRow CopyChildOutput(const PhysicalPlanNode &node, std::size_t child,
                           const SlottedRow &input) {
  CHECK(child < node.child_mappings.size(), common::InternalError,
        "physical operator child mapping is missing");
  return CopyMappedRow(input, node.output_slots, node.child_mappings[child]);
}

SlottedRow CopyUnaryOutput(const PhysicalPlanNode &node,
                           const SlottedRow &input) {
  CHECK(node.child_mappings.size() == 1, common::InternalError,
        "unary physical operator mapping is missing");
  return CopyChildOutput(node, 0, input);
}

SlottedRow ForwardUnaryOutput(const PhysicalPlanNode &node, SlottedRow input) {
  if (input.Slots() == node.output_slots) {
    return input;
  }
  return CopyUnaryOutput(node, input);
}

bool HasSharedUnaryLayout(const PhysicalPlanNode &node) {
  return node.children.size() == 1 &&
         node.children.front()->output_slots == node.output_slots;
}

void CopySlotDirect(const SlottedRow &source, SlottedRow *target,
                    std::size_t source_offset, std::size_t target_offset) {
  CHECK(target != nullptr, common::InternalError, "target row is null");
  if (!source.IsInitialized(source_offset)) {
    return;
  }
  target->CopySlotFrom(source, source_offset, target_offset);
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
    if (HasSharedUnaryLayout(*node_)) {
      while (source_->Next(row)) {
        if (PredicateIsTrue(Evaluate(data_->predicate, *row, *state_))) {
          return true;
        }
        state_->CheckCancelled();
      }
    } else {
      SlottedRow input(node_->children[0]->output_slots);
      while (source_->Next(&input)) {
        if (PredicateIsTrue(Evaluate(data_->predicate, input, *state_))) {
          *row = ForwardUnaryOutput(*node_, std::move(input));
          return true;
        }
        state_->CheckCancelled();
      }
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
    if (passthrough_only_ && HasSharedUnaryLayout(*node_)) {
      if (!source_->Next(row)) {
        Close();
        return false;
      }
      return true;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (!source_->Next(&input)) {
      Close();
      return false;
    }
    SlottedRow output(node_->output_slots);
    for (const auto &item : data_->items) {
      if (item.passthrough) {
        CHECK(item.source_slot.has_value(), common::InternalError,
              "passthrough projection source slot is missing");
        CopySlotDirect(input, &output, *item.source_slot, item.output_slot);
      } else {
        Value value = Evaluate(item.expression, input, *state_);
        StoreEvaluatedValue(&output, item.output_slot, std::move(value),
                            *state_);
      }
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
  bool passthrough_only_ = std::all_of(
      data_->items.begin(), data_->items.end(),
      [](const PhysicalProjectionItem &item) { return item.passthrough; });
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
    if (pending_.has_value()) {
      *row = ForwardUnaryOutput(*node_, std::move(*pending_));
      pending_.reset();
    } else if (HasSharedUnaryLayout(*node_)) {
      if (!source_->Next(row)) {
        Close();
        return false;
      }
    } else {
      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        Close();
        return false;
      }
      *row = ForwardUnaryOutput(*node_, std::move(input));
    }
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

    if (pending_.has_value()) {
      *row = ForwardUnaryOutput(*node_, std::move(*pending_));
      pending_.reset();
    } else if (HasSharedUnaryLayout(*node_)) {
      if (!source_->Next(row)) {
        Close();
        return false;
      }
    } else {
      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        Close();
        return false;
      }
      *row = ForwardUnaryOutput(*node_, std::move(input));
    }
    ++emitted_;
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
        SlottedRow output = CopyUnaryOutput(*node_, input);
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
    (void)data_;
    if (HasSharedUnaryLayout(*node_)) {
      if (!source_->Next(row)) {
        Close();
        return false;
      }
      return true;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (!source_->Next(&input)) {
      Close();
      return false;
    }
    *row = ForwardUnaryOutput(*node_, std::move(input));
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

class PathBuildOperator final : public PullOperator {
 public:
  PathBuildOperator(const PhysicalPlanNode &node, RuntimeState &state,
                    std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<PathBuildOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~PathBuildOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    while (!closed_) {
      state_->CheckCancelled();
      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        Close();
        return false;
      }
      SlottedRow output = CopyUnaryOutput(*node_, input);
      if (TryBindSlot(&output, data_->path_output_slot,
                      BuildPathValue(*data_, input, state_))) {
        *row = std::move(output);
        return true;
      }
    }
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
  const PathBuildOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  bool closed_ = false;
};

class ProcedureCallOperator final : public PullOperator {
 public:
  ProcedureCallOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<ProcedureCallOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~ProcedureCallOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    return NextProcedure(row);
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    ReleaseBuffer();
    closed_ = true;
  }

 private:
  void ReleaseBuffer() noexcept {
    state_->memory_tracker.Release(buffer_reserved_bytes_);
    buffer_reserved_bytes_ = 0;
    buffer_.clear();
    buffer_index_ = 0;
  }

  bool PullInput(SlottedRow *input) {
    if (!source_->Next(input)) {
      Close();
      return false;
    }
    return true;
  }

  bool NextProcedure(SlottedRow *row) {
    while (buffer_index_ >= buffer_.size()) {
      ReleaseBuffer();
      SlottedRow input(node_->children[0]->output_slots);
      if (!PullInput(&input)) {
        return false;
      }
      for (const auto &record : ExecuteProcedure(*data_, input, state_)) {
        SlottedRow output(node_->output_slots);
        CopyMappings(input, &output, node_->child_mappings[0]);
        for (const auto &item : data_->yields) {
          const auto found = record.find(item.result_field);
          CHECK(found != record.end(), common::InvalidArgumentError,
                "unknown yield field for " + data_->procedure_name + ": " +
                    item.result_field);
          StoreEvaluatedValue(&output, item.output_slot, found->second,
                              *state_);
        }
        const std::size_t bytes = output.EstimatedHeapUsage();
        state_->memory_tracker.Reserve(bytes);
        buffer_reserved_bytes_ += bytes;
        buffer_.push_back(std::move(output));
      }
    }
    *row = std::move(buffer_[buffer_index_++]);
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  const ProcedureCallOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> buffer_;
  std::size_t buffer_index_ = 0;
  std::size_t buffer_reserved_bytes_ = 0;
  bool closed_ = false;
};

class UnwindOperator final : public PullOperator {
 public:
  UnwindOperator(const PhysicalPlanNode &node, RuntimeState &state,
                 std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<UnwindOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~UnwindOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    while (true) {
      if (input_.has_value() && value_index_ < values_.size()) {
        SlottedRow output(node_->output_slots);
        CopyMappings(*input_, &output, node_->child_mappings[0]);
        StoreEvaluatedValue(&output, data_->value_slot, values_[value_index_++],
                            *state_);
        *row = std::move(output);
        return true;
      }

      ReleaseValues();
      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        Close();
        return false;
      }
      Value value = Evaluate(data_->expression, input, *state_);
      if (value.IsNull()) {
        continue;
      }
      if (value.IsList()) {
        values_ = value.AsList();
      } else {
        values_.push_back(std::move(value));
      }
      std::size_t reserved_bytes = 0;
      for (const auto &item : values_) {
        reserved_bytes += EstimatedValueHeapUsage(item);
      }
      state_->memory_tracker.Reserve(reserved_bytes);
      reserved_bytes_ = reserved_bytes;
      input_.emplace(std::move(input));
    }
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    source_->Close();
    ReleaseValues();
    input_.reset();
    closed_ = true;
  }

 private:
  void ReleaseValues() noexcept {
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    values_.clear();
    value_index_ = 0;
  }

  const PhysicalPlanNode *node_ = nullptr;
  const UnwindOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> input_;
  std::vector<Value> values_;
  std::size_t value_index_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool closed_ = false;
};

class AssertIsNodeOperator final : public PullOperator {
 public:
  AssertIsNodeOperator(const PhysicalPlanNode &node, RuntimeState &state,
                       std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<AssertIsNodeOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~AssertIsNodeOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    std::optional<SlottedRow> local_input;
    SlottedRow *input = row;
    if (!HasSharedUnaryLayout(*node_)) {
      local_input.emplace(node_->children[0]->output_slots);
      input = &*local_input;
    }
    if (!source_->Next(input)) {
      Close();
      return false;
    }
    for (const auto &assertion : data_->nodes) {
      const Value value = ReadRowValue(*input, assertion.input_slot);
      CHECK(value.IsNull() || value.IsNode(), common::InvalidArgumentError,
            "expected node value: " + assertion.variable);
    }
    if (local_input.has_value()) {
      *row = ForwardUnaryOutput(*node_, std::move(*local_input));
    }
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
  const AssertIsNodeOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  bool closed_ = false;
};

template <typename Data>
class StreamingWriteOperator final : public PullOperator {
 public:
  StreamingWriteOperator(const PhysicalPlanNode &node, RuntimeState &state,
                         std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<Data>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~StreamingWriteOperator() override { Close(); }

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
    SlottedRow output = CopyUnaryOutput(*node_, input);
    ExecuteStreamingWrite(*data_, input, &output, state_);
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
  const Data *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  bool closed_ = false;
};

class OrderedDistinctOperator final : public PullOperator {
 public:
  OrderedDistinctOperator(const PhysicalPlanNode &node, RuntimeState &state,
                          std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<OrderedDistinctOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~OrderedDistinctOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }

    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      std::vector<Value> values =
          EvaluateGroupingValues(data_->grouping_items, input, state_);
      CompositeValueKey key{.values = values};
      if (last_key_.has_value() && ValueEqual{}(*last_key_, key)) {
        continue;
      }
      ReplaceLastKey(std::move(key));

      SlottedRow output(node_->output_slots);
      for (std::size_t index = 0; index < values.size(); ++index) {
        StoreEvaluatedValue(&output, data_->grouping_items[index].output_slot,
                            std::move(values[index]), *state_);
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
  const OrderedDistinctOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<CompositeValueKey> last_key_;
  std::size_t reserved_bytes_ = 0;
  bool closed_ = false;
};

class HashDistinctOperator final : public PullOperator {
 public:
  HashDistinctOperator(const PhysicalPlanNode &node, RuntimeState &state,
                       std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<HashDistinctOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~HashDistinctOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
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
    if (closed_) {
      return;
    }
    source_->Close();
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
    std::unordered_set<CompositeValueKey, ValueHash, ValueEqual> seen;
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      std::vector<Value> values =
          EvaluateGroupingValues(data_->grouping_items, input, state_);
      auto [key, inserted] = seen.insert(CompositeValueKey{.values = values});
      if (!inserted) {
        continue;
      }
      const std::size_t key_bytes = EstimatedKeyHeapUsage(*key);
      state_->memory_tracker.Reserve(key_bytes);
      reserved_bytes_ += key_bytes;
      SlottedRow output(node_->output_slots);
      for (std::size_t index = 0; index < values.size(); ++index) {
        StoreEvaluatedValue(&output, data_->grouping_items[index].output_slot,
                            std::move(values[index]), *state_);
      }
      BufferRow(std::move(output));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const HashDistinctOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class HashAggregationOperator final : public PullOperator {
 public:
  HashAggregationOperator(const PhysicalPlanNode &node, RuntimeState &state,
                          std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<HashAggregationOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~HashAggregationOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
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
    if (closed_) {
      return;
    }
    source_->Close();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  struct Group {
    Group(SlottedRow projected,
          const std::vector<AggregateAccumulator> &templates)
        : output(std::move(projected)), accumulators(templates) {}

    SlottedRow output;
    std::vector<AggregateAccumulator> accumulators;
  };

  void BufferRow(SlottedRow row) {
    const std::size_t bytes = row.EstimatedHeapUsage();
    state_->memory_tracker.Reserve(bytes);
    reserved_bytes_ += bytes;
    rows_.push_back(std::move(row));
  }

  void Initialize() {
    initialized_ = true;
    const std::vector<AggregateAccumulator> accumulator_templates =
        CreateAggregateAccumulators(data_->aggregation_items);
    std::vector<std::unique_ptr<Group>> groups;
    std::unordered_map<CompositeValueKey, std::size_t, ValueHash, ValueEqual>
        group_indexes;
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      std::vector<Value> values =
          EvaluateGroupingValues(data_->grouping_items, input, state_);
      auto [group, inserted] = group_indexes.emplace(
          CompositeValueKey{.values = values}, groups.size());
      if (inserted) {
        SlottedRow projected(node_->output_slots);
        for (std::size_t index = 0; index < values.size(); ++index) {
          StoreEvaluatedValue(&projected,
                              data_->grouping_items[index].output_slot,
                              std::move(values[index]), *state_);
        }
        groups.push_back(std::make_unique<Group>(std::move(projected),
                                                 accumulator_templates));
        const std::size_t key_bytes = EstimatedKeyHeapUsage(group->first);
        state_->memory_tracker.Reserve(key_bytes);
        reserved_bytes_ += key_bytes;
      }
      Group *group_state = groups[group->second].get();
      for (auto &accumulator : group_state->accumulators) {
        UpdateAggregateAccumulator(&accumulator, input, state_,
                                   &reserved_bytes_);
      }
    }
    if (data_->grouping_items.empty() && groups.empty()) {
      group_indexes.emplace(CompositeValueKey{}, 0U);
      groups.push_back(std::make_unique<Group>(SlottedRow(node_->output_slots),
                                               accumulator_templates));
    }
    for (auto &group : groups) {
      state_->CheckCancelled();
      for (auto &accumulator : group->accumulators) {
        StoreEvaluatedValue(&group->output, accumulator.item->output_slot,
                            FinalizeAggregateAccumulator(&accumulator, state_,
                                                         &reserved_bytes_),
                            *state_);
      }
      BufferRow(std::move(group->output));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const HashAggregationOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class OrderedAggregationOperator final : public PullOperator {
 public:
  OrderedAggregationOperator(const PhysicalPlanNode &node, RuntimeState &state,
                             std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<OrderedAggregationOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~OrderedAggregationOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_ || finished_) {
      return false;
    }

    SlottedRow input(node_->children[0]->output_slots);
    CompositeValueKey key;
    if (!TakeFirstInput(&input, &key)) {
      finished_ = true;
      Close();
      return false;
    }
    const std::size_t key_bytes = EstimatedKeyHeapUsage(key);
    state_->memory_tracker.Reserve(key_bytes);
    reserved_bytes_ += key_bytes;

    std::vector<AggregateAccumulator> accumulators =
        CreateAggregateAccumulators(data_->aggregation_items);

    SlottedRow output(node_->output_slots);
    for (std::size_t index = 0; index < key.values.size(); ++index) {
      StoreEvaluatedValue(&output, data_->grouping_items[index].output_slot,
                          key.values[index], *state_);
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
      CompositeValueKey next_key{.values = EvaluateGroupingValues(
                                     data_->grouping_items, next, state_)};
      if (!ValueEqual{}(key, next_key)) {
        BufferPending(std::move(next), std::move(next_key));
        break;
      }
      input = std::move(next);
    }

    for (auto &accumulator : accumulators) {
      StoreEvaluatedValue(
          &output, accumulator.item->output_slot,
          FinalizeAggregateAccumulator(&accumulator, state_, &reserved_bytes_),
          *state_);
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

  bool TakeFirstInput(SlottedRow *row, CompositeValueKey *key) {
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
    key->values = EvaluateGroupingValues(data_->grouping_items, *row, state_);
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
  const OrderedAggregationOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<PendingInput> pending_;
  std::size_t reserved_bytes_ = 0;
  bool source_exhausted_ = false;
  bool finished_ = false;
  bool closed_ = false;
};

class FullSortOperator final : public PullOperator {
 public:
  FullSortOperator(const PhysicalPlanNode &node, RuntimeState &state,
                   std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<FullSortOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~FullSortOperator() override { Close(); }

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
    source_->Close();
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

  void Initialize() {
    initialized_ = true;
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      Entry entry{.row = CopyUnaryOutput(*node_, input),
                  .keys = EvaluateSortKeys(data_->items, input, state_),
                  .sequence = sequence_++};
      entry.reserved_bytes = EstimatedSortEntryHeapUsage(entry.row, entry.keys);
      state_->memory_tracker.Reserve(entry.reserved_bytes);
      reserved_bytes_ += entry.reserved_bytes;
      entries_.push_back(std::move(entry));
    }
    std::stable_sort(entries_.begin(), entries_.end(),
                     [this](const Entry &left, const Entry &right) {
                       return SortKeysComeBefore(data_->items, left.keys,
                                                 left.sequence, right.keys,
                                                 right.sequence);
                     });
  }

  const PhysicalPlanNode *node_ = nullptr;
  const FullSortOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<Entry> entries_;
  std::uint64_t sequence_ = 0;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

class PartialSortOperator final : public PullOperator {
 public:
  PartialSortOperator(const PhysicalPlanNode &node, RuntimeState &state,
                      std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<PartialSortOp>(node)),
        state_(&state),
        source_(std::move(source)) {
    CHECK(data_->prefix > 0 && data_->prefix <= data_->items.size(),
          common::InternalError, "partial sort prefix is invalid");
  }

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
    return {.row = CopyUnaryOutput(*node_, input),
            .keys = EvaluateSortKeys(data_->items, input, state_),
            .sequence = sequence_++};
  }

  void Retain(Entry *entry) {
    CHECK(entry != nullptr, common::InternalError,
          "partial sort entry is null");
    entry->reserved_bytes =
        EstimatedSortEntryHeapUsage(entry->row, entry->keys);
    state_->memory_tracker.Reserve(entry->reserved_bytes);
    reserved_bytes_ += entry->reserved_bytes;
  }

  [[nodiscard]] bool SamePrefix(const Entry &left, const Entry &right) const {
    return SortKeysHaveSamePrefix(left.keys, right.keys, data_->prefix);
  }

  [[nodiscard]] bool ComesBefore(const Entry &left, const Entry &right) const {
    return SortKeysComeBefore(data_->items, left.keys, left.sequence,
                              right.keys, right.sequence);
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
  const PartialSortOp *data_ = nullptr;
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
      : node_(&node),
        data_(&OperatorData<PartialTopNOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

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
    return {.row = CopyUnaryOutput(*node_, input),
            .keys = EvaluateSortKeys(data_->items, input, state_),
            .sequence = sequence_++};
  }

  [[nodiscard]] std::size_t EstimatedEntryHeapUsage(const Entry &entry) const {
    return EstimatedSortEntryHeapUsage(entry.row, entry.keys);
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
    return SortKeysHaveSamePrefix(left.keys, right.keys, data_->prefix);
  }

  [[nodiscard]] bool ComesBefore(const Entry &left, const Entry &right) const {
    return SortKeysComeBefore(data_->items, left.keys, left.sequence,
                              right.keys, right.sequence);
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
              node_->children.size() == 1 && data_->prefix > 0 &&
              data_->prefix <= data_->items.size(),
          common::InternalError, "partial Top-N physical node is incomplete");
    CHECK(!data_->limit.RequiresInputRow(), common::InternalError,
          "partial Top-N LIMIT must not depend on an input row");
    remaining_ = static_cast<std::uint64_t>(
        EvaluatePaginationCount(data_->limit, nullptr, state_, "LIMIT"));
    if (remaining_ == 0) {
      source_->Close();
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const PartialTopNOp *data_ = nullptr;
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

class WriteBarrierOperator final : public PullOperator {
 public:
  WriteBarrierOperator(const PhysicalPlanNode &node, RuntimeState &state,
                       std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<WriteBarrierOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~WriteBarrierOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
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
    if (closed_) {
      return;
    }
    source_->Close();
    rows_.clear();
    state_->memory_tracker.Release(reserved_bytes_);
    reserved_bytes_ = 0;
    closed_ = true;
  }

 private:
  void Initialize() {
    initialized_ = true;
    (void)data_;
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      SlottedRow output = CopyUnaryOutput(*node_, input);
      const std::size_t bytes = output.EstimatedHeapUsage();
      state_->memory_tracker.Reserve(bytes);
      reserved_bytes_ += bytes;
      rows_.push_back(std::move(output));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const WriteBarrierOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t reserved_bytes_ = 0;
  std::size_t next_ = 0;
  bool initialized_ = false;
  bool closed_ = false;
};

template <typename Data>
class DeleteOperator final : public PullOperator {
 public:
  DeleteOperator(const PhysicalPlanNode &node, RuntimeState &state,
                 std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<Data>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~DeleteOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
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
    if (closed_) {
      return;
    }
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
    std::set<std::int64_t> node_ids;
    std::set<std::int64_t> relationship_ids;
    std::unordered_map<std::int64_t, RelationshipReference>
        graphdb_relationships;
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      state_->CheckCancelled();
      SlottedRow output = CopyUnaryOutput(*node_, input);
      BufferRow(std::move(output));
      for (const PhysicalExpression &expression : data_->expressions) {
        const Value entity = Evaluate(expression, input, *state_);
        if (entity.IsNull()) {
          continue;
        }
        CHECK(entity.IsNode() || entity.IsRelationship() || entity.IsPath(),
              common::InvalidArgumentError,
              "DELETE expression is not a graph entity or path");
        if (entity.IsNode()) {
          node_ids.insert(entity.AsNode().id);
        } else if (entity.IsRelationship()) {
          const Relationship &relationship = entity.AsRelationship();
          relationship_ids.insert(relationship.id);
          graphdb_relationships.insert_or_assign(
              relationship.id,
              RelationshipReference{.id = relationship.id,
                                    .type_id = relationship.type_id});
        } else {
          const Path &path = entity.AsPath();
          for (const auto &node : path.nodes) {
            CHECK(node != nullptr, common::InternalError,
                  "DELETE path contains a null node");
            node_ids.insert(node->id);
          }
          for (const auto &relationship : path.relationships) {
            CHECK(relationship != nullptr, common::InternalError,
                  "DELETE path contains a null relationship");
            relationship_ids.insert(relationship->id);
            graphdb_relationships.insert_or_assign(
                relationship->id,
                RelationshipReference{.id = relationship->id,
                                      .type_id = relationship->type_id});
          }
        }
      }
    }
    if constexpr (!Data::kDetach) {
      for (std::int64_t node_id : node_ids) {
        graphdb::Vertex vertex =
            GraphDBVertexById(*state_->transaction, node_id);
        auto relationships =
            vertex.NewEdgeIterator(graphdb::EdgeDirection::BOTH, {}, {});
        while (relationships->Valid()) {
          CHECK(
              relationship_ids.contains(relationships->GetEdge().GetNativeId()),
              common::InvalidArgumentError,
              "DELETE node still has relationships");
          relationships->Next();
        }
      }
    }
    for (std::int64_t relationship_id : relationship_ids) {
      DeleteGraphDBEdge(*state_->transaction,
                        graphdb_relationships.at(relationship_id));
    }
    for (std::int64_t node_id : node_ids) {
      DeleteGraphDBVertex(*state_->transaction, node_id);
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const Data *data_ = nullptr;
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
      : node_(&node),
        data_(&OperatorData<TopNOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

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
    return SortKeysComeBefore(data_->items, left.keys, left.sequence,
                              right.keys, right.sequence);
  }

  [[nodiscard]] std::size_t EstimatedEntryHeapUsage(const Entry &entry) const {
    return EstimatedSortEntryHeapUsage(entry.row, entry.keys);
  }

  [[nodiscard]] Entry CreateEntry(const SlottedRow &input) {
    Entry entry{.row = CopyUnaryOutput(*node_, input),
                .keys = EvaluateSortKeys(data_->items, input, state_),
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
              node_->children.size() == 1,
          common::InternalError, "Top-N physical node is incomplete");
    CHECK(!data_->limit.RequiresInputRow(), common::InternalError,
          "Top-N LIMIT must not depend on an input row");
    limit_ = static_cast<std::uint64_t>(
        EvaluatePaginationCount(data_->limit, nullptr, state_, "LIMIT"));
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
  const TopNOp *data_ = nullptr;
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

std::unique_ptr<PullOperator> BuildUnaryOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::unique_ptr<PullOperator> source) {
  CHECK(
      node.children.size() == 1, common::InternalError,
      std::string(ToString(node.kind)) + " physical node must have one child");
  switch (node.kind) {
    case PhysicalOperatorKind::kFilter:
      return std::make_unique<FilterOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kProjection:
      return std::make_unique<ProjectionOperator>(node, state,
                                                  std::move(source));
    case PhysicalOperatorKind::kSkip:
      return std::make_unique<SkipOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kLimit:
      return std::make_unique<LimitOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kProduceResults:
      return std::make_unique<ProduceResultsOperator>(node, state,
                                                      std::move(source));
    case PhysicalOperatorKind::kTopN:
      return std::make_unique<TopNOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kPartialTopN:
      return std::make_unique<PartialTopNOperator>(node, state,
                                                   std::move(source));
    case PhysicalOperatorKind::kOrderedDistinct:
      return std::make_unique<OrderedDistinctOperator>(node, state,
                                                       std::move(source));
    case PhysicalOperatorKind::kHashDistinct:
      return std::make_unique<HashDistinctOperator>(node, state,
                                                    std::move(source));
    case PhysicalOperatorKind::kOrderedAggregation:
      return std::make_unique<OrderedAggregationOperator>(node, state,
                                                          std::move(source));
    case PhysicalOperatorKind::kHashAggregation:
      return std::make_unique<HashAggregationOperator>(node, state,
                                                       std::move(source));
    case PhysicalOperatorKind::kPartialSort:
      return std::make_unique<PartialSortOperator>(node, state,
                                                   std::move(source));
    case PhysicalOperatorKind::kFullSort:
      return std::make_unique<FullSortOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kDelete:
      return std::make_unique<DeleteOperator<DeleteOp>>(node, state,
                                                        std::move(source));
    case PhysicalOperatorKind::kDetachDelete:
      return std::make_unique<DeleteOperator<DetachDeleteOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kWriteBarrier:
      return std::make_unique<WriteBarrierOperator>(node, state,
                                                    std::move(source));
    case PhysicalOperatorKind::kPathBuild:
      return std::make_unique<PathBuildOperator>(node, state,
                                                 std::move(source));
    case PhysicalOperatorKind::kProcedureCall:
      return std::make_unique<ProcedureCallOperator>(node, state,
                                                     std::move(source));
    case PhysicalOperatorKind::kUnwind:
      return std::make_unique<UnwindOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kAssertIsNode:
      return std::make_unique<AssertIsNodeOperator>(node, state,
                                                    std::move(source));
    case PhysicalOperatorKind::kCreateNode:
      return std::make_unique<StreamingWriteOperator<CreateNodeOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kCreateRelationship:
      return std::make_unique<StreamingWriteOperator<CreateRelationshipOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kSetProperty:
      return std::make_unique<StreamingWriteOperator<SetPropertyOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kSetProperties:
      return std::make_unique<StreamingWriteOperator<SetPropertiesOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kSetLabels:
      return std::make_unique<StreamingWriteOperator<SetLabelsOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kRemoveProperty:
      return std::make_unique<StreamingWriteOperator<RemovePropertyOp>>(
          node, state, std::move(source));
    case PhysicalOperatorKind::kRemoveLabels:
      return std::make_unique<StreamingWriteOperator<RemoveLabelsOp>>(
          node, state, std::move(source));
    default:
      THROW(common::InternalError, "not a unary physical operator: " +
                                       std::string(ToString(node.kind)));
  }
}

}  // namespace rg::slotted
