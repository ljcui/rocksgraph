#include "ir/planner_query_printer.h"

#include <algorithm>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/expression_to_string.h"
#include "common/exception.h"

namespace ir {
namespace {

std::vector<std::string> Sorted(const std::unordered_set<std::string> &values) {
  std::vector<std::string> result(values.begin(), values.end());
  std::sort(result.begin(), result.end());
  return result;
}

std::string Join(const std::vector<std::string> &values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += values[i];
  }
  return out;
}

std::string List(const std::vector<std::string> &values) {
  return "[" + Join(values) + "]";
}

std::string SortedList(const std::unordered_set<std::string> &values) {
  return List(Sorted(values));
}

std::string DirectionToString(Direction direction) {
  switch (direction) {
    case Direction::kIncoming:
      return "incoming";
    case Direction::kOutgoing:
      return "outgoing";
    case Direction::kBoth:
      return "both";
  }
  THROW(common::InternalError, "unknown pattern relationship direction");
}

std::string OrderDirectionToString(OrderDirection direction) {
  switch (direction) {
    case OrderDirection::kAscending:
      return "ascending";
    case OrderDirection::kDescending:
      return "descending";
  }
  THROW(common::InternalError, "unknown order direction");
}

std::string PredicateKindToString(PredicateKind kind) {
  switch (kind) {
    case PredicateKind::kGenericExpression:
      return "generic_expression";
    case PredicateKind::kNodeLabel:
      return "node_label";
    case PredicateKind::kRelationshipType:
      return "relationship_type";
    case PredicateKind::kPropertyEquality:
      return "property_equality";
    case PredicateKind::kPropertyComparison:
      return "property_comparison";
    case PredicateKind::kPropertyIn:
      return "property_in";
    case PredicateKind::kPropertyStringPredicate:
      return "property_string_predicate";
    case PredicateKind::kPropertyIsNull:
      return "property_is_null";
    case PredicateKind::kPropertyIsNotNull:
      return "property_is_not_null";
    case PredicateKind::kExistsSubquery:
      return "exists_subquery";
    case PredicateKind::kNotExistsSubquery:
      return "not_exists_subquery";
  }
  THROW(common::InternalError, "unknown predicate kind");
}

std::string NestedIRExpressionKindToString(NestedIRExpressionKind kind) {
  switch (kind) {
    case NestedIRExpressionKind::kExists:
      return "exists";
    case NestedIRExpressionKind::kList:
      return "list";
  }
  THROW(common::InternalError, "unknown nested IR expression kind");
}

std::string MutatingPatternKindToString(MutatingPatternKind kind) {
  switch (kind) {
    case MutatingPatternKind::kCreate:
      return "create";
    case MutatingPatternKind::kMerge:
      return "merge";
    case MutatingPatternKind::kSet:
      return "set";
    case MutatingPatternKind::kDelete:
      return "delete";
    case MutatingPatternKind::kRemove:
      return "remove";
  }
  THROW(common::InternalError, "unknown mutating pattern kind");
}

std::string ExpressionText(const ast::Expression *expression) {
  if (expression == nullptr) {
    return "null";
  }
  return ast::ExpressionToString(*expression);
}

std::string UpdatingClauseText(const ast::UpdatingClause *clause) {
  if (clause == nullptr) {
    return "null";
  }
  const std::string text = ast::UpdatingClauseToString(*clause);
  return text.empty() ? "<unprintable>" : text;
}

std::string PatternLengthText(const PatternLength &length) {
  if (!length.variable) {
    return "fixed(" + std::to_string(length.fixed) + ")";
  }
  std::string out = "variable(";
  if (length.min) {
    out += std::to_string(*length.min);
  }
  out += "..";
  if (length.max) {
    out += std::to_string(*length.max);
  }
  out += ")";
  return out;
}

class PlannerQueryPrinter {
 public:
  explicit PlannerQueryPrinter(std::ostream &out) : out_(out) {}

  void Print(const PlannerQuery &query) { PrintPlannerQueryNode(query); }

 private:
  void Line(const std::string &text) {
    for (int i = 0; i < indent_; ++i) {
      out_ << "  ";
    }
    out_ << text << '\n';
  }

  void Indent() { ++indent_; }
  void Dedent() {
    CHECK(indent_ > 0, common::InternalError,
          "planner query printer indent underflow");
    --indent_;
  }

  void PrintPlannerQueryNode(const PlannerQuery &query) {
    switch (query.Kind()) {
      case PlannerQueryKind::kSingle:
        PrintSingle(query.RequireSingle());
        return;
      case PlannerQueryKind::kUnion:
        PrintUnion(query.RequireUnion());
        return;
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  void PrintSingle(const SinglePlannerQuery &query) {
    Line("SinglePlannerQuery");
    Indent();
    PrintQueryGraph(query.query_graph);
    PrintHorizon(query.horizon);
    Line("tail:");
    Indent();
    if (query.tail == nullptr) {
      Line("null");
    } else {
      PrintSingle(*query.tail);
    }
    Dedent();
    Dedent();
  }

  void PrintUnion(const UnionPlannerQuery &query) {
    Line("UnionPlannerQuery");
    Indent();
    Line(std::string("all: ") + (query.all ? "true" : "false"));
    Line(std::string("distinct: ") + (query.distinct ? "true" : "false"));
    PrintUnionMappings(query.mappings);
    Line("lhs:");
    Indent();
    CHECK(query.lhs != nullptr, common::InvalidArgumentError,
          "UNION lhs planner query is null");
    PrintPlannerQueryNode(*query.lhs);
    Dedent();
    Line("rhs:");
    Indent();
    PrintSingle(query.rhs);
    Dedent();
    Dedent();
  }

  void PrintUnionMappings(
      const std::vector<UnionPlannerQuery::UnionMapping> &mappings) {
    Line("mappings:");
    Indent();
    if (mappings.empty()) {
      Line("[]");
    } else {
      for (const auto &mapping : mappings) {
        Line("- output: " + mapping.output_variable);
        Indent();
        Line("lhs: " + mapping.lhs_variable);
        Line("rhs: " + mapping.rhs_variable);
        Dedent();
      }
    }
    Dedent();
  }

  void PrintQueryGraph(const QueryGraph &query_graph) {
    Line("query_graph:");
    Indent();
    Line("argument_ids: " + SortedList(query_graph.argument_ids));
    if (!query_graph.assert_is_node_variables.empty()) {
      Line("assert_is_node_variables: " +
           SortedList(query_graph.assert_is_node_variables));
    }
    if (!query_graph.pattern_paths.empty()) {
      Line("pattern_paths: " + SortedList(query_graph.pattern_paths));
    }
    Line("pattern_nodes: " + SortedList(query_graph.pattern_nodes));
    PrintRelationships(query_graph.pattern_relationships);
    PrintSelections(query_graph.selections);
    Line("optional_matches: " +
         std::to_string(query_graph.optional_matches.size()));
    Line("hints: " + std::to_string(query_graph.hints.size()));
    PrintMutatingPatterns(query_graph.mutating_patterns);
    Dedent();
  }

  void PrintMutatingPatterns(
      const std::vector<MutatingPattern> &mutating_patterns) {
    if (mutating_patterns.empty()) {
      Line("mutating_patterns: 0");
      return;
    }
    Line("mutating_patterns:");
    Indent();
    for (const auto &mutating_pattern : mutating_patterns) {
      Line("- kind: " + MutatingPatternKindToString(mutating_pattern.kind));
      Indent();
      Line("clause: " + UpdatingClauseText(mutating_pattern.clause));
      switch (mutating_pattern.kind) {
        case MutatingPatternKind::kMerge:
          Line("merge_actions: " +
               std::to_string(mutating_pattern.merge.actions.size()));
          Line("match_nodes: " +
               std::to_string(
                   mutating_pattern.merge.match_graph.pattern_nodes.size()));
          Line("match_relationships: " +
               std::to_string(mutating_pattern.merge.match_graph
                                  .pattern_relationships.size()));
          break;
        case MutatingPatternKind::kDelete: {
          const bool detach = !mutating_pattern.delete_patterns.empty() &&
                              mutating_pattern.delete_patterns.front().detach;
          Line(std::string("detach: ") + (detach ? "true" : "false"));
          Line("expressions: " +
               std::to_string(mutating_pattern.delete_patterns.size()));
          break;
        }
        case MutatingPatternKind::kSet:
          Line("items: " +
               std::to_string(mutating_pattern.set_patterns.size()));
          break;
        case MutatingPatternKind::kRemove:
          Line("items: " +
               std::to_string(mutating_pattern.remove_patterns.size()));
          break;
        case MutatingPatternKind::kCreate:
          Line("nodes: " +
               std::to_string(mutating_pattern.create.nodes.size()));
          Line("relationships: " +
               std::to_string(mutating_pattern.create.relationships.size()));
          break;
      }
      Dedent();
    }
    Dedent();
  }

  void PrintRelationships(
      const std::vector<PatternRelationship> &relationships) {
    Line("pattern_relationships:");
    Indent();
    if (relationships.empty()) {
      Line("[]");
      Dedent();
      return;
    }
    for (const auto &relationship : relationships) {
      Line("- variable: " + relationship.variable);
      Indent();
      Line("left_node: " + relationship.left_node);
      Line("right_node: " + relationship.right_node);
      Line("direction: " + DirectionToString(relationship.direction));
      Line("types: " + List(relationship.types));
      Line("length: " + PatternLengthText(relationship.length));
      Dedent();
    }
    Dedent();
  }

  void PrintSelections(const Selections &selections) {
    Line("selections:");
    Indent();
    if (selections.predicates.empty()) {
      Line("[]");
      Dedent();
      return;
    }
    for (const auto &predicate : selections.predicates) {
      Line("- kind: " + PredicateKindToString(predicate.kind));
      Indent();
      Line("expression: " + ExpressionText(predicate.expression));
      Line("dependencies: " + SortedList(predicate.dependencies));
      if (!predicate.variable.empty()) {
        Line("variable: " + predicate.variable);
      }
      if (!predicate.property_key.empty()) {
        Line("property_key: " + predicate.property_key);
      }
      if (predicate.property_value != nullptr) {
        Line("property_value: " + ExpressionText(predicate.property_value));
      }
      if (!predicate.labels.empty()) {
        Line("labels: " + List(predicate.labels));
      }
      if (!predicate.relationship_types.empty()) {
        Line("relationship_types: " + List(predicate.relationship_types));
      }
      if (!predicate.comparison_op.empty()) {
        Line("comparison_op: " + predicate.comparison_op);
      }
      if (predicate.subquery != nullptr) {
        Line("subquery:");
        Indent();
        PrintPlannerQueryNode(*predicate.subquery);
        Dedent();
      }
      PrintNestedIRExpressions(predicate.nested_expressions);
      Dedent();
    }
    Dedent();
  }

  void PrintNestedIRExpressions(
      const std::vector<NestedIRExpression> &nested_expressions) {
    if (nested_expressions.empty()) {
      return;
    }
    Line("nested_expressions:");
    Indent();
    for (const auto &nested : nested_expressions) {
      Line("- kind: " + NestedIRExpressionKindToString(nested.kind));
      Indent();
      Line("expression: " + ExpressionText(nested.expression));
      Line("dependencies: " + SortedList(nested.dependencies));
      if (!nested.value_variable.empty()) {
        Line("value_variable: " + nested.value_variable);
      }
      if (!nested.collection_variable.empty()) {
        Line("collection_variable: " + nested.collection_variable);
      }
      if (nested.query != nullptr) {
        Line("query:");
        Indent();
        PrintPlannerQueryNode(*nested.query);
        Dedent();
      }
      Dedent();
    }
    Dedent();
  }

  void PrintHorizon(const QueryHorizon &horizon) {
    Line("horizon:");
    Indent();
    switch (horizon.kind) {
      case QueryHorizonKind::kRegularProjection:
        PrintRegularProjection(horizon.RequireRegularProjection());
        Dedent();
        return;
      case QueryHorizonKind::kDistinctProjection:
        PrintDistinctProjection(horizon.RequireDistinctProjection());
        Dedent();
        return;
      case QueryHorizonKind::kAggregatingProjection:
        PrintAggregatingProjection(horizon.RequireAggregatingProjection());
        Dedent();
        return;
      case QueryHorizonKind::kUnwind:
        PrintUnwind(horizon.RequireUnwind());
        Dedent();
        return;
      case QueryHorizonKind::kProcedureCall:
        PrintProcedureCall(horizon.RequireProcedureCall());
        Dedent();
        return;
      case QueryHorizonKind::kPassthrough:
        Line("passthrough");
        Dedent();
        return;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  void PrintRegularProjection(const RegularQueryProjection &projection) {
    Line("regular_projection:");
    Indent();
    PrintProjectionItems("items", projection.items);
    PrintProjectionTail(projection);
    Dedent();
  }

  void PrintDistinctProjection(const DistinctQueryProjection &projection) {
    Line("distinct_projection:");
    Indent();
    PrintProjectionItems("grouping_items", projection.grouping_items);
    PrintProjectionTail(projection);
    Dedent();
  }

  void PrintAggregatingProjection(
      const AggregatingQueryProjection &projection) {
    Line("aggregating_projection:");
    Indent();
    PrintProjectionItems("grouping_items", projection.grouping_items);
    PrintProjectionItems("aggregation_items", projection.aggregation_items);
    PrintProjectionTail(projection);
    Dedent();
  }

  void PrintProjectionItems(const std::string &name,
                            const std::vector<ProjectionItem> &items) {
    Line(name + ":");
    Indent();
    if (items.empty()) {
      Line("[]");
    } else {
      for (const auto &item : items) {
        Line("- alias: " + item.alias);
        Indent();
        Line("expression: " + ExpressionText(item.expression));
        Dedent();
      }
    }
    Dedent();
  }

  void PrintProjectionTail(const QueryProjection &projection) {
    PrintSelections(projection.selections);
    PrintNestedIRExpressions(projection.nested_expressions);
    Line("required_order:");
    Indent();
    if (projection.required_order.empty()) {
      Line("[]");
    } else {
      for (const auto &item : projection.required_order.items) {
        Line("- expression: " + ExpressionText(item.expression));
        Indent();
        Line("direction: " + OrderDirectionToString(item.direction));
        Dedent();
      }
    }
    Dedent();
    Line("pagination:");
    Indent();
    Line("skip: " + ExpressionText(projection.pagination.skip));
    Line("limit: " + ExpressionText(projection.pagination.limit));
    Dedent();
  }

  void PrintUnwind(const UnwindHorizon &unwind) {
    Line("unwind:");
    Indent();
    Line("expression: " + ExpressionText(unwind.expression));
    Line("alias: " + unwind.alias);
    Dedent();
  }

  void PrintProcedureCall(const ProcedureCallHorizon &procedure_call) {
    Line("procedure_call:");
    Indent();
    Line("procedure_name: " + procedure_call.procedure_name);
    Line(std::string("read_only: ") +
         (procedure_call.read_only ? "true" : "false"));
    Line(std::string("yield_star: ") +
         (procedure_call.yield_star ? "true" : "false"));
    Line("arguments:");
    Indent();
    if (procedure_call.arguments.empty()) {
      Line("[]");
    } else {
      for (const ast::Expression *argument : procedure_call.arguments) {
        Line("- " + ExpressionText(argument));
      }
    }
    Dedent();
    Line("yield_items:");
    Indent();
    if (procedure_call.yield_items.empty()) {
      Line("[]");
    } else {
      for (const auto &item : procedure_call.yield_items) {
        Line("- variable: " + item.variable);
        if (item.result_field.has_value()) {
          Indent();
          Line("result_field: " + *item.result_field);
          Dedent();
        }
      }
    }
    Dedent();
    PrintSelections(procedure_call.yield_selections);
    Dedent();
  }

  std::ostream &out_;
  int indent_ = 0;
};

}  // namespace

void PrintPlannerQuery(const PlannerQuery &query, std::ostream &out) {
  PlannerQueryPrinter printer(out);
  printer.Print(query);
}

std::string PlannerQueryToString(const PlannerQuery &query) {
  std::ostringstream out;
  PrintPlannerQuery(query, out);
  return out.str();
}

}  // namespace ir
