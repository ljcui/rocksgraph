#include "runtime/slotted_executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "runtime/aggregation_evaluator.h"
#include "runtime/expression_evaluator.h"
#include "runtime/graph_access_executor.h"
#include "runtime/join_executor.h"
#include "runtime/procedure_executor.h"
#include "runtime/query_row_util.h"
#include "runtime/result_set_executor.h"
#include "runtime/row_operator_executor.h"
#include "runtime/write_executor.h"

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

struct RuntimeState {
  const GraphReader *graph_reader = nullptr;
  Storage *storage = nullptr;
  ExecutionContext context;
};

Value Evaluate(const ast::Expression &expression, const SlottedRow &row,
               const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
               const RuntimeState &state) {
  return EvaluateExpression(expression, row.Materialize(*state.graph_reader),
                            precomputed, state.context);
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
    Value value = source.Get(mapping.source_name, *state.graph_reader);
    if (!TryBindSlot(target, mapping.target_name, std::move(value),
                     *state.graph_reader)) {
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

class LeafOperator final : public PullOperator {
 public:
  LeafOperator(const PhysicalPlanNode &node, RuntimeState &state,
               std::optional<SlottedRow> argument)
      : node_(&node), state_(&state), argument_(std::move(argument)) {
    if (!argument_.has_value()) {
      argument_.emplace(EmptyArgument(node.argument_slots));
    }
  }

  ~LeafOperator() override { Close(); }

  [[nodiscard]] bool Next(SlottedRow *row) override {
    CHECK(row != nullptr, common::InvalidArgumentError, "output row is null");
    if (closed_) {
      return false;
    }
    const ir::LogicalPlan &plan = *node_->logical;
    if (plan.Type() == ir::LogicalPlanNodeType::kArgument) {
      if (emitted_argument_) {
        Close();
        return false;
      }
      emitted_argument_ = true;
      *row = argument_->CopyTo(node_->output_slots, *state_->graph_reader);
      return true;
    }

    EnsureCursor();
    if (pending_reverse_) {
      pending_reverse_ = false;
      if (EmitRelationship(pending_relationship_id_, true, row)) {
        return true;
      }
    }
    while (cursor_ != nullptr && cursor_->Next()) {
      const std::int64_t id = cursor_->Id();
      if (IsNodeLeaf(plan.Type())) {
        if (EmitNode(id, row)) {
          return true;
        }
      } else {
        if (EmitRelationship(id, false, row)) {
          return true;
        }
        if (pending_reverse_) {
          pending_reverse_ = false;
          if (EmitRelationship(pending_relationship_id_, true, row)) {
            return true;
          }
        }
      }
    }
    Close();
    return false;
  }

  void Close() noexcept override {
    if (cursor_ != nullptr) {
      cursor_->Close();
      cursor_.reset();
    }
    closed_ = true;
  }

 private:
  static bool IsNodeLeaf(ir::LogicalPlanNodeType type) {
    return type == ir::LogicalPlanNodeType::kAllNodeScan ||
           type == ir::LogicalPlanNodeType::kNodeByLabelScan ||
           type == ir::LogicalPlanNodeType::kNodeIndexSeek ||
           type == ir::LogicalPlanNodeType::kNodeIndexRangeSeek;
  }

  void EnsureCursor() {
    if (cursor_ != nullptr || initialized_) {
      return;
    }
    initialized_ = true;
    const ir::LogicalPlan &plan = *node_->logical;
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kAllNodeScan:
      case ir::LogicalPlanNodeType::kNodeByLabelScan:
        cursor_ = state_->graph_reader->ScanNodeIds();
        break;
      case ir::LogicalPlanNodeType::kNodeIndexSeek: {
        const auto &seek = static_cast<const ir::NodeIndexSeekPlan &>(plan);
        Value expected =
            Evaluate(*seek.ValueExpression(), *argument_, {}, *state_);
        cursor_ = state_->graph_reader->FindNodeIdsByIndex(
            seek.Labels(), seek.PropertyKey(), expected);
        break;
      }
      case ir::LogicalPlanNodeType::kNodeIndexRangeSeek: {
        const auto &seek =
            static_cast<const ir::NodeIndexRangeSeekPlan &>(plan);
        cursor_ = state_->graph_reader->NodeIdsInIndex(seek.Labels(),
                                                       seek.PropertyKey());
        break;
      }
      case ir::LogicalPlanNodeType::kRelationshipTypeScan:
        cursor_ = state_->graph_reader->ScanRelationshipIds();
        break;
      case ir::LogicalPlanNodeType::kRelationshipIndexSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipIndexSeekPlan &>(plan);
        Value expected =
            Evaluate(*seek.ValueExpression(), *argument_, {}, *state_);
        cursor_ = state_->graph_reader->FindRelationshipIdsByIndex(
            seek.Types(), seek.PropertyKey(), expected);
        break;
      }
      case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek: {
        const auto &seek =
            static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan);
        cursor_ = state_->graph_reader->RelationshipIdsInIndex(
            seek.Types(), seek.PropertyKey());
        break;
      }
      default:
        THROW(common::InternalError,
              "unsupported physical leaf: " + std::string(plan.Name()));
    }
  }

  bool EmitNode(std::int64_t id, SlottedRow *row) {
    const ir::LogicalPlan &plan = *node_->logical;
    const Node &node = *state_->graph_reader->NodeById(id);
    std::string variable;
    if (plan.Type() == ir::LogicalPlanNodeType::kAllNodeScan) {
      variable = static_cast<const ir::AllNodeScanPlan &>(plan).Variable();
    } else if (plan.Type() == ir::LogicalPlanNodeType::kNodeByLabelScan) {
      const auto &scan = static_cast<const ir::NodeByLabelScanPlan &>(plan);
      if (!NodeHasAllLabels(node, scan.Labels())) {
        return false;
      }
      variable = scan.Variable();
    } else if (plan.Type() == ir::LogicalPlanNodeType::kNodeIndexSeek) {
      variable = static_cast<const ir::NodeIndexSeekPlan &>(plan).Variable();
    } else {
      variable =
          static_cast<const ir::NodeIndexRangeSeekPlan &>(plan).Variable();
    }

    SlottedRow next =
        argument_->CopyTo(node_->output_slots, *state_->graph_reader);
    if (!TryBindSlot(&next, variable, Value(state_->graph_reader->NodeById(id)),
                     *state_->graph_reader)) {
      return false;
    }
    if (plan.Type() == ir::LogicalPlanNodeType::kNodeIndexRangeSeek) {
      const auto &seek = static_cast<const ir::NodeIndexRangeSeekPlan &>(plan);
      for (const ast::Expression *predicate : seek.Predicates()) {
        if (!PredicateIsTrue(Evaluate(*predicate, next, {}, *state_))) {
          return false;
        }
      }
    }
    *row = std::move(next);
    return true;
  }

  bool EmitRelationship(std::int64_t id, bool reverse, SlottedRow *row) {
    const ir::LogicalPlan &plan = *node_->logical;
    const Relationship &relationship =
        *state_->graph_reader->RelationshipById(id);
    std::string from;
    std::string rel;
    std::string to;
    ir::ExpandDirection direction = ir::ExpandDirection::kBoth;
    std::vector<std::string> types;
    const std::vector<const ast::Expression *> *predicates = nullptr;
    if (plan.Type() == ir::LogicalPlanNodeType::kRelationshipTypeScan) {
      const auto &scan =
          static_cast<const ir::RelationshipTypeScanPlan &>(plan);
      from = scan.FromNode();
      rel = scan.Relationship();
      to = scan.ToNode();
      direction = scan.Direction();
      types = scan.Types();
    } else if (plan.Type() == ir::LogicalPlanNodeType::kRelationshipIndexSeek) {
      const auto &scan =
          static_cast<const ir::RelationshipIndexSeekPlan &>(plan);
      from = scan.FromNode();
      rel = scan.Relationship();
      to = scan.ToNode();
      direction = scan.Direction();
      types = scan.Types();
    } else {
      const auto &scan =
          static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan);
      from = scan.FromNode();
      rel = scan.Relationship();
      to = scan.ToNode();
      direction = scan.Direction();
      types = scan.Types();
      predicates = &scan.Predicates();
    }
    if (!RelationshipHasType(relationship, types)) {
      return false;
    }

    if (direction == ir::ExpandDirection::kBoth && !reverse &&
        relationship.start_node_id != relationship.end_node_id) {
      pending_reverse_ = true;
      pending_relationship_id_ = id;
    }
    const std::int64_t from_id =
        direction == ir::ExpandDirection::kIncoming
            ? relationship.end_node_id
            : (reverse ? relationship.end_node_id : relationship.start_node_id);
    const std::int64_t to_id =
        direction == ir::ExpandDirection::kIncoming
            ? relationship.start_node_id
            : (reverse ? relationship.start_node_id : relationship.end_node_id);
    SlottedRow next =
        argument_->CopyTo(node_->output_slots, *state_->graph_reader);
    if (!TryBindSlot(&next, from,
                     Value(state_->graph_reader->NodeById(from_id)),
                     *state_->graph_reader) ||
        !TryBindSlot(
            &next, rel,
            Value(state_->graph_reader->RelationshipById(relationship.id)),
            *state_->graph_reader) ||
        !TryBindSlot(&next, to, Value(state_->graph_reader->NodeById(to_id)),
                     *state_->graph_reader)) {
      return false;
    }
    if (predicates != nullptr) {
      for (const ast::Expression *predicate : *predicates) {
        if (!PredicateIsTrue(Evaluate(*predicate, next, {}, *state_))) {
          return false;
        }
      }
    }
    *row = std::move(next);
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::optional<SlottedRow> argument_;
  std::unique_ptr<EntityIdCursor> cursor_;
  std::int64_t pending_relationship_id_ = -1;
  bool emitted_argument_ = false;
  bool initialized_ = false;
  bool pending_reverse_ = false;
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
    if (closed_) {
      return false;
    }
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kFilter:
        return NextFilter(row);
      case ir::LogicalPlanNodeType::kProjection:
        return NextProjection(row);
      case ir::LogicalPlanNodeType::kSkip:
        return NextSkip(row);
      case ir::LogicalPlanNodeType::kLimit:
        return NextLimit(row);
      case ir::LogicalPlanNodeType::kProduceResults:
        return NextPassthrough(row);
      case ir::LogicalPlanNodeType::kExpand:
      case ir::LogicalPlanNodeType::kExpandInto:
        return NextExpand(row);
      case ir::LogicalPlanNodeType::kVarExpand:
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
      relationship_cursor_->Close();
      relationship_cursor_.reset();
    }
    if (source_ != nullptr) {
      source_->Close();
    }
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

  bool NextPassthrough(SlottedRow *row) {
    SlottedRow input(node_->children[0]->output_slots);
    if (!PullInput(&input)) {
      return false;
    }
    *row = input.CopyTo(node_->output_slots, *state_->graph_reader);
    return true;
  }

  bool NextFilter(SlottedRow *row) {
    const auto &filter = static_cast<const ir::FilterPlan &>(*node_->logical);
    SlottedRow input(node_->children[0]->output_slots);
    while (PullInput(&input)) {
      if (PredicateIsTrue(Evaluate(*filter.Predicate(), input,
                                   filter.PrecomputedExpressions(), *state_))) {
        *row = input.CopyTo(node_->output_slots, *state_->graph_reader);
        return true;
      }
    }
    return false;
  }

  bool NextProjection(SlottedRow *row) {
    const auto &projection =
        static_cast<const ir::ProjectionPlan &>(*node_->logical);
    SlottedRow input(node_->children[0]->output_slots);
    if (!PullInput(&input)) {
      return false;
    }
    const QueryRow expression_row = input.Materialize(*state_->graph_reader);
    SlottedRow output(node_->output_slots);
    for (const auto &item : projection.Items()) {
      Value value = item.passthrough
                        ? LookupQueryVariable(expression_row, item.alias)
                        : EvaluateLogicalProjectionItem(item, expression_row,
                                                        state_->context);
      output.Set(item.alias, std::move(value));
    }
    *row = std::move(output);
    return true;
  }

  std::optional<std::int64_t> EvaluatePaginationCount(
      const ast::Expression *expression,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
      SlottedRow *first_row, std::string_view name) {
    CHECK(expression != nullptr, common::InvalidArgumentError,
          std::string(name) + " expression is null");
    const bool requires_row =
        !ast::CollectExpressionDependencies(*expression).empty() ||
        !precomputed.empty();
    QueryRow expression_row;
    if (requires_row) {
      if (!source_->Next(first_row)) {
        return std::nullopt;
      }
      expression_row = first_row->Materialize(*state_->graph_reader);
      first_pending_ = true;
    }
    const Value value = EvaluateExpression(*expression, expression_row,
                                           precomputed, state_->context);
    CHECK(value.IsInteger() && value.AsInteger() >= 0,
          common::InvalidArgumentError,
          std::string(name) + " requires a non-negative integer");
    return value.AsInteger();
  }

  bool NextSkip(SlottedRow *row) {
    const auto &skip = static_cast<const ir::SkipPlan &>(*node_->logical);
    if (!pagination_initialized_) {
      pagination_initialized_ = true;
      SlottedRow first(node_->children[0]->output_slots);
      pagination_count_ = EvaluatePaginationCount(
          skip.Skip(), skip.PrecomputedExpressions(), &first, "SKIP");
      if (!pagination_count_.has_value()) {
        Close();
        return false;
      }
      if (first_pending_) {
        pending_first_.emplace(std::move(first));
      }
    }
    while (pagination_seen_ < static_cast<std::uint64_t>(*pagination_count_)) {
      SlottedRow ignored(node_->children[0]->output_slots);
      if (pending_first_.has_value()) {
        ignored = std::move(*pending_first_);
        pending_first_.reset();
      } else if (!source_->Next(&ignored)) {
        Close();
        return false;
      }
      ++pagination_seen_;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (pending_first_.has_value()) {
      input = std::move(*pending_first_);
      pending_first_.reset();
    } else if (!source_->Next(&input)) {
      Close();
      return false;
    }
    *row = input.CopyTo(node_->output_slots, *state_->graph_reader);
    return true;
  }

  bool NextLimit(SlottedRow *row) {
    const auto &limit = static_cast<const ir::LimitPlan &>(*node_->logical);
    if (!pagination_initialized_) {
      pagination_initialized_ = true;
      SlottedRow first(node_->children[0]->output_slots);
      pagination_count_ = EvaluatePaginationCount(
          limit.Limit(), limit.PrecomputedExpressions(), &first, "LIMIT");
      if (!pagination_count_.has_value() || *pagination_count_ == 0) {
        Close();
        return false;
      }
      if (first_pending_) {
        pending_first_.emplace(std::move(first));
      }
    }
    if (pagination_seen_ >= static_cast<std::uint64_t>(*pagination_count_)) {
      Close();
      return false;
    }
    SlottedRow input(node_->children[0]->output_slots);
    if (pending_first_.has_value()) {
      input = std::move(*pending_first_);
      pending_first_.reset();
    } else if (!source_->Next(&input)) {
      Close();
      return false;
    }
    ++pagination_seen_;
    *row = input.CopyTo(node_->output_slots, *state_->graph_reader);
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
          if (!TryBindSlot(&output, rel_name,
                           Value(state_->graph_reader->RelationshipById(
                               relationship.id)),
                           *state_->graph_reader) ||
              (!into &&
               !TryBindSlot(&output, to_name,
                            Value(state_->graph_reader->NodeById(other)),
                            *state_->graph_reader))) {
            continue;
          }
          *row = std::move(output);
          return true;
        }
        relationship_cursor_->Close();
        relationship_cursor_.reset();
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
      relationship_cursor_ =
          ExpandCursor(*state_->graph_reader, current_from_id_, direction);
    }
  }

  bool NextBufferedPerInput(SlottedRow *row) {
    while (buffer_index_ >= buffer_.size()) {
      buffer_.clear();
      buffer_index_ = 0;
      SlottedRow input(node_->children[0]->output_slots);
      if (!PullInput(&input)) {
        return false;
      }
      QueryRows rows;
      const QueryRows input_rows{input.Materialize(*state_->graph_reader)};
      if (node_->logical->Type() == ir::LogicalPlanNodeType::kVarExpand) {
        rows = GraphAccessExecutor(*state_->graph_reader, state_->context)
                   .ExecuteVarExpand(
                       static_cast<const ir::VarExpandPlan &>(*node_->logical),
                       input_rows);
      } else if (node_->logical->Type() ==
                 ir::LogicalPlanNodeType::kPathBuild) {
        rows = GraphAccessExecutor(*state_->graph_reader, state_->context)
                   .ExecutePathBuild(
                       static_cast<const ir::PathBuildPlan &>(*node_->logical),
                       input_rows);
      } else {
        rows =
            ProcedureExecutor(*state_->graph_reader)
                .Execute(
                    static_cast<const ir::ProcedureCallPlan &>(*node_->logical),
                    input_rows);
      }
      for (const auto &result : rows) {
        buffer_.push_back(
            SlottedRow::FromQueryRow(node_->output_slots, result));
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
    const QueryRows input_rows{input.Materialize(*state_->graph_reader)};
    const WritePlanExecutor passthrough =
        [](const ir::LogicalPlan &, const QueryRows &rows) { return rows; };
    WriteExecutor executor(state_->storage, state_->context);
    QueryRows result;
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kCreateNode:
        result = executor.Execute(
            static_cast<const ir::CreateNodePlan &>(*node_->logical),
            input_rows, passthrough);
        break;
      case ir::LogicalPlanNodeType::kCreateRelationship:
        result = executor.Execute(
            static_cast<const ir::CreateRelationshipPlan &>(*node_->logical),
            input_rows, passthrough);
        break;
      case ir::LogicalPlanNodeType::kSetProperty:
        result = executor.Execute(
            static_cast<const ir::SetPropertyPlan &>(*node_->logical),
            input_rows, passthrough);
        break;
      case ir::LogicalPlanNodeType::kSetProperties:
        result = executor.Execute(
            static_cast<const ir::SetPropertiesPlan &>(*node_->logical),
            input_rows, passthrough);
        break;
      case ir::LogicalPlanNodeType::kSetLabels:
        result = executor.Execute(
            static_cast<const ir::SetLabelsPlan &>(*node_->logical), input_rows,
            passthrough);
        break;
      case ir::LogicalPlanNodeType::kRemoveProperty:
        result = executor.Execute(
            static_cast<const ir::RemovePropertyPlan &>(*node_->logical),
            input_rows, passthrough);
        break;
      case ir::LogicalPlanNodeType::kRemoveLabels:
        result = executor.Execute(
            static_cast<const ir::RemoveLabelsPlan &>(*node_->logical),
            input_rows, passthrough);
        break;
      default:
        THROW(common::InternalError, "unexpected streaming write operator");
    }
    CHECK(result.size() == 1, common::InternalError,
          "streaming write changed row cardinality");
    *row = SlottedRow::FromQueryRow(node_->output_slots, result.front());
    return true;
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::unique_ptr<EntityIdCursor> relationship_cursor_;
  std::optional<SlottedRow> current_input_;
  std::optional<SlottedRow> unwind_input_;
  std::optional<SlottedRow> pending_first_;
  std::vector<Value> unwind_values_;
  std::vector<SlottedRow> buffer_;
  std::size_t unwind_index_ = 0;
  std::size_t buffer_index_ = 0;
  std::int64_t current_from_id_ = -1;
  std::int64_t current_to_id_ = -1;
  std::optional<std::int64_t> pagination_count_;
  std::uint64_t pagination_seen_ = 0;
  bool pagination_initialized_ = false;
  bool first_pending_ = false;
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
    closed_ = true;
  }

 private:
  void Initialize() {
    initialized_ = true;
    if (node_->logical->Type() == ir::LogicalPlanNodeType::kWriteBarrier) {
      SlottedRow input(node_->children[0]->output_slots);
      while (source_->Next(&input)) {
        rows_.push_back(
            input.CopyTo(node_->output_slots, *state_->graph_reader));
      }
      return;
    }

    QueryRows input_rows;
    SlottedRow input(node_->children[0]->output_slots);
    while (source_->Next(&input)) {
      input_rows.push_back(input.Materialize(*state_->graph_reader));
    }
    QueryRows output_rows;
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kDistinct:
        output_rows =
            RowOperatorExecutor(state_->context)
                .Execute(static_cast<const ir::DistinctPlan &>(*node_->logical),
                         input_rows);
        break;
      case ir::LogicalPlanNodeType::kAggregation:
        output_rows =
            RowOperatorExecutor(state_->context)
                .Execute(
                    static_cast<const ir::AggregationPlan &>(*node_->logical),
                    input_rows);
        break;
      case ir::LogicalPlanNodeType::kSort:
        output_rows =
            ResultSetExecutor(state_->context)
                .Execute(static_cast<const ir::SortPlan &>(*node_->logical),
                         std::move(input_rows));
        break;
      case ir::LogicalPlanNodeType::kLimit:
        output_rows =
            ResultSetExecutor(state_->context)
                .Execute(static_cast<const ir::LimitPlan &>(*node_->logical),
                         std::move(input_rows));
        break;
      case ir::LogicalPlanNodeType::kDelete:
      case ir::LogicalPlanNodeType::kDetachDelete: {
        const WritePlanExecutor passthrough =
            [](const ir::LogicalPlan &, const QueryRows &rows) { return rows; };
        WriteExecutor executor(state_->storage, state_->context);
        if (node_->logical->Type() == ir::LogicalPlanNodeType::kDelete) {
          output_rows = executor.Execute(
              static_cast<const ir::DeletePlan &>(*node_->logical), input_rows,
              passthrough);
        } else {
          output_rows = executor.Execute(
              static_cast<const ir::DetachDeletePlan &>(*node_->logical),
              input_rows, passthrough);
        }
        break;
      }
      default:
        THROW(common::InternalError, "unsupported blocking unary operator: " +
                                         std::string(node_->logical->Name()));
    }
    rows_.reserve(output_rows.size());
    for (const auto &output : output_rows) {
      rows_.push_back(SlottedRow::FromQueryRow(node_->output_slots, output));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t next_ = 0;
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
  }

 private:
  static QueryRows Collect(PullOperator *source,
                           SlotConfigurationPtr source_slots,
                           const RuntimeState &state) {
    QueryRows rows;
    SlottedRow row(std::move(source_slots));
    while (source->Next(&row)) {
      rows.push_back(row.Materialize(*state.graph_reader));
    }
    return rows;
  }

  void Initialize() {
    initialized_ = true;
    QueryRows lhs_rows =
        Collect(lhs_.get(), node_->children[0]->output_slots, *state_);
    QueryRows rhs_rows =
        Collect(rhs_.get(), node_->children[1]->output_slots, *state_);
    JoinExecutor executor(state_->context);
    QueryRows output;
    switch (node_->logical->Type()) {
      case ir::LogicalPlanNodeType::kCartesianProduct:
        output = executor.Execute(
            static_cast<const ir::CartesianProductPlan &>(*node_->logical),
            lhs_rows, rhs_rows);
        break;
      case ir::LogicalPlanNodeType::kNodeHashJoin:
        output = executor.Execute(
            static_cast<const ir::NodeHashJoinPlan &>(*node_->logical),
            lhs_rows, rhs_rows);
        break;
      case ir::LogicalPlanNodeType::kValueHashJoin:
        output = executor.Execute(
            static_cast<const ir::ValueHashJoinPlan &>(*node_->logical),
            lhs_rows, rhs_rows);
        break;
      case ir::LogicalPlanNodeType::kPredicateJoin:
        output = executor.Execute(
            static_cast<const ir::PredicateJoinPlan &>(*node_->logical),
            lhs_rows, rhs_rows);
        break;
      default:
        THROW(common::InternalError, "unexpected blocking binary operator");
    }
    rows_.reserve(output.size());
    for (const auto &result : output) {
      rows_.push_back(SlottedRow::FromQueryRow(node_->output_slots, result));
    }
  }

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::vector<SlottedRow> rows_;
  std::size_t next_ = 0;
  bool initialized_ = false;
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
        std::string key;
        for (const auto &column : node_->output_slots->Columns()) {
          const std::string part =
              ValueKey(output.Get(column, *state_->graph_reader));
          key.append(std::to_string(part.size()));
          key.push_back(':');
          key.append(part);
        }
        if (!seen_.insert(std::move(key)).second) {
          continue;
        }
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
  }

 private:
  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  std::unique_ptr<PullOperator> lhs_;
  std::unique_ptr<PullOperator> rhs_;
  std::set<std::string> seen_;
  std::size_t side_ = 0;
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

  const PhysicalPlanNode *node_ = nullptr;
  RuntimeState *state_ = nullptr;
  OperatorFactory *factory_ = nullptr;
  std::unique_ptr<PullOperator> source_;
  std::vector<SlottedRow> rows_;
  std::size_t next_ = 0;
  bool initialized_ = false;
};

class OperatorFactory final {
 public:
  explicit OperatorFactory(RuntimeState &state) : state_(&state) {}

  std::unique_ptr<PullOperator> Build(
      const PhysicalPlanNode &node,
      std::optional<SlottedRow> argument = std::nullopt) {
    const ir::LogicalPlanNodeType type = node.logical->Type();
    if (node.logical->ChildCount() == 0) {
      return std::make_unique<LeafOperator>(node, *state_, std::move(argument));
    }
    if (type == ir::LogicalPlanNodeType::kApply ||
        type == ir::LogicalPlanNodeType::kSemiApply ||
        type == ir::LogicalPlanNodeType::kAntiSemiApply ||
        type == ir::LogicalPlanNodeType::kLetSemiApply ||
        type == ir::LogicalPlanNodeType::kRollUpApply ||
        type == ir::LogicalPlanNodeType::kOptionalApply) {
      return std::make_unique<ApplyOperator>(
          node, *state_, *this, Build(*node.children[0], std::move(argument)));
    }
    if (type == ir::LogicalPlanNodeType::kMerge) {
      return std::make_unique<MergeOperator>(
          node, *state_, *this, Build(*node.children[0], std::move(argument)));
    }
    if (node.logical->ChildCount() == 2) {
      auto lhs = Build(*node.children[0], argument);
      auto rhs = Build(*node.children[1], std::move(argument));
      if (type == ir::LogicalPlanNodeType::kUnion) {
        return std::make_unique<UnionOperator>(node, *state_, std::move(lhs),
                                               std::move(rhs));
      }
      return std::make_unique<BlockingBinaryOperator>(
          node, *state_, std::move(lhs), std::move(rhs));
    }

    auto source = Build(*node.children[0], std::move(argument));
    if (type == ir::LogicalPlanNodeType::kDistinct ||
        type == ir::LogicalPlanNodeType::kAggregation ||
        type == ir::LogicalPlanNodeType::kSort ||
        (type == ir::LogicalPlanNodeType::kLimit &&
         PlanContainsWrites(node.logical->Child(0))) ||
        type == ir::LogicalPlanNodeType::kWriteBarrier ||
        type == ir::LogicalPlanNodeType::kDelete ||
        type == ir::LogicalPlanNodeType::kDetachDelete) {
      return std::make_unique<BlockingUnaryOperator>(node, *state_,
                                                     std::move(source));
    }
    return std::make_unique<StreamingUnaryOperator>(node, *state_,
                                                    std::move(source));
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
  rhs_ = factory_->Build(*node_->children[1], *current_left_);
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
  const ir::LogicalPlanNodeType type = node_->logical->Type();
  while (true) {
    if (!current_left_.has_value() && !NextLeft()) {
      return false;
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
      SlottedRow rhs_row(node_->children[1]->output_slots);
      while (rhs_->Next(&rhs_row)) {
        values.push_back(
            rhs_row.Get(rollup.ValueVariable(), *state_->graph_reader));
      }
      SlottedRow output =
          current_left_->CopyTo(node_->output_slots, *state_->graph_reader);
      output.Set(rollup.CollectionVariable(), Value(std::move(values)));
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
        type == ir::LogicalPlanNodeType::kSemiApply
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
  QueryRows input_rows;
  SlottedRow input(node_->children[0]->output_slots);
  while (source_->Next(&input)) {
    input_rows.push_back(input.Materialize(*state_->graph_reader));
  }
  WritePlanExecutor execute = [this](const ir::LogicalPlan &child,
                                     const QueryRows &inputs) {
    if (&child == &node_->logical->Child(0)) {
      return inputs;
    }
    CHECK(&child == &node_->logical->Child(1), common::InternalError,
          "merge invoked an unexpected child plan");
    QueryRows output;
    for (const auto &input_row : inputs) {
      SlottedRow argument = SlottedRow::FromQueryRow(
          node_->children[1]->argument_slots, input_row);
      std::unique_ptr<PullOperator> rhs =
          factory_->Build(*node_->children[1], std::move(argument));
      SlottedRow rhs_row(node_->children[1]->output_slots);
      while (rhs->Next(&rhs_row)) {
        output.push_back(rhs_row.Materialize(*state_->graph_reader));
      }
      rhs->Close();
    }
    return output;
  };
  QueryRows output =
      WriteExecutor(state_->storage, state_->context)
          .Execute(static_cast<const ir::MergePlan &>(*node_->logical),
                   input_rows, execute);
  rows_.reserve(output.size());
  for (const auto &result : output) {
    rows_.push_back(SlottedRow::FromQueryRow(node_->output_slots, result));
  }
}

bool MergeOperator::Next(SlottedRow *row) {
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
}

}  // namespace

PhysicalExecutionResult ExecutePhysicalPlan(
    const PhysicalPlan &plan, const GraphReader &graph_reader, Storage *storage,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns, bool collect_results) {
  RuntimeState state{.graph_reader = &graph_reader,
                     .storage = storage,
                     .context = {.graph_reader = &graph_reader,
                                 .parameters = &parameters,
                                 .clock = ExecutionClock::Start()}};
  OperatorFactory factory(state);
  std::unique_ptr<PullOperator> root = factory.Build(plan.Root());
  PhysicalExecutionResult result;
  SlottedRow row(plan.Root().output_slots);
  while (root->Next(&row)) {
    if (!collect_results || result_columns.empty()) {
      continue;
    }
    std::vector<Value> values;
    values.reserve(result_columns.size());
    for (const auto &column : result_columns) {
      values.push_back(row.IsInitialized(column) ? row.Get(column, graph_reader)
                                                 : Value::Null());
    }
    result.rows.push_back(std::move(values));
  }
  root->Close();
  return result;
}

}  // namespace rg
