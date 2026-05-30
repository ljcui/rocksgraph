#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ast/ast_node.h"

namespace ir {

using LogicalVariable = std::string;

enum class Direction { kIncoming, kOutgoing, kBoth };

struct PatternLength {
  bool variable = false;
  int fixed = 1;
  std::optional<int> min;
  std::optional<int> max;
};

struct PatternRelationship {
  std::string variable;
  std::string left_node;
  std::string right_node;
  Direction direction = Direction::kBoth;
  std::vector<std::string> types;
  PatternLength length;
};

enum class PredicateKind {
  kGenericExpression,
  kNodeLabel,
  kRelationshipType,
  kPropertyEquality,
  kPropertyComparison,
  kPropertyIn,
  kPropertyStringPredicate,
  kPropertyIsNull,
  kPropertyIsNotNull,
  kExistsSubquery,
  kNotExistsSubquery,
};

class PlannerQuery;

enum class NestedIRExpressionKind {
  kExists,
  kList,
};

struct NestedIRExpression {
  NestedIRExpressionKind kind = NestedIRExpressionKind::kExists;
  const ast::Expression *expression = nullptr;
  std::unordered_set<LogicalVariable> dependencies;
  std::unique_ptr<PlannerQuery> query;
  std::string value_variable;
  std::string collection_variable;
};

struct PredicateLabelInfo {
  std::string variable;
  std::vector<std::string> labels;
};

struct PredicateRelationshipTypeInfo {
  std::string variable;
  std::vector<std::string> relationship_types;
};

struct PredicatePropertyInfo {
  std::string variable;
  std::string property_key;
};

struct Predicate {
  const ast::Expression *expression = nullptr;
  std::unordered_set<std::string> dependencies;
  PredicateKind kind = PredicateKind::kGenericExpression;
  std::string variable;
  std::string property_key;
  const ast::Expression *property_value = nullptr;
  std::vector<std::string> labels;
  std::vector<std::string> relationship_types;
  std::string comparison_op;
  std::vector<PredicateLabelInfo> nested_node_labels;
  std::vector<PredicateRelationshipTypeInfo> nested_relationship_types;
  std::vector<PredicatePropertyInfo> nested_properties;
  std::vector<NestedIRExpression> nested_expressions;
  PlannerQuery *subquery = nullptr;
};

struct PropertyInequalityGroup {
  std::string variable;
  std::string property_key;
  std::vector<const Predicate *> lower_bounds;
  std::vector<const Predicate *> upper_bounds;
};

struct Selections {
  std::vector<Predicate> predicates;
  std::unordered_set<std::string> predicate_keys;

  [[nodiscard]] bool empty() const { return predicates.empty(); }
  [[nodiscard]] std::size_t size() const { return predicates.size(); }

  bool AddPredicate(Predicate predicate);

  [[nodiscard]] std::vector<const Predicate *> PredicatesByKind(
      PredicateKind kind) const;
  [[nodiscard]] std::vector<const Predicate *> PredicatesByVariable(
      std::string_view variable) const;
  [[nodiscard]] std::vector<const Predicate *> PredicatesDependingOn(
      std::string_view symbol) const;
  [[nodiscard]] std::vector<const Predicate *> PredicatesGiven(
      const std::unordered_set<std::string> &bound_symbols) const;

  [[nodiscard]] std::vector<const Predicate *> NodeLabelPredicates(
      std::string_view variable) const;
  [[nodiscard]] std::vector<const Predicate *> RelationshipTypePredicates(
      std::string_view variable) const;
  [[nodiscard]] std::vector<const Predicate *> PropertyPredicates(
      std::string_view variable, std::string_view property_key) const;
  [[nodiscard]] std::vector<const Predicate *> PropertyPredicates(
      std::string_view variable, std::string_view property_key,
      std::string_view comparison_op) const;

  [[nodiscard]] bool ContainsNodeLabel(std::string_view variable,
                                       std::string_view label) const;
  [[nodiscard]] bool ContainsRelationshipType(std::string_view variable,
                                              std::string_view type) const;
  [[nodiscard]] bool ContainsPropertyPredicate(
      std::string_view variable, std::string_view property_key) const;
  [[nodiscard]] bool ContainsPropertyPredicate(
      std::string_view variable, std::string_view property_key,
      std::string_view comparison_op) const;
  [[nodiscard]] std::vector<PropertyInequalityGroup> PropertyInequalityGroups()
      const;
};

struct Hint {};

enum class MutatingPatternKind {
  kCreate,
  kMerge,
  kSet,
  kDelete,
  kRemove,
};

struct PropertyMapEntry {
  std::string key;
  const ast::Expression *value = nullptr;
};

struct PatternPropertyMap {
  std::vector<PropertyMapEntry> entries;
  const ast::Parameter *parameter = nullptr;

  [[nodiscard]] bool empty() const {
    return entries.empty() && parameter == nullptr;
  }
};

struct CreateNodePattern {
  std::string variable;
  std::vector<std::string> labels;
  PatternPropertyMap properties;
  bool previously_bound = false;
};

struct CreateRelationshipPattern {
  std::string variable;
  std::string left_node;
  std::string right_node;
  Direction direction = Direction::kBoth;
  std::vector<std::string> types;
  PatternPropertyMap properties;
};

enum class CreateEntityKind {
  kNode,
  kRelationship,
};

struct CreateEntityCommand {
  CreateEntityKind kind = CreateEntityKind::kNode;
  std::size_t index = 0;
};

struct CreatePattern {
  std::unordered_set<LogicalVariable> path_variables;
  std::vector<CreateNodePattern> nodes;
  std::vector<CreateRelationshipPattern> relationships;
  std::vector<CreateEntityCommand> commands;
};

enum class SetMutatingPatternKind {
  kSetProperty,
  kSetExactPropertiesFromMap,
  kSetIncludingPropertiesFromMap,
  kSetLabels,
};

struct SetMutatingPattern {
  SetMutatingPatternKind kind = SetMutatingPatternKind::kSetProperty;
  const ast::Expression *entity = nullptr;
  std::string property_key;
  const ast::Expression *value = nullptr;
  std::vector<std::string> labels;
};

enum class RemoveMutatingPatternKind {
  kRemoveProperty,
  kRemoveLabels,
};

struct RemoveMutatingPattern {
  RemoveMutatingPatternKind kind = RemoveMutatingPatternKind::kRemoveProperty;
  const ast::Expression *entity = nullptr;
  std::string property_key;
  std::vector<std::string> labels;
};

struct DeleteExpressionPattern {
  const ast::Expression *expression = nullptr;
  bool detach = false;
};

struct PatternLabelPredicate {
  std::string variable;
  std::vector<std::string> labels;
  const ast::Expression *expression = nullptr;
};

struct PatternPropertyEquality {
  std::string variable;
  std::string property_key;
  const ast::Expression *value = nullptr;
  const ast::Expression *expression = nullptr;
};

struct MergeMatchGraph {
  std::unordered_set<LogicalVariable> pattern_nodes;
  std::vector<PatternRelationship> pattern_relationships;
  std::vector<PatternLabelPredicate> node_labels;
  std::vector<PatternPropertyEquality> property_equalities;
  std::unordered_set<LogicalVariable> argument_ids;
  std::vector<std::shared_ptr<ast::Expression>> predicate_expressions;
};

struct MergeActionPattern {
  bool on_match = false;
  std::vector<SetMutatingPattern> set_patterns;
};

struct MergePattern {
  CreatePattern create_pattern;
  MergeMatchGraph match_graph;
  std::vector<MergeActionPattern> actions;
};

struct MutatingPattern {
  MutatingPatternKind kind = MutatingPatternKind::kCreate;
  const ast::UpdatingClause *clause = nullptr;
  CreatePattern create;
  MergePattern merge;
  std::vector<SetMutatingPattern> set_patterns;
  std::vector<DeleteExpressionPattern> delete_patterns;
  std::vector<RemoveMutatingPattern> remove_patterns;
};

struct QueryGraphComponent {
  std::unordered_set<LogicalVariable> pattern_nodes;
  std::vector<std::size_t> pattern_relationship_indices;
  std::unordered_set<LogicalVariable> covered_ids;
  bool touches_arguments = false;
};

struct QueryGraph {
  std::unordered_set<LogicalVariable> pattern_paths;
  std::unordered_set<LogicalVariable> pattern_nodes;
  std::vector<PatternRelationship> pattern_relationships;
  std::unordered_set<LogicalVariable> argument_ids;
  std::unordered_set<LogicalVariable> assert_is_node_variables;
  Selections selections;
  std::vector<QueryGraph> optional_matches;
  std::vector<Hint> hints;
  std::vector<MutatingPattern> mutating_patterns;

  [[nodiscard]] bool HasLocalWork() const;
  [[nodiscard]] bool ContainsUpdates() const;
  [[nodiscard]] bool ReadOnly() const;
  [[nodiscard]] bool CouldContainRead() const;

  [[nodiscard]] std::unordered_set<LogicalVariable> CoveredIdsForPatterns()
      const;
  [[nodiscard]] std::unordered_set<LogicalVariable>
  IdsWithoutOptionalMatchesOrUpdates() const;
  [[nodiscard]] std::unordered_set<LogicalVariable> LocalAvailableSymbols()
      const;
  [[nodiscard]] std::unordered_set<LogicalVariable> AvailableSymbols() const;
  [[nodiscard]] std::unordered_set<LogicalVariable> AllCoveredIds() const;
  [[nodiscard]] std::unordered_set<LogicalVariable> Dependencies() const;

  [[nodiscard]] std::unordered_set<std::string> LabelsOnNode(
      std::string_view variable) const;
  [[nodiscard]] std::unordered_set<std::string> TypesOnRelationship(
      std::string_view variable) const;
  [[nodiscard]] std::unordered_set<std::string> KnownProperties(
      std::string_view variable) const;
  [[nodiscard]] std::unordered_set<std::string> AllPossibleLabelsOnNode(
      std::string_view variable) const;
  [[nodiscard]] std::unordered_set<std::string> AllPossibleTypesOnRelationship(
      std::string_view variable) const;
  [[nodiscard]] std::unordered_set<std::string> AllKnownPropertiesOnIdentifier(
      std::string_view variable) const;

  [[nodiscard]] std::vector<QueryGraphComponent> ConnectedComponents() const;
};

struct ProjectionItem {
  const ast::Expression *expression = nullptr;
  std::string alias;
};

enum class OrderDirection {
  kAscending,
  kDescending,
};

struct OrderItem {
  const ast::Expression *expression = nullptr;
  OrderDirection direction = OrderDirection::kAscending;
};

struct RequiredOrder {
  std::vector<OrderItem> items;

  [[nodiscard]] bool empty() const { return items.empty(); }
  [[nodiscard]] std::size_t size() const { return items.size(); }
};

struct OrderProjectionMapping {
  std::string projected_alias;
  const ast::Expression *source_expression = nullptr;
};

struct InterestingOrder {
  RequiredOrder required_order;
  std::vector<OrderItem> candidates;
  std::vector<OrderProjectionMapping> reverse_projection;

  [[nodiscard]] bool empty() const {
    return required_order.empty() && candidates.empty();
  }
};

struct Pagination {
  const ast::Expression *skip = nullptr;
  const ast::Expression *limit = nullptr;
};

enum class ProjectionPosition {
  kIntermediate,
  kFinal,
};

struct QueryProjection {
  RequiredOrder required_order;
  InterestingOrder interesting_order;
  Selections selections;
  Pagination pagination;
  ProjectionPosition position = ProjectionPosition::kIntermediate;
  std::vector<NestedIRExpression> nested_expressions;
};

struct RegularQueryProjection : QueryProjection {
  std::vector<ProjectionItem> items;
};

struct DistinctQueryProjection : QueryProjection {
  std::vector<ProjectionItem> grouping_items;
};

struct AggregatingQueryProjection : QueryProjection {
  std::vector<ProjectionItem> grouping_items;
  std::vector<ProjectionItem> aggregation_items;
};

struct UnwindHorizon {
  const ast::Expression *expression = nullptr;
  std::string alias;
};

struct ProcedureYieldItem {
  std::optional<std::string> result_field;
  std::string variable;
};

struct ProcedureCallHorizon {
  std::string procedure_name;
  std::vector<const ast::Expression *> arguments;
  std::vector<ProcedureYieldItem> yield_items;
  bool yield_star = false;
  Selections yield_selections;
  bool read_only = false;
};

enum class QueryHorizonKind {
  kRegularProjection,
  kDistinctProjection,
  kAggregatingProjection,
  kUnwind,
  kProcedureCall,
  kPassthrough,
};

struct QueryHorizon {
  QueryHorizonKind kind = QueryHorizonKind::kRegularProjection;
  RegularQueryProjection regular_projection;
  DistinctQueryProjection distinct_projection;
  AggregatingQueryProjection aggregating_projection;
  UnwindHorizon unwind;
  ProcedureCallHorizon procedure_call;

  static QueryHorizon ForRegularProjection(RegularQueryProjection projection);
  static QueryHorizon ForDistinctProjection(DistinctQueryProjection projection);
  static QueryHorizon ForAggregatingProjection(
      AggregatingQueryProjection projection);
  static QueryHorizon ForUnwind(UnwindHorizon unwind);
  static QueryHorizon ForProcedureCall(ProcedureCallHorizon procedure_call);
  static QueryHorizon ForPassthrough();

  [[nodiscard]] const RegularQueryProjection &RequireRegularProjection() const;
  RegularQueryProjection &RequireRegularProjection();

  [[nodiscard]] const DistinctQueryProjection &RequireDistinctProjection()
      const;
  DistinctQueryProjection &RequireDistinctProjection();

  [[nodiscard]] const AggregatingQueryProjection &RequireAggregatingProjection()
      const;
  AggregatingQueryProjection &RequireAggregatingProjection();

  [[nodiscard]] const UnwindHorizon &RequireUnwind() const;
  UnwindHorizon &RequireUnwind();

  [[nodiscard]] const ProcedureCallHorizon &RequireProcedureCall() const;
  ProcedureCallHorizon &RequireProcedureCall();
};

struct SinglePlannerQuery;
struct UnionPlannerQuery;

enum class PlannerQueryKind { kSingle, kUnion };

class PlannerQuery {
 public:
  PlannerQuery() = default;
  PlannerQuery(const PlannerQuery &) = delete;
  PlannerQuery &operator=(const PlannerQuery &) = delete;
  PlannerQuery(PlannerQuery &&) noexcept = default;
  PlannerQuery &operator=(PlannerQuery &&) noexcept = default;
  virtual ~PlannerQuery() = default;

  [[nodiscard]] virtual PlannerQueryKind Kind() const = 0;

  [[nodiscard]] const SinglePlannerQuery &RequireSingle() const;
  SinglePlannerQuery &RequireSingle();

  [[nodiscard]] const UnionPlannerQuery &RequireUnion() const;
  UnionPlannerQuery &RequireUnion();
};

struct SinglePlannerQuery final : public PlannerQuery {
  QueryGraph query_graph;
  QueryHorizon horizon;
  std::unique_ptr<SinglePlannerQuery> tail;

  SinglePlannerQuery() = default;

  [[nodiscard]] PlannerQueryKind Kind() const final {
    return PlannerQueryKind::kSingle;
  }

  [[nodiscard]] const SinglePlannerQuery *Last() const;
  SinglePlannerQuery *Last();
};

struct UnionPlannerQuery final : public PlannerQuery {
  std::unique_ptr<PlannerQuery> lhs;
  SinglePlannerQuery rhs;
  bool all = false;
  bool distinct = true;
  struct UnionMapping {
    std::string output_variable;
    std::string lhs_variable;
    std::string rhs_variable;
  };
  std::vector<UnionMapping> mappings;

  UnionPlannerQuery() = default;

  [[nodiscard]] PlannerQueryKind Kind() const final {
    return PlannerQueryKind::kUnion;
  }
};

std::unique_ptr<PlannerQuery> MakeSinglePlannerQuery(SinglePlannerQuery query);
std::unique_ptr<PlannerQuery> MakeUnionPlannerQuery(
    std::unique_ptr<PlannerQuery> lhs, SinglePlannerQuery rhs, bool all);

// CreatePlannerQuery expects a semantic AST after the default rewriter
// pipeline, normally produced by ast::ParseCypherAndRewrite. Raw parser output
// is rejected when it still contains unexpanded-star, missing-alias,
// anonymous-pattern, or pattern-predicate states that the planner IR does not
// consume directly.
std::unique_ptr<PlannerQuery> CreatePlannerQuery(
    const ast::Statement &statement);

}  // namespace ir
