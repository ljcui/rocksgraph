#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "common/exception.h"
#include "graphdb/graph_entity.h"
#include "graphdb/transaction.h"
#include "runtime/composite_value_key.h"
#include "runtime/physical_executor.h"

namespace rg::execution {

class PullOperator {
 public:
  PullOperator() = default;
  PullOperator(const PullOperator &) = delete;
  PullOperator &operator=(const PullOperator &) = delete;
  virtual ~PullOperator() = default;

  [[nodiscard]] virtual bool Next(ExecutionRow *row) = 0;
  virtual void Close() noexcept = 0;
};

struct RuntimeExpressionProgram {
  std::vector<std::pair<const ast::Variable *, std::size_t>> variables;
  std::vector<std::pair<const ast::Parameter *, std::size_t>> parameters;
  std::vector<std::pair<std::string_view, std::size_t>> named_variables;
};

struct RuntimeState {
  RuntimeState(graphdb::Transaction &graphdb_transaction,
               const QueryParameters &parameters,
               QueryExecutionOptions options);

  void CheckCancelled() const;
  [[nodiscard]] const RuntimeExpressionProgram &ExpressionProgram(
      const ast::Expression &expression, const RowLayout &layout);

  graphdb::Transaction *transaction = nullptr;
  BoundQueryParameters bound_parameters;
  std::shared_ptr<QueryCancellationToken> cancellation;
  QueryMemoryTracker memory_tracker;
  ExecutionContext context;

 private:
  struct CachedExpression {
    const ast::Expression *expression = nullptr;
    const RowLayout *layout = nullptr;
    RuntimeExpressionProgram program;
  };
  std::vector<CachedExpression> expressions;
  std::optional<std::size_t> last_expression_index_;
};

class OperatorFactory final {
 public:
  explicit OperatorFactory(RuntimeState &state) : state_(&state) {}

  [[nodiscard]] std::unique_ptr<PullOperator> Build(
      const PhysicalPlanNode &node,
      std::optional<ExecutionRow> argument = std::nullopt);

 private:
  RuntimeState *state_ = nullptr;
};

template <typename T>
const T &OperatorData(const PhysicalPlanNode &node) {
  const T *data = std::get_if<T>(&node.data);
  RG_CHECK(data != nullptr, common::ErrorCode::InternalError,
           "physical operator payload does not match " +
               std::string(ToString(node.kind)));
  return *data;
}

[[nodiscard]] Value Evaluate(
    const ast::Expression &expression, const ExecutionRow &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    RuntimeState &state);
[[nodiscard]] Value Evaluate(const PhysicalExpression &expression,
                             const ExecutionRow &row, RuntimeState &state);

[[nodiscard]] ExecutionRow EmptyArgument(RowLayoutPtr layout);
void CopyMappings(const ExecutionRow &source, ExecutionRow *target,
                  const std::vector<RowMapping> &mappings);
[[nodiscard]] ExecutionRow CopyMappedRow(
    const ExecutionRow &source, RowLayoutPtr target,
    const std::vector<RowMapping> &mappings);
[[nodiscard]] Value ReadRowValue(const ExecutionRow &row, std::size_t offset);
[[nodiscard]] ExecutionRow CopyChildOutput(const PhysicalPlanNode &node,
                                           std::size_t child,
                                           const ExecutionRow &input);
void StoreEvaluatedValue(ExecutionRow *row, std::size_t offset, Value value,
                         RuntimeState &state);

[[nodiscard]] bool TryBindNode(ExecutionRow *row, std::size_t offset,
                               graphdb::Vertex vertex);
[[nodiscard]] bool TryBindNode(ExecutionRow *row,
                               std::optional<std::size_t> offset,
                               graphdb::Vertex vertex);
[[nodiscard]] bool TryBindOptionalEdge(ExecutionRow *row,
                                       std::optional<std::size_t> offset,
                                       graphdb::Edge edge);
[[nodiscard]] bool TryBindNode(ExecutionRow *row, std::size_t offset,
                               std::int64_t id, RuntimeState &state);
[[nodiscard]] bool TryBindNode(ExecutionRow *row,
                               std::optional<std::size_t> offset,
                               std::int64_t id, RuntimeState &state);
void SetScannedNode(ExecutionRow *row, std::size_t offset,
                    graphdb::Vertex vertex);
[[nodiscard]] graphdb::Vertex Endpoint(const graphdb::Edge &edge,
                                       std::int64_t id);
[[nodiscard]] bool MergeMappings(const ExecutionRow &source,
                                 ExecutionRow *target,
                                 const std::vector<RowMapping> &mappings);
[[nodiscard]] std::int64_t NodeId(const ExecutionRow &row, std::size_t offset);
[[nodiscard]] bool NodeHasAllLabels(const Node &node,
                                    const std::vector<std::string> &labels);
[[nodiscard]] bool RelationshipHasType(const Relationship &relationship,
                                       const std::vector<std::string> &types);

[[nodiscard]] Value BuildPathValue(const PathBuildOp &data,
                                   const ExecutionRow &row,
                                   RuntimeState *state);
using ProcedureRecord = Value::Map;
[[nodiscard]] std::vector<ProcedureRecord> ExecuteProcedure(
    const ProcedureCallOp &data, const ExecutionRow &row, RuntimeState *state);

void ExecuteStreamingWrite(const CreateNodeOp &data, const ExecutionRow &input,
                           ExecutionRow *output, RuntimeState *state);
void ExecuteStreamingWrite(const CreateRelationshipOp &data,
                           const ExecutionRow &input, ExecutionRow *output,
                           RuntimeState *state);
void ExecuteStreamingWrite(const SetPropertyOp &data, const ExecutionRow &input,
                           ExecutionRow *output, RuntimeState *state);
void ExecuteStreamingWrite(const SetPropertiesOp &data,
                           const ExecutionRow &input, ExecutionRow *output,
                           RuntimeState *state);
void ExecuteStreamingWrite(const SetLabelsOp &data, const ExecutionRow &input,
                           ExecutionRow *output, RuntimeState *state);
void ExecuteStreamingWrite(const RemovePropertyOp &data,
                           const ExecutionRow &input, ExecutionRow *output,
                           RuntimeState *state);
void ExecuteStreamingWrite(const RemoveLabelsOp &data,
                           const ExecutionRow &input, ExecutionRow *output,
                           RuntimeState *state);
void ExecuteMergeCreate(const CreateNodeOp &data, ExecutionRow *row,
                        RuntimeState *state);
void ExecuteMergeCreate(const CreateRelationshipOp &data, ExecutionRow *row,
                        RuntimeState *state);
void ExecuteMergeActions(const MergeOp &data, bool on_match, ExecutionRow *row,
                         RuntimeState *state);

[[nodiscard]] std::size_t EstimatedKeyHeapUsage(const CompositeValueKey &key);
[[nodiscard]] std::optional<CompositeValueKey> NodeJoinKey(
    const ExecutionRow &row, const std::vector<std::size_t> &layout);

[[nodiscard]] std::unique_ptr<PullOperator> BuildLeafOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::optional<ExecutionRow> argument);
[[nodiscard]] std::unique_ptr<PullOperator> BuildExpandOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::unique_ptr<PullOperator> source);
[[nodiscard]] std::unique_ptr<PullOperator> BuildUnaryOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::unique_ptr<PullOperator> source);
[[nodiscard]] std::unique_ptr<PullOperator> BuildBinaryOperator(
    const PhysicalPlanNode &node, RuntimeState &state,
    std::unique_ptr<PullOperator> lhs, std::unique_ptr<PullOperator> rhs);
[[nodiscard]] std::unique_ptr<PullOperator> BuildCorrelatedOperator(
    const PhysicalPlanNode &node, RuntimeState &state, OperatorFactory &factory,
    std::unique_ptr<PullOperator> lhs);

}  // namespace rg::execution
