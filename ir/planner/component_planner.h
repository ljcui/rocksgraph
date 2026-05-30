#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/planner/logical_plan_builder.h"
#include "ir/planner_query.h"

namespace ir {

struct LeafPlanCandidate {
  std::unique_ptr<LogicalPlan> plan;
  std::unordered_set<const Predicate *> planned_predicates;
};

class QueryGraphPlanningContext {
 public:
  explicit QueryGraphPlanningContext(
      std::unordered_set<const Predicate *> *planned_predicates,
      const LogicalPlanBuilderOptions *options);

  [[nodiscard]] std::vector<LeafPlanCandidate> BuildNodeLeafCandidates(
      const QueryGraph &query_graph, std::string_view variable);
  [[nodiscard]] std::vector<LeafPlanCandidate> BuildRelationshipLeafCandidates(
      const QueryGraph &query_graph, std::size_t relationship_index);
  [[nodiscard]] std::vector<std::string> ConsumeRelationshipTypes(
      const Selections &selections, const PatternRelationship &relationship);
  std::size_t ApplyAvailableFilters(const Selections &selections,
                                    std::unique_ptr<LogicalPlan> *plan);
  void ApplyNestedExpressions(
      const std::vector<NestedIRExpression> &nested_expressions,
      std::unique_ptr<LogicalPlan> *plan) const;
  void ValidateAllPredicatesPlanned(const Selections &selections) const;

  [[nodiscard]] std::unordered_set<const Predicate *> Snapshot() const;
  void Restore(std::unordered_set<const Predicate *> planned_predicates);

 private:
  enum class IndexEntityKind {
    kNode,
    kRelationship,
  };

  [[nodiscard]] std::vector<const Predicate *> ConsumableNodeLabelPredicates(
      const Selections &selections, std::string_view variable) const;
  [[nodiscard]] std::vector<const Predicate *>
  ConsumableRelationshipTypePredicates(const Selections &selections,
                                       std::string_view variable) const;
  [[nodiscard]] bool HasIndex(IndexEntityKind entity_kind,
                              const std::vector<std::string> &qualifiers,
                              std::string_view property_key) const;
  [[nodiscard]] std::vector<const Predicate *> IndexSeekPredicates(
      const Selections &selections, std::string_view variable,
      IndexEntityKind entity_kind,
      const std::vector<std::string> &qualifiers) const;
  [[nodiscard]] std::vector<std::vector<const Predicate *>>
  IndexRangePredicateGroups(const Selections &selections,
                            std::string_view variable,
                            IndexEntityKind entity_kind,
                            const std::vector<std::string> &qualifiers) const;
  void MarkPlanned(const std::vector<const Predicate *> &predicates);
  [[nodiscard]] std::unique_ptr<LogicalPlan> BuildNestedPlan(
      const PlannerQuery &query) const;

  std::unordered_set<const Predicate *> *planned_predicates_;
  const LogicalPlanBuilderOptions *options_ = nullptr;
};

class ComponentPlanner {
 public:
  ComponentPlanner() = default;
  ComponentPlanner(const ComponentPlanner &) = delete;
  ComponentPlanner &operator=(const ComponentPlanner &) = delete;
  virtual ~ComponentPlanner() = default;

  [[nodiscard]] virtual std::unique_ptr<LogicalPlan> Plan(
      const QueryGraph &query_graph, const QueryGraphComponent &component,
      QueryGraphPlanningContext *context) const = 0;
};

[[nodiscard]] std::unique_ptr<ComponentPlanner> MakeComponentPlanner(
    const LogicalPlanBuilderOptions &options);

}  // namespace ir
