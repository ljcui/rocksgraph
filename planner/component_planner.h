#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/query_ir.h"
#include "planner/logical_plan_builder.h"

namespace planner {

struct LeafPlanCandidate {
  std::unique_ptr<ir::LogicalPlan> plan;
  std::unordered_set<const ir::Predicate *> planned_predicates;
};

class QueryGraphPlanningContext {
 public:
  explicit QueryGraphPlanningContext(
      std::unordered_set<const ir::Predicate *> *planned_predicates,
      const LogicalPlanBuilderOptions *options);

  [[nodiscard]] std::vector<LeafPlanCandidate> BuildNodeLeafCandidates(
      const ir::QueryGraph &query_graph, std::string_view variable);
  [[nodiscard]] std::vector<LeafPlanCandidate> BuildRelationshipLeafCandidates(
      const ir::QueryGraph &query_graph, std::size_t relationship_index);
  [[nodiscard]] std::vector<std::string> ConsumeRelationshipTypes(
      const ir::Selections &selections,
      const ir::PatternRelationship &relationship);
  std::size_t ApplyAvailableFilters(const ir::Selections &selections,
                                    std::unique_ptr<ir::LogicalPlan> *plan);
  void ApplyNestedExpressions(
      const std::vector<ir::NestedIRExpression> &nested_expressions,
      std::unique_ptr<ir::LogicalPlan> *plan) const;
  void ValidateAllPredicatesPlanned(const ir::Selections &selections) const;

  [[nodiscard]] std::unordered_set<const ir::Predicate *> Snapshot() const;
  void Restore(std::unordered_set<const ir::Predicate *> planned_predicates);

 private:
  enum class IndexEntityKind {
    kNode,
    kRelationship,
  };

  struct IndexDescriptor {
    std::string property_key;
    bool unique = false;
    bool supports_equality = true;
    bool supports_range = true;
  };

  struct IndexedPredicate {
    const ir::Predicate *predicate = nullptr;
    IndexDescriptor index;
  };

  [[nodiscard]] std::vector<const ir::Predicate *>
  ConsumableNodeLabelPredicates(const ir::Selections &selections,
                                std::string_view variable) const;
  [[nodiscard]] std::vector<const ir::Predicate *>
  ConsumableRelationshipTypePredicates(const ir::Selections &selections,
                                       std::string_view variable) const;
  [[nodiscard]] std::optional<IndexDescriptor> FindIndex(
      IndexEntityKind entity_kind, const std::vector<std::string> &qualifiers,
      std::string_view property_key) const;
  [[nodiscard]] std::vector<IndexedPredicate> IndexSeekPredicates(
      const ir::Selections &selections, std::string_view variable,
      IndexEntityKind entity_kind,
      const std::vector<std::string> &qualifiers) const;
  [[nodiscard]] std::vector<std::vector<const ir::Predicate *>>
  IndexRangePredicateGroups(const ir::Selections &selections,
                            std::string_view variable,
                            IndexEntityKind entity_kind,
                            const std::vector<std::string> &qualifiers) const;
  void MarkPlanned(const std::vector<const ir::Predicate *> &predicates);
  [[nodiscard]] std::unique_ptr<ir::LogicalPlan> BuildNestedPlan(
      const ir::QueryIR &query) const;

  std::unordered_set<const ir::Predicate *> *planned_predicates_;
  const LogicalPlanBuilderOptions *options_ = nullptr;
};

class ComponentPlanner {
 public:
  ComponentPlanner() = default;
  ComponentPlanner(const ComponentPlanner &) = delete;
  ComponentPlanner &operator=(const ComponentPlanner &) = delete;
  virtual ~ComponentPlanner() = default;

  [[nodiscard]] virtual std::unique_ptr<ir::LogicalPlan> Plan(
      const ir::QueryGraph &query_graph,
      const ir::QueryGraphComponent &component,
      QueryGraphPlanningContext *context,
      const std::vector<ir::LogicalSortItem> &interesting_order) const = 0;
};

[[nodiscard]] std::unique_ptr<ComponentPlanner> MakeComponentPlanner(
    const LogicalPlanBuilderOptions &options);

}  // namespace planner
