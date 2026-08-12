#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "storage/access_path.h"

namespace rg {

class GraphAccessExecutor final {
 public:
  explicit GraphAccessExecutor(const AccessPath &access_path)
      : access_path_(&access_path) {}

  [[nodiscard]] QueryRows ExecuteAllNodeScan(const ir::AllNodeScanPlan &plan,
                                             const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteNodeByLabelScan(
      const ir::NodeByLabelScanPlan &plan, const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteNodeIndexSeek(
      const ir::NodeIndexSeekPlan &plan, const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteNodeIndexRangeSeek(
      const ir::NodeIndexRangeSeekPlan &plan, const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteRelationshipTypeScan(
      const ir::RelationshipTypeScanPlan &plan, const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteRelationshipIndexSeek(
      const ir::RelationshipIndexSeekPlan &plan, const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteRelationshipIndexRangeSeek(
      const ir::RelationshipIndexRangeSeekPlan &plan,
      const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteExpand(const ir::ExpandPlan &plan,
                                        const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteExpandInto(const ir::ExpandIntoPlan &plan,
                                            const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecuteVarExpand(const ir::VarExpandPlan &plan,
                                           const QueryRows &input) const;
  [[nodiscard]] QueryRows ExecutePathBuild(const ir::PathBuildPlan &plan,
                                           const QueryRows &input) const;

 private:
  void AddRelationshipRow(const QueryRow &row, const Relationship &relationship,
                          const std::string &from_node,
                          const std::string &relationship_variable,
                          const std::string &to_node,
                          ir::ExpandDirection direction, QueryRows *out) const;
  void AddDirectedRelationshipRow(const QueryRow &row,
                                  const Relationship &relationship,
                                  const std::string &from_node,
                                  const std::string &relationship_variable,
                                  const std::string &to_node,
                                  std::int64_t from_id, std::int64_t to_id,
                                  QueryRows *out) const;
  [[nodiscard]] std::vector<AccessPath::RelationshipPtr> ExpandCandidates(
      std::int64_t from_node_id, ir::ExpandDirection direction) const;
  [[nodiscard]] std::size_t VarExpandMinLength(
      const ir::LogicalVariableLength &length) const;
  [[nodiscard]] std::size_t VarExpandMaxLength(
      const ir::LogicalVariableLength &length) const;
  [[nodiscard]] std::optional<std::int64_t> NextVarExpandNode(
      const Relationship &relationship, std::int64_t current_node_id,
      ir::ExpandDirection direction) const;
  void ExpandVariableLengthPath(
      const ir::VarExpandPlan &plan, const QueryRow &row,
      std::int64_t current_node_id, std::optional<std::int64_t> bound_to_id,
      std::size_t min_length, std::size_t max_length,
      std::vector<AccessPath::RelationshipPtr> *path,
      std::unordered_set<std::int64_t> *used_relationships,
      QueryRows *out) const;
  void EmitVarExpandRow(const ir::VarExpandPlan &plan, const QueryRow &row,
                        std::int64_t current_node_id,
                        const std::vector<AccessPath::RelationshipPtr> &path,
                        QueryRows *out) const;
  [[nodiscard]] Value BuildPathValue(const ir::PathPattern &pattern,
                                     const QueryRow &row) const;
  [[nodiscard]] bool CanTraverseRelationshipSequence(
      const std::vector<AccessPath::RelationshipPtr> &relationships,
      std::int64_t start_node_id, std::int64_t target_node_id) const;
  void AppendPathRelationship(const Relationship &relationship, Path *path,
                              std::int64_t *current_node_id) const;

  const AccessPath *access_path_ = nullptr;
};

}  // namespace rg
