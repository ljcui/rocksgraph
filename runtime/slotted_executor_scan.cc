#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "common/exception.h"
#include "graphdb/edge_iterator.h"
#include "graphdb/graph_db.h"
#include "graphdb/vertex_iterator.h"
#include "ir/query_ir_internal.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graphdb_access.h"
#include "runtime/slotted_executor_internal.h"

namespace rg::slotted {

graphdb::EdgeDirection GraphDBDirection(PhysicalExpandDirection direction) {
  if (direction == PhysicalExpandDirection::kOutgoing) {
    return graphdb::EdgeDirection::OUTGOING;
  }
  if (direction == PhysicalExpandDirection::kIncoming) {
    return graphdb::EdgeDirection::INCOMING;
  }
  return graphdb::EdgeDirection::BOTH;
}

struct IndexRangeBound {
  Value value;
  bool inclusive = false;
};

struct IndexRange {
  std::optional<Value> prefix;
  std::vector<IndexRangeBound> lower_bounds;
  std::vector<IndexRangeBound> upper_bounds;
};

std::optional<std::int64_t> NextPhysicalExpandNode(
    const Relationship &relationship, std::int64_t current_node_id,
    PhysicalExpandDirection direction) {
  if (direction == PhysicalExpandDirection::kOutgoing) {
    return relationship.start_node_id == current_node_id
               ? std::optional<std::int64_t>(relationship.end_node_id)
               : std::nullopt;
  }
  if (direction == PhysicalExpandDirection::kIncoming) {
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
    RG_CHECK(expression != nullptr, common::InternalError,
             "index range predicate is null");
    if (expression->Is(ast::ASTNodeType::kStringPredicateExpression)) {
      const auto &prefix =
          *ast::CastAst<ast::StringPredicateExpression>(expression);
      RG_CHECK(prefix.op == "STARTS WITH" && is_property(prefix.left.get()) &&
                   prefix.right != nullptr,
               common::InternalError, "invalid index prefix predicate");
      range.prefix = Evaluate(*prefix.right, argument,
                              predicate.PrecomputedExpressions(), state);
      continue;
    }
    RG_CHECK(expression->Is(ast::ASTNodeType::kComparisonExpression),
             common::InternalError, "invalid index range predicate");
    const auto &comparison =
        *ast::CastAst<ast::ComparisonExpression>(expression);
    const bool reversed = !is_property(comparison.left.get());
    RG_CHECK((!reversed || is_property(comparison.right.get())) &&
                 (comparison.op == "<" || comparison.op == "<=" ||
                  comparison.op == ">" || comparison.op == ">="),
             common::InternalError, "invalid index range comparison");
    const auto *bound =
        reversed ? comparison.left.get() : comparison.right.get();
    RG_CHECK(bound != nullptr, common::InternalError, "index bound is null");
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
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (graphdb_cursor_ == nullptr) {
      graphdb_cursor_ = state_->transaction->NewVertexIterator();
    }
    while (graphdb_cursor_->Valid()) {
      graphdb::Vertex vertex = graphdb_cursor_->GetVertex();
      graphdb_cursor_->Next();
      row->Reset(node_->output_slots);
      CopyMappings(*argument_, row, node_->argument_mapping);
      SetScannedNode(row, data_->output_slot, std::move(vertex));
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    graphdb_cursor_.reset();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const AllNodeScanOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::optional<SlottedRow> argument_;
  std::unique_ptr<graphdb::VertexIterator> graphdb_cursor_;
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
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    closed_ = true;
    *row =
        CopyMappedRow(argument_, node_->output_slots, node_->argument_mapping);
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
                   std::optional<SlottedRow> argument, std::size_t output_slot,
                   const std::vector<std::string> *labels_to_verify,
                   const std::vector<PhysicalExpression> *predicates)
      : node_(&node),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))),
        output_slot_(output_slot),
        labels_to_verify_(labels_to_verify),
        predicates_(predicates) {}

  ~NodeScanOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (graphdb_cursor_ == nullptr) {
      graphdb_cursor_ = OpenGraphDBCursor();
    }
    while (graphdb_cursor_->Valid()) {
      graphdb::Vertex vertex = graphdb_cursor_->GetVertex();
      graphdb_cursor_->Next();
      if (labels_to_verify_ != nullptr) {
        const auto labels = vertex.GetLabels();
        if (!std::all_of(labels_to_verify_->begin(), labels_to_verify_->end(),
                         [&](const std::string &label) {
                           return labels.contains(label);
                         })) {
          continue;
        }
      }
      row->Reset(node_->output_slots);
      CopyMappings(argument_, row, node_->argument_mapping);
      SetScannedNode(row, output_slot_, std::move(vertex));
      if (predicates_ != nullptr &&
          !std::all_of(predicates_->begin(), predicates_->end(),
                       [&](const PhysicalExpression &predicate) {
                         return PredicateIsTrue(
                             Evaluate(predicate, *row, *state_));
                       })) {
        continue;
      }
      return true;
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    graphdb_cursor_.reset();
    closed_ = true;
  }

 protected:
  [[nodiscard]] virtual std::unique_ptr<graphdb::VertexIterator>
  OpenGraphDBCursor() = 0;
  [[nodiscard]] const SlottedRow &Argument() const noexcept {
    return argument_;
  }
  [[nodiscard]] RuntimeState &State() const noexcept { return *state_; }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  std::size_t output_slot_ = 0;
  const std::vector<std::string> *labels_to_verify_ = nullptr;
  const std::vector<PhysicalExpression> *predicates_ = nullptr;
  std::unique_ptr<graphdb::VertexIterator> graphdb_cursor_;
  bool closed_ = false;
};

class NodeByLabelScanOperator final : public NodeScanOperator {
 public:
  NodeByLabelScanOperator(const PhysicalPlanNode &node, RuntimeState &state,
                          std::optional<SlottedRow> argument)
      : NodeScanOperator(node, state, std::move(argument),
                         OperatorData<NodeByLabelScanOp>(node).output_slot,
                         &OperatorData<NodeByLabelScanOp>(node).labels,
                         nullptr),
        data_(&OperatorData<NodeByLabelScanOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<graphdb::VertexIterator> OpenGraphDBCursor()
      override {
    if (data_->labels.empty()) {
      return State().transaction->NewVertexIterator();
    }
    return State().transaction->NewVertexIterator(data_->labels.front());
  }

  const NodeByLabelScanOp *data_ = nullptr;
};

class NodeIndexSeekOperator final : public NodeScanOperator {
 public:
  NodeIndexSeekOperator(const PhysicalPlanNode &node, RuntimeState &state,
                        std::optional<SlottedRow> argument)
      : NodeScanOperator(node, state, std::move(argument),
                         OperatorData<NodeIndexSeekOp>(node).output_slot,
                         nullptr, nullptr),
        data_(&OperatorData<NodeIndexSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<graphdb::VertexIterator> OpenGraphDBCursor()
      override {
    Value expected = Evaluate(data_->value, Argument(), State());
    return State().transaction->QueryVertexByPropertyIndex(
        data_->labels, data_->property_key, expected);
  }

  const NodeIndexSeekOp *data_ = nullptr;
};

class NodeIndexRangeSeekOperator final : public NodeScanOperator {
 public:
  NodeIndexRangeSeekOperator(const PhysicalPlanNode &node, RuntimeState &state,
                             std::optional<SlottedRow> argument)
      : NodeScanOperator(node, state, std::move(argument),
                         OperatorData<NodeIndexRangeSeekOp>(node).output_slot,
                         nullptr,
                         &OperatorData<NodeIndexRangeSeekOp>(node).predicates),
        data_(&OperatorData<NodeIndexRangeSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<graphdb::VertexIterator> OpenGraphDBCursor()
      override {
    const IndexRange range =
        EvaluateIndexRange(data_->predicates, data_->variable,
                           data_->property_key, Argument(), State());
    std::optional<Value> lower;
    std::optional<Value> upper;
    bool left_closed = true;
    bool right_closed = true;
    if (!range.prefix.has_value() && range.lower_bounds.size() == 1) {
      lower = range.lower_bounds.front().value;
      left_closed = range.lower_bounds.front().inclusive;
    }
    if (!range.prefix.has_value() && range.upper_bounds.size() == 1) {
      upper = range.upper_bounds.front().value;
      right_closed = range.upper_bounds.front().inclusive;
    }
    return State().transaction->QueryVertexByPropertyRange(
        data_->labels, data_->property_key, lower, upper, left_closed,
        right_closed);
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
      RG_CHECK(value.IsList(), common::InvalidArgumentError,
               "IN requires a list");
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
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    while (const std::optional<std::int64_t> id = values_.Next()) {
      try {
        graphdb::Vertex vertex = GraphDBVertexById(*state_->transaction, *id);
        SlottedRow output = CopyMappedRow(argument_, node_->output_slots,
                                          node_->argument_mapping);
        if (TryBindNode(&output, data_->output_slot, std::move(vertex))) {
          *row = std::move(output);
          return true;
        }
      } catch (const common::RocksGraphException &error) {
        if (error.code() == common::ErrorCode::VertexIdNotFound) {
          continue;
        }
        throw;
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

std::optional<graphdb::Edge> FindGraphDBRelationshipById(
    graphdb::Transaction &transaction,
    const std::vector<std::string> &relationship_types, std::int64_t id) {
  if (!relationship_types.empty()) {
    for (const auto &type : relationship_types) {
      const auto type_id = transaction.db()->id_generator().GetTid(type);
      if (!type_id.has_value()) {
        continue;
      }
      try {
        const RelationshipReference reference{.id = id, .type_id = *type_id};
        return GraphDBEdgeById(transaction, reference);
      } catch (const common::RocksGraphException &error) {
        if (error.code() != common::ErrorCode::EdgeIdNotFound) {
          throw;
        }
      }
    }
    return std::nullopt;
  }

  auto edges = transaction.NewEdgeIterator();
  while (edges->Valid()) {
    const graphdb::Edge &edge = edges->GetEdge();
    if (edge.GetNativeId() == id) {
      return edge;
    }
    edges->Next();
  }
  return std::nullopt;
}

bool EmitGraphDBRelationship(const PhysicalPlanNode &node,
                             const PhysicalRelationshipPattern &pattern,
                             const PhysicalRelationshipSlots &slots,
                             const std::vector<PhysicalExpression> *predicates,
                             const SlottedRow &argument, graphdb::Edge edge,
                             bool reverse,
                             std::optional<graphdb::Edge> *pending_reverse,
                             SlottedRow *row, RuntimeState &state) {
  Relationship relationship{.id = edge.GetNativeId(),
                            .start_node_id = edge.GetNativeStartId(),
                            .end_node_id = edge.GetNativeEndId(),
                            .type_id = edge.GetTypeId(),
                            .type = edge.GetType()};
  if (!RelationshipHasType(relationship, pattern.types)) {
    return false;
  }
  if (pattern.direction == PhysicalExpandDirection::kBoth && !reverse &&
      relationship.start_node_id != relationship.end_node_id) {
    *pending_reverse = edge;
  }
  const std::int64_t from_id =
      pattern.direction == PhysicalExpandDirection::kIncoming
          ? relationship.end_node_id
          : (reverse ? relationship.end_node_id : relationship.start_node_id);
  const std::int64_t to_id =
      pattern.direction == PhysicalExpandDirection::kIncoming
          ? relationship.start_node_id
          : (reverse ? relationship.start_node_id : relationship.end_node_id);
  graphdb::Vertex from = Endpoint(edge, from_id);
  graphdb::Vertex to = Endpoint(edge, to_id);
  row->Reset(node.output_slots);
  CopyMappings(argument, row, node.argument_mapping);
  if (!TryBindNode(row, slots.from_node_output_slot, std::move(from)) ||
      !TryBindOptionalEdge(row, slots.relationship_output_slot,
                           std::move(edge)) ||
      !TryBindNode(row, slots.to_node_output_slot, std::move(to))) {
    return false;
  }
  if (predicates != nullptr &&
      !std::all_of(predicates->begin(), predicates->end(),
                   [&](const PhysicalExpression &predicate) {
                     return PredicateIsTrue(Evaluate(predicate, *row, state));
                   })) {
    return false;
  }
  return true;
}

bool EmitPendingGraphDBRelationship(
    const PhysicalPlanNode &node, const PhysicalRelationshipPattern &pattern,
    const PhysicalRelationshipSlots &slots,
    const std::vector<PhysicalExpression> *predicates,
    const SlottedRow &argument, std::optional<graphdb::Edge> *pending_reverse,
    SlottedRow *row, RuntimeState &state) {
  if (!pending_reverse->has_value()) {
    return false;
  }
  graphdb::Edge edge = std::move(**pending_reverse);
  pending_reverse->reset();
  return EmitGraphDBRelationship(node, pattern, slots, predicates, argument,
                                 std::move(edge), true, pending_reverse, row,
                                 state);
}

class RelationshipScanOperator : public PullOperator {
 public:
  RelationshipScanOperator(const PhysicalPlanNode &node, RuntimeState &state,
                           std::optional<SlottedRow> argument,
                           const PhysicalRelationshipPattern &pattern,
                           const PhysicalRelationshipSlots &slots,
                           const std::vector<PhysicalExpression> *predicates)
      : node_(&node),
        state_(&state),
        argument_(LeafArgument(node, std::move(argument))),
        pattern_(&pattern),
        slots_(&slots),
        predicates_(predicates) {}

  ~RelationshipScanOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (graphdb_cursor_ == nullptr) {
      graphdb_cursor_ = OpenGraphDBCursor();
    }
    if (EmitPendingGraphDBRelationship(*node_, *pattern_, *slots_, predicates_,
                                       argument_, &graphdb_pending_reverse_,
                                       row, *state_)) {
      return true;
    }
    while (graphdb_cursor_->Valid()) {
      graphdb::Edge edge = graphdb_cursor_->GetEdge();
      graphdb_cursor_->Next();
      if (EmitGraphDBRelationship(*node_, *pattern_, *slots_, predicates_,
                                  argument_, std::move(edge), false,
                                  &graphdb_pending_reverse_, row, *state_) ||
          EmitPendingGraphDBRelationship(
              *node_, *pattern_, *slots_, predicates_, argument_,
              &graphdb_pending_reverse_, row, *state_)) {
        return true;
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    graphdb_cursor_.reset();
    graphdb_pending_reverse_.reset();
    closed_ = true;
  }

 protected:
  [[nodiscard]] virtual std::unique_ptr<graphdb::EdgeIterator>
  OpenGraphDBCursor() = 0;
  [[nodiscard]] const SlottedRow &Argument() const noexcept {
    return argument_;
  }
  [[nodiscard]] RuntimeState &State() const noexcept { return *state_; }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  const PhysicalRelationshipPattern *pattern_ = nullptr;
  const PhysicalRelationshipSlots *slots_ = nullptr;
  const std::vector<PhysicalExpression> *predicates_ = nullptr;
  std::unique_ptr<graphdb::EdgeIterator> graphdb_cursor_;
  std::optional<graphdb::Edge> graphdb_pending_reverse_;
  bool closed_ = false;
};

class RelationshipTypeScanOperator final : public RelationshipScanOperator {
 public:
  RelationshipTypeScanOperator(const PhysicalPlanNode &node,
                               RuntimeState &state,
                               std::optional<SlottedRow> argument)
      : RelationshipScanOperator(
            node, state, std::move(argument),
            OperatorData<RelationshipTypeScanOp>(node).pattern,
            OperatorData<RelationshipTypeScanOp>(node).slots, nullptr),
        data_(&OperatorData<RelationshipTypeScanOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<graphdb::EdgeIterator> OpenGraphDBCursor()
      override {
    return State().transaction->NewEdgeIterator(std::unordered_set<std::string>(
        data_->pattern.types.begin(), data_->pattern.types.end()));
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
            OperatorData<RelationshipIndexSeekOp>(node).pattern,
            OperatorData<RelationshipIndexSeekOp>(node).slots, nullptr),
        data_(&OperatorData<RelationshipIndexSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<graphdb::EdgeIterator> OpenGraphDBCursor()
      override {
    Value expected = Evaluate(data_->value, Argument(), State());
    return State().transaction->QueryEdgeByPropertyIndex(
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
            OperatorData<RelationshipIndexRangeSeekOp>(node).slots,
            &OperatorData<RelationshipIndexRangeSeekOp>(node).predicates),
        data_(&OperatorData<RelationshipIndexRangeSeekOp>(node)) {}

 private:
  [[nodiscard]] std::unique_ptr<graphdb::EdgeIterator> OpenGraphDBCursor()
      override {
    const IndexRange range =
        EvaluateIndexRange(data_->predicates, data_->pattern.relationship,
                           data_->property_key, Argument(), State());
    std::optional<Value> lower;
    std::optional<Value> upper;
    bool left_closed = true;
    bool right_closed = true;
    if (!range.prefix.has_value() && range.lower_bounds.size() == 1) {
      lower = range.lower_bounds.front().value;
      left_closed = range.lower_bounds.front().inclusive;
    }
    if (!range.prefix.has_value() && range.upper_bounds.size() == 1) {
      upper = range.upper_bounds.front().value;
      right_closed = range.upper_bounds.front().inclusive;
    }
    return State().transaction->QueryEdgeByPropertyRange(
        data_->pattern.types, data_->property_key, lower, upper, left_closed,
        right_closed);
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
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    state_->CheckCancelled();
    if (closed_) {
      return false;
    }
    if (EmitPendingGraphDBRelationship(
            *node_, data_->pattern, data_->slots, nullptr, argument_,
            &graphdb_pending_reverse_, row, *state_)) {
      return true;
    }
    while (const std::optional<std::int64_t> id = values_.Next()) {
      const auto relationship = FindGraphDBRelationshipById(
          *state_->transaction, data_->pattern.types, *id);
      if (!relationship.has_value()) {
        continue;
      }
      if (EmitGraphDBRelationship(*node_, data_->pattern, data_->slots, nullptr,
                                  argument_, std::move(*relationship), false,
                                  &graphdb_pending_reverse_, row, *state_) ||
          EmitPendingGraphDBRelationship(
              *node_, data_->pattern, data_->slots, nullptr, argument_,
              &graphdb_pending_reverse_, row, *state_)) {
        return true;
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    values_.Close();
    graphdb_pending_reverse_.reset();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const RelationshipByIdSeekOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  SlottedRow argument_;
  IdSeekValues values_;
  std::optional<graphdb::Edge> graphdb_pending_reverse_;
  bool closed_ = false;
};

class FixedExpandOperatorBase : public PullOperator {
 protected:
  FixedExpandOperatorBase(const PhysicalPlanNode &node, RuntimeState &state,
                          std::unique_ptr<PullOperator> source,
                          const PhysicalRelationshipPattern &pattern,
                          std::size_t from_node_input_slot,
                          std::optional<std::size_t> to_node_input_slot,
                          std::size_t relationship_output_slot,
                          std::optional<std::size_t> to_node_output_slot)
      : node_(&node),
        state_(&state),
        source_(std::move(source)),
        pattern_(&pattern),
        from_node_input_slot_(from_node_input_slot),
        to_node_input_slot_(to_node_input_slot),
        relationship_output_slot_(relationship_output_slot),
        to_node_output_slot_(to_node_output_slot) {}

 public:
  ~FixedExpandOperatorBase() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    while (!closed_) {
      state_->CheckCancelled();
      if (graphdb_cursor_ != nullptr) {
        while (graphdb_cursor_->Valid()) {
          graphdb::Edge edge = graphdb_cursor_->GetEdge();
          graphdb_cursor_->Next();
          Relationship relationship{.id = edge.GetNativeId(),
                                    .start_node_id = edge.GetNativeStartId(),
                                    .end_node_id = edge.GetNativeEndId(),
                                    .type_id = edge.GetTypeId()};
          const std::optional<std::int64_t> to = NextPhysicalExpandNode(
              relationship, from_id_, pattern_->direction);
          if (!to.has_value() ||
              (to_node_input_slot_.has_value() && *to != to_id_)) {
            continue;
          }
          SlottedRow output = CopyMappedRow(*input_, node_->output_slots,
                                            node_->child_mappings.front());
          graphdb::Vertex to_vertex = Endpoint(edge, *to);
          if (!TryBindEdge(&output, relationship_output_slot_,
                           std::move(edge)) ||
              (to_node_output_slot_.has_value() &&
               !TryBindNode(&output, *to_node_output_slot_,
                            std::move(to_vertex)))) {
            continue;
          }
          *row = std::move(output);
          return true;
        }
        graphdb_cursor_.reset();
        input_.reset();
      }

      SlottedRow input(node_->children[0]->output_slots);
      if (!source_->Next(&input)) {
        Close();
        return false;
      }
      from_id_ = NodeId(input, from_node_input_slot_);
      to_id_ = to_node_input_slot_.has_value()
                   ? NodeId(input, *to_node_input_slot_)
                   : -1;
      if (from_id_ < 0 || (to_node_input_slot_.has_value() && to_id_ < 0)) {
        continue;
      }
      input_.emplace(std::move(input));
      auto vertex = GraphDBVertexById(*state_->transaction, from_id_);
      graphdb_cursor_ = vertex.NewEdgeIterator(
          GraphDBDirection(pattern_->direction),
          std::unordered_set<std::string>(pattern_->types.begin(),
                                          pattern_->types.end()),
          {});
    }
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    graphdb_cursor_.reset();
    input_.reset();
    source_->Close();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  const PhysicalRelationshipPattern *pattern_ = nullptr;
  std::size_t from_node_input_slot_ = 0;
  std::optional<std::size_t> to_node_input_slot_;
  std::size_t relationship_output_slot_ = 0;
  std::optional<std::size_t> to_node_output_slot_;
  std::optional<SlottedRow> input_;
  std::unique_ptr<graphdb::EdgeIterator> graphdb_cursor_;
  std::int64_t from_id_ = -1;
  std::int64_t to_id_ = -1;
  bool closed_ = false;
};

class ExpandOperator final : public FixedExpandOperatorBase {
 public:
  ExpandOperator(const PhysicalPlanNode &node, RuntimeState &state,
                 std::unique_ptr<PullOperator> source)
      : FixedExpandOperatorBase(
            node, state, std::move(source),
            OperatorData<ExpandOp>(node).pattern,
            OperatorData<ExpandOp>(node).from_node_input_slot, std::nullopt,
            OperatorData<ExpandOp>(node).relationship_output_slot,
            OperatorData<ExpandOp>(node).to_node_output_slot) {}
};

class ExpandIntoOperator final : public FixedExpandOperatorBase {
 public:
  ExpandIntoOperator(const PhysicalPlanNode &node, RuntimeState &state,
                     std::unique_ptr<PullOperator> source)
      : FixedExpandOperatorBase(
            node, state, std::move(source),
            OperatorData<ExpandIntoOp>(node).pattern,
            OperatorData<ExpandIntoOp>(node).from_node_input_slot,
            OperatorData<ExpandIntoOp>(node).to_node_input_slot,
            OperatorData<ExpandIntoOp>(node).relationship_output_slot,
            std::nullopt) {}
};

class VarExpandOperator final : public PullOperator {
 public:
  VarExpandOperator(const PhysicalPlanNode &node, RuntimeState &state,
                    std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<VarExpandOp>(node)),
        state_(&state),
        source_(std::move(source)) {
    RG_CHECK(data_->length.min.value_or(1) >= 0 &&
                 data_->length.max.value_or(0) >= 0,
             common::InvalidArgumentError, "negative variable path length");
  }
  ~VarExpandOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    return NextGraphDB(row);
  }

  bool NextGraphDB(SlottedRow *row) {
    while (!closed_) {
      state_->CheckCancelled();
      if (frames_.empty()) {
        input_.reset();
        SlottedRow input(node_->children[0]->output_slots);
        if (!source_->Next(&input)) {
          Close();
          return false;
        }
        const std::int64_t from = NodeId(input, data_->from_node_input_slot);
        if (from < 0) {
          continue;
        }
        bound_to_.reset();
        if (data_->to_node_input_slot.has_value() &&
            input.IsInitialized(*data_->to_node_input_slot)) {
          const std::int64_t to = NodeId(input, *data_->to_node_input_slot);
          if (to < 0) {
            continue;
          }
          bound_to_ = to;
        }
        min_ = static_cast<std::size_t>(data_->length.min.value_or(1));
        max_ = data_->length.max.has_value()
                   ? static_cast<std::size_t>(*data_->length.max)
                   : std::numeric_limits<std::size_t>::max();
        if (min_ > max_) {
          continue;
        }
        input_.emplace(std::move(input));
        Push(from);
      }

      Frame &frame = frames_.back();
      if (!frame.emitted) {
        frame.emitted = true;
        if (path_.size() >= min_ &&
            (!bound_to_.has_value() || frame.node == *bound_to_)) {
          Value::List relationships;
          relationships.reserve(path_references_.size());
          for (std::size_t i = 0; i < path_references_.size(); ++i) {
            const std::size_t index = data_->reverse_relationships
                                          ? path_references_.size() - i - 1
                                          : i;
            const RelationshipReference relationship = path_references_[index];
            relationships.emplace_back(Value(
                MaterializeGraphDBEdge(*state_->transaction, relationship)));
          }
          auto output = CopyMappedRow(*input_, node_->output_slots,
                                      node_->child_mappings.front());
          if (TryBindSlot(&output, data_->relationship_output_slot,
                          Value(std::move(relationships))) &&
              TryBindNode(&output, data_->to_node_output_slot, frame.node,
                          *state_)) {
            *row = std::move(output);
            return true;
          }
        }
      }
      if (path_.size() == max_) {
        Pop();
        continue;
      }
      if (frame.graphdb_cursor == nullptr) {
        auto vertex = GraphDBVertexById(*state_->transaction, frame.node);
        frame.graphdb_cursor = vertex.NewEdgeIterator(
            GraphDBDirection(data_->pattern.direction),
            std::unordered_set<std::string>(data_->pattern.types.begin(),
                                            data_->pattern.types.end()),
            {});
      }
      bool advanced = false;
      while (frame.graphdb_cursor->Valid()) {
        const graphdb::Edge edge = frame.graphdb_cursor->GetEdge();
        frame.graphdb_cursor->Next();
        const RelationshipReference relationship{.id = edge.GetNativeId(),
                                                 .type_id = edge.GetTypeId()};
        if (used_.contains(relationship.id)) {
          continue;
        }
        const Relationship value{.id = relationship.id,
                                 .start_node_id = edge.GetNativeStartId(),
                                 .end_node_id = edge.GetNativeEndId(),
                                 .type_id = relationship.type_id};
        const std::optional<std::int64_t> next =
            NextPhysicalExpandNode(value, frame.node, data_->pattern.direction);
        if (!next.has_value()) {
          continue;
        }
        Push(*next);
        used_.insert(relationship.id);
        path_.push_back(relationship.id);
        path_references_.push_back(relationship);
        advanced = true;
        break;
      }
      if (!advanced) {
        Pop();
      }
    }
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    for (auto &frame : frames_) {
      if (frame.graphdb_cursor != nullptr) {
        frame.graphdb_cursor.reset();
      }
    }
    frames_.clear();
    path_.clear();
    path_references_.clear();
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
    std::unique_ptr<graphdb::EdgeIterator> graphdb_cursor;
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
    frames_.pop_back();
    if (!path_.empty()) {
      used_.erase(path_.back());
      path_.pop_back();
    }
    if (!path_references_.empty()) {
      path_references_.pop_back();
    }
    state_->memory_tracker.Release(kFrameBytes);
    reserved_bytes_ -= kFrameBytes;
  }
  const PhysicalPlanNode *node_;
  const VarExpandOp *data_;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> input_;
  std::optional<std::int64_t> bound_to_;
  std::vector<Frame> frames_;
  std::vector<std::int64_t> path_;
  std::vector<RelationshipReference> path_references_;
  std::unordered_set<std::int64_t> used_;
  std::size_t min_ = 1;
  std::size_t max_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool closed_ = false;
};

class PruningVarExpandOperator final : public PullOperator {
 public:
  PruningVarExpandOperator(const PhysicalPlanNode &node, RuntimeState &state,
                           std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<PruningVarExpandOp>(node)),
        state_(&state),
        source_(std::move(source)) {
    RG_CHECK(data_->pattern.direction != PhysicalExpandDirection::kBoth,
             common::InternalError,
             "pruning variable expand cannot be undirected");
    RG_CHECK(data_->length.min.value_or(1) >= 0 &&
                 data_->length.min.value_or(1) <= 1 &&
                 data_->length.max.value_or(0) >= 0,
             common::InvalidArgumentError,
             "unsupported pruning variable path length");
  }
  ~PruningVarExpandOperator() override { Close(); }

  bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    return NextGraphDB(row);
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    graphdb_cursor_.reset();
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
    auto output = CopyMappedRow(*input_, node_->output_slots,
                                node_->child_mappings.front());
    if (!TryBindNode(&output, data_->to_node_output_slot, node, *state_)) {
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

  bool NextGraphDB(SlottedRow *row) {
    while (!closed_) {
      state_->CheckCancelled();
      if (!input_.has_value()) {
        SlottedRow input(node_->children[0]->output_slots);
        if (!source_->Next(&input)) {
          Close();
          return false;
        }
        start_ = NodeId(input, data_->from_node_input_slot);
        if (start_ < 0) {
          continue;
        }
        input_.emplace(std::move(input));
        max_ = data_->length.max.has_value()
                   ? static_cast<std::size_t>(*data_->length.max)
                   : std::numeric_limits<std::size_t>::max();
        Add(start_, 0);
        start_emitted_ = data_->length.min.value_or(1) == 0;
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
        if (graphdb_cursor_ == nullptr) {
          auto vertex = GraphDBVertexById(*state_->transaction, from);
          graphdb_cursor_ = vertex.NewEdgeIterator(
              GraphDBDirection(data_->pattern.direction),
              std::unordered_set<std::string>(data_->pattern.types.begin(),
                                              data_->pattern.types.end()),
              {});
        }
        while (graphdb_cursor_->Valid()) {
          auto edge = graphdb_cursor_->GetEdge();
          graphdb_cursor_->Next();
          const Relationship relationship{
              .id = edge.GetNativeId(),
              .start_node_id = edge.GetNativeStartId(),
              .end_node_id = edge.GetNativeEndId(),
              .type_id = edge.GetTypeId(),
              .type = edge.GetType()};
          if (!RelationshipHasType(relationship, data_->pattern.types)) {
            continue;
          }
          const std::int64_t to =
              data_->pattern.direction == PhysicalExpandDirection::kIncoming
                  ? edge.GetNativeStartId()
                  : edge.GetNativeEndId();
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
        graphdb_cursor_.reset();
        ++head_;
      }
      ClearInput();
    }
    return false;
  }
  const PhysicalPlanNode *node_;
  const PruningVarExpandOp *data_;
  RuntimeState *state_;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> input_;
  std::vector<std::pair<std::int64_t, std::size_t>> queue_;
  std::unordered_set<std::int64_t> visited_;
  std::unique_ptr<graphdb::EdgeIterator> graphdb_cursor_;
  std::int64_t start_ = -1;
  std::size_t head_ = 0;
  std::size_t max_ = 0;
  std::size_t reserved_bytes_ = 0;
  bool start_emitted_ = false;
  bool closed_ = false;
};

class OptionalExpandOperator final : public PullOperator {
 public:
  OptionalExpandOperator(const PhysicalPlanNode &node, RuntimeState &state,
                         std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<OptionalExpandOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~OptionalExpandOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    return NextGraphDB(row);
  }

  bool NextGraphDB(SlottedRow *row) {
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
        from_id_ = NodeId(*input_, data_->from_node_input_slot);
        if (from_id_ >= 0) {
          auto vertex = GraphDBVertexById(*state_->transaction, from_id_);
          graphdb_cursor_ = vertex.NewEdgeIterator(
              GraphDBDirection(data_->pattern.direction),
              std::unordered_set<std::string>(data_->pattern.types.begin(),
                                              data_->pattern.types.end()),
              {});
        }
      }

      while (graphdb_cursor_ != nullptr && graphdb_cursor_->Valid()) {
        state_->CheckCancelled();
        const graphdb::Edge edge = graphdb_cursor_->GetEdge();
        graphdb_cursor_->Next();
        const RelationshipReference reference{.id = edge.GetNativeId(),
                                              .type_id = edge.GetTypeId()};
        const Relationship relationship{
            .id = reference.id,
            .start_node_id = edge.GetNativeStartId(),
            .end_node_id = edge.GetNativeEndId(),
            .type_id = reference.type_id};
        const std::optional<std::int64_t> to = NextPhysicalExpandNode(
            relationship, from_id_, data_->pattern.direction);
        if (!to.has_value()) {
          continue;
        }
        SlottedRow output = CopyMappedRow(*input_, node_->output_slots,
                                          node_->child_mappings.front());
        graphdb::Vertex to_vertex = Endpoint(edge, *to);
        if (!TryBindEdge(&output, data_->relationship_output_slot,
                         std::move(edge)) ||
            !TryBindNode(&output, data_->to_node_output_slot,
                         std::move(to_vertex))) {
          continue;
        }
        if (!std::all_of(data_->predicates.begin(), data_->predicates.end(),
                         [&](const PhysicalExpression &predicate) {
                           return PredicateIsTrue(
                               Evaluate(predicate, output, *state_));
                         })) {
          continue;
        }
        matched_ = true;
        *row = std::move(output);
        return true;
      }
      graphdb_cursor_.reset();
      if (!matched_) {
        SlottedRow output = CopyMappedRow(*input_, node_->output_slots,
                                          node_->child_mappings.front());
        for (const std::size_t offset : data_->output_slots) {
          if (!output.IsInitialized(offset)) {
            output.SetNull(offset);
          }
        }
        input_.reset();
        *row = std::move(output);
        return true;
      }
      input_.reset();
    }
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    graphdb_cursor_.reset();
    input_.reset();
    source_->Close();
    closed_ = true;
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  const OptionalExpandOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> input_;
  std::unique_ptr<graphdb::EdgeIterator> graphdb_cursor_;
  std::int64_t from_id_ = -1;
  bool matched_ = false;
  bool closed_ = false;
};

class ProjectEndpointsOperator final : public PullOperator {
 public:
  ProjectEndpointsOperator(const PhysicalPlanNode &node, RuntimeState &state,
                           std::unique_ptr<PullOperator> source)
      : node_(&node),
        data_(&OperatorData<ProjectEndpointsOp>(node)),
        state_(&state),
        source_(std::move(source)) {}

  ~ProjectEndpointsOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    RG_CHECK(row != nullptr, common::InvalidArgumentError,
             "output row is null");
    while (!closed_) {
      state_->CheckCancelled();
      if (!input_.has_value()) {
        SlottedRow input(node_->children[0]->output_slots);
        if (!source_->Next(&input)) {
          Close();
          return false;
        }
        input_.emplace(std::move(input));
        PrepareEndpoints();
      }
      while (next_endpoint_ < endpoints_.size()) {
        const auto [from, to] = endpoints_[next_endpoint_++];
        SlottedRow output = CopyMappedRow(*input_, node_->output_slots,
                                          node_->child_mappings.front());
        if (TryBindNode(&output, data_->from_node_output_slot, from, *state_) &&
            TryBindNode(&output, data_->to_node_output_slot, to, *state_)) {
          *row = std::move(output);
          return true;
        }
      }
      input_.reset();
    }
    return false;
  }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    input_.reset();
    endpoints_.clear();
    source_->Close();
    closed_ = true;
  }

 private:
  void PrepareEndpoints() {
    endpoints_.clear();
    next_endpoint_ = 0;
    const Value value = ReadRowValue(*input_, data_->relationship_input_slot);
    if (value.IsNull()) {
      return;
    }

    std::vector<const Relationship *> relationships;
    if (data_->length.variable) {
      RG_CHECK(value.IsList(), common::InvalidArgumentError,
               "expected a relationship list");
      for (const Value &item : value.AsList()) {
        if (!item.IsRelationship()) {
          return;
        }
        relationships.push_back(&item.AsRelationship());
      }
      const std::size_t length = relationships.size();
      if (length < static_cast<std::size_t>(data_->length.min.value_or(1)) ||
          (data_->length.max.has_value() &&
           length > static_cast<std::size_t>(*data_->length.max))) {
        return;
      }
    } else {
      RG_CHECK(value.IsRelationship(), common::InvalidArgumentError,
               "expected a relationship");
      relationships.push_back(&value.AsRelationship());
    }

    std::unordered_set<std::int64_t> used;
    for (const Relationship *relationship : relationships) {
      state_->CheckCancelled();
      try {
        (void)GraphDBEdgeById(
            *state_->transaction,
            {.id = relationship->id, .type_id = relationship->type_id});
      } catch (const common::RocksGraphException &error) {
        if (error.code() == common::ErrorCode::EdgeIdNotFound) {
          return;
        }
        throw;
      }
      if (!RelationshipHasType(*relationship, data_->pattern.types) ||
          !used.insert(relationship->id).second) {
        return;
      }
    }

    if (relationships.empty()) {
      for (const std::optional<std::size_t> &offset :
           {data_->from_node_input_slot, data_->to_node_input_slot}) {
        if (offset.has_value() && input_->IsInitialized(*offset)) {
          const std::int64_t id = NodeId(*input_, *offset);
          if (id >= 0) {
            endpoints_.emplace_back(id, id);
          }
          return;
        }
      }
      return;
    }

    const Relationship &first = *relationships.front();
    std::vector<std::int64_t> starts;
    if (data_->pattern.direction != PhysicalExpandDirection::kIncoming) {
      starts.push_back(first.start_node_id);
    }
    if (data_->pattern.direction != PhysicalExpandDirection::kOutgoing &&
        (starts.empty() || starts.front() != first.end_node_id)) {
      starts.push_back(first.end_node_id);
    }
    for (const std::int64_t start : starts) {
      std::optional<std::int64_t> current = start;
      for (const Relationship *relationship : relationships) {
        current = NextPhysicalExpandNode(*relationship, *current,
                                         data_->pattern.direction);
        if (!current.has_value()) {
          break;
        }
      }
      if (current.has_value()) {
        endpoints_.emplace_back(start, *current);
      }
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  const ProjectEndpointsOp *data_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::optional<SlottedRow> input_;
  std::vector<std::pair<std::int64_t, std::int64_t>> endpoints_;
  std::size_t next_endpoint_ = 0;
  bool closed_ = false;
};

std::unique_ptr<PullOperator> BuildLeafOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::optional<SlottedRow> argument) {
  RG_CHECK(node.children.empty(), common::InternalError,
           std::string(ToString(node.kind)) +
               " physical node must not have children");
  switch (node.kind) {
    case PhysicalOperatorKind::kArgument:
      return std::make_unique<ArgumentOperator>(node, state,
                                                std::move(argument));
    case PhysicalOperatorKind::kAllNodeScan:
      return std::make_unique<AllNodeScanOperator>(node, state,
                                                   std::move(argument));
    case PhysicalOperatorKind::kNodeByLabelScan:
      return std::make_unique<NodeByLabelScanOperator>(node, state,
                                                       std::move(argument));
    case PhysicalOperatorKind::kNodeIndexSeek:
      return std::make_unique<NodeIndexSeekOperator>(node, state,
                                                     std::move(argument));
    case PhysicalOperatorKind::kNodeIndexRangeSeek:
      return std::make_unique<NodeIndexRangeSeekOperator>(node, state,
                                                          std::move(argument));
    case PhysicalOperatorKind::kRelationshipTypeScan:
      return std::make_unique<RelationshipTypeScanOperator>(
          node, state, std::move(argument));
    case PhysicalOperatorKind::kRelationshipIndexSeek:
      return std::make_unique<RelationshipIndexSeekOperator>(
          node, state, std::move(argument));
    case PhysicalOperatorKind::kRelationshipIndexRangeSeek:
      return std::make_unique<RelationshipIndexRangeSeekOperator>(
          node, state, std::move(argument));
    case PhysicalOperatorKind::kNodeByIdSeek:
      return std::make_unique<NodeByIdSeekOperator>(node, state,
                                                    std::move(argument));
    case PhysicalOperatorKind::kRelationshipByIdSeek:
      return std::make_unique<RelationshipByIdSeekOperator>(
          node, state, std::move(argument));
    default:
      RG_THROW(common::InternalError, "not a leaf physical operator: " +
                                          std::string(ToString(node.kind)));
  }
}

std::unique_ptr<PullOperator> BuildExpandOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::unique_ptr<PullOperator> source) {
  RG_CHECK(
      node.children.size() == 1, common::InternalError,
      std::string(ToString(node.kind)) + " physical node must have one child");
  switch (node.kind) {
    case PhysicalOperatorKind::kVarExpand:
      return std::make_unique<VarExpandOperator>(node, state,
                                                 std::move(source));
    case PhysicalOperatorKind::kPruningVarExpand:
      return std::make_unique<PruningVarExpandOperator>(node, state,
                                                        std::move(source));
    case PhysicalOperatorKind::kProjectEndpoints:
      return std::make_unique<ProjectEndpointsOperator>(node, state,
                                                        std::move(source));
    case PhysicalOperatorKind::kOptionalExpand:
      return std::make_unique<OptionalExpandOperator>(node, state,
                                                      std::move(source));
    case PhysicalOperatorKind::kExpand:
      return std::make_unique<ExpandOperator>(node, state, std::move(source));
    case PhysicalOperatorKind::kExpandInto:
      return std::make_unique<ExpandIntoOperator>(node, state,
                                                  std::move(source));
    default:
      RG_THROW(common::InternalError, "not an expand physical operator: " +
                                          std::string(ToString(node.kind)));
  }
}

}  // namespace rg::slotted
