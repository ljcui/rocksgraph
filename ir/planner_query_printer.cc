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
    case PredicateKind::kExistsSubquery:
      return "exists_subquery";
  }
  THROW(common::InternalError, "unknown predicate kind");
}

std::string ExpressionText(const ast::Expression *expression) {
  if (expression == nullptr) {
    return "null";
  }
  return ast::ExpressionToString(*expression);
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

  void PrintQueryGraph(const QueryGraph &query_graph) {
    Line("query_graph:");
    Indent();
    Line("argument_ids: " + SortedList(query_graph.argument_ids));
    Line("pattern_nodes: " + SortedList(query_graph.pattern_nodes));
    PrintRelationships(query_graph.pattern_relationships);
    PrintSelections(query_graph.selections);
    Line("optional_matches: " +
         std::to_string(query_graph.optional_matches.size()));
    Line("hints: " + std::to_string(query_graph.hints.size()));
    Line("mutating_patterns: " +
         std::to_string(query_graph.mutating_patterns.size()));
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
      if (!predicate.labels.empty()) {
        Line("labels: " + List(predicate.labels));
      }
      if (!predicate.relationship_types.empty()) {
        Line("relationship_types: " + List(predicate.relationship_types));
      }
      if (!predicate.comparison_op.empty()) {
        Line("comparison_op: " + predicate.comparison_op);
      }
      Dedent();
    }
    Dedent();
  }

  void PrintHorizon(const QueryHorizon &horizon) {
    Line("horizon:");
    Indent();
    switch (horizon.kind) {
      case QueryHorizonKind::kProjection:
        PrintProjection(horizon.RequireProjection());
        Dedent();
        return;
      case QueryHorizonKind::kUnwind:
        PrintUnwind(horizon.RequireUnwind());
        Dedent();
        return;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  void PrintProjection(const Projection &projection) {
    Line("projection:");
    Indent();
    Line(std::string("distinct: ") + (projection.distinct ? "true" : "false"));
    Line("items:");
    Indent();
    if (projection.items.empty()) {
      Line("[]");
    } else {
      for (const auto &item : projection.items) {
        Line("- alias: " + item.alias);
        Indent();
        Line("expression: " + ExpressionText(item.expression));
        Dedent();
      }
    }
    Dedent();
    Line("order_by:");
    Indent();
    if (projection.order_by.empty()) {
      Line("[]");
    } else {
      for (const auto &item : projection.order_by) {
        Line("- expression: " + ExpressionText(item.expression));
        Indent();
        Line(std::string("ascending: ") + (item.ascending ? "true" : "false"));
        Dedent();
      }
    }
    Dedent();
    Line("where: " + ExpressionText(projection.where));
    Line("skip: " + ExpressionText(projection.skip));
    Line("limit: " + ExpressionText(projection.limit));
    Dedent();
  }

  void PrintUnwind(const UnwindHorizon &unwind) {
    Line("unwind:");
    Indent();
    Line("expression: " + ExpressionText(unwind.expression));
    Line("alias: " + unwind.alias);
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
