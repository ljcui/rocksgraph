#include "ast_printer.h"

#include <sstream>

namespace ast {

ASTPrinter::ASTPrinter(std::ostream &out) : out_(out) {}

void ASTPrinter::Print(ASTNode &node) { node.Accept(*this); }

void ASTPrinter::Line(const std::string &text) {
  for (int i = 0; i < indent_; ++i) {
    out_ << "  ";
  }
  out_ << text << "\n";
}

void ASTPrinter::LineNodeType(const ASTNode &node) {
  Line(std::string(ToString(node.node_type)));
}

void ASTPrinter::Indent() { ++indent_; }

void ASTPrinter::Dedent() {
  if (indent_ > 0) {
    --indent_;
  }
}

void ASTPrinter::PrintYieldItems(
    const std::vector<StandaloneCall::YieldItem> &yield_items) {
  for (const auto &item : yield_items) {
    std::ostringstream item_line;
    item_line << "YieldItem ";
    if (item.result_field) {
      item_line << *item.result_field << " AS ";
    }
    item_line << item.variable;
    Line(item_line.str());
  }
}

void ASTPrinter::Visit(Statement &node) { LineNodeType(node); }

void ASTPrinter::Visit(Query &node) { LineNodeType(node); }

void ASTPrinter::Visit(RegularQuery &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.single_query);
  VisitList(node.unions);
}

void ASTPrinter::Visit(StandaloneCall &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " procedure=" << node.procedure_name;
  if (node.yield_star) {
    oss << " yield=*";
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitList(node.arguments);
  PrintYieldItems(node.yield_items);
  VisitMaybe(node.yield_where);
}

void ASTPrinter::Visit(SingleQuery &node) { LineNodeType(node); }

void ASTPrinter::Visit(SinglePartQuery &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitList(node.reading_clauses);
  VisitList(node.updating_clauses);
  VisitMaybe(node.return_clause);
}

void ASTPrinter::Visit(MultiPartQuery &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  for (const auto &part : node.parts) {
    Line("WithPart");
    IndentGuard part_guard(*this);
    VisitList(part.reading_clauses);
    VisitList(part.updating_clauses);
    VisitMaybe(part.with_clause);
  }
  VisitMaybe(node.final_single_part_query);
}

void ASTPrinter::Visit(UnionPart &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " all=" << (node.all ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.query);
}

void ASTPrinter::Visit(Expression &node) { LineNodeType(node); }

void ASTPrinter::Visit(BinaryExpression &node) {
  Line("BinaryExpression");
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(OrExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(XorExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(AndExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(ComparisonExpression &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " op=" << node.op;
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(ComparisonChainExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  for (const auto &entry : node.rights) {
    std::ostringstream oss;
    oss << "Op " << entry.first;
    Line(oss.str());
    IndentGuard op_guard(*this);
    VisitMaybe(entry.second);
  }
}

void ASTPrinter::Visit(AddExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(SubtractExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(MultiplyExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(DivideExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(ModuloExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(PowerExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(UnaryExpression &node) {
  Line("UnaryExpression");
  IndentGuard guard(*this);
  VisitMaybe(node.operand);
}

void ASTPrinter::Visit(NotExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.operand);
}

void ASTPrinter::Visit(UnaryPlusExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.operand);
}

void ASTPrinter::Visit(UnaryMinusExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.operand);
}

void ASTPrinter::Visit(StringPredicateExpression &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " op=" << node.op;
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.left);
  VisitMaybe(node.right);
}

void ASTPrinter::Visit(ListPredicateExpression &node) {
  Line("ListPredicateExpression IN");
  IndentGuard guard(*this);
  VisitMaybe(node.element);
  VisitMaybe(node.list);
}

void ASTPrinter::Visit(LabelPredicateExpression &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " labels=";
  for (size_t i = 0; i < node.labels.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << node.labels[i];
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.expr);
}

void ASTPrinter::Visit(NullPredicateExpression &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " is_null=" << (node.is_null ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.operand);
}

void ASTPrinter::Visit(Literal &node) { LineNodeType(node); }

void ASTPrinter::Visit(BooleanLiteral &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " value=" << (node.value ? "true" : "false");
  Line(oss.str());
}

void ASTPrinter::Visit(IntegerLiteral &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " value=" << node.value;
  Line(oss.str());
}

void ASTPrinter::Visit(DoubleLiteral &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " value=" << node.value;
  Line(oss.str());
}

void ASTPrinter::Visit(StringLiteral &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " value=\"" << node.value << "\"";
  Line(oss.str());
}

void ASTPrinter::Visit(NullLiteral &node) { LineNodeType(node); }

void ASTPrinter::Visit(ListLiteral &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitList(node.elements);
}

void ASTPrinter::Visit(MapLiteral &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  for (const auto &entry : node.entries) {
    std::ostringstream oss;
    oss << "Entry key=" << entry.first;
    Line(oss.str());
    IndentGuard entry_guard(*this);
    VisitMaybe(entry.second);
  }
}

void ASTPrinter::Visit(Properties &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.map);
  VisitMaybe(node.parameter);
}

void ASTPrinter::Visit(Variable &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " name=" << node.name;
  Line(oss.str());
}

void ASTPrinter::Visit(Parameter &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " name=" << node.name;
  Line(oss.str());
}

void ASTPrinter::Visit(PropertyExpression &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " key=" << node.property_key;
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.object);
}

void ASTPrinter::Visit(ListIndexExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.list);
  VisitMaybe(node.index);
}

void ASTPrinter::Visit(ListSliceExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.list);
  if (node.start_index) {
    Line("StartIndex");
    IndentGuard start_guard(*this);
    VisitMaybe(node.start_index);
  }
  if (node.end_index) {
    Line("EndIndex");
    IndentGuard end_guard(*this);
    VisitMaybe(node.end_index);
  }
}

void ASTPrinter::Visit(FunctionInvocation &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " name=" << node.function_name
      << " distinct=" << (node.distinct ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitList(node.arguments);
}

void ASTPrinter::Visit(CountStarExpression &node) { LineNodeType(node); }

void ASTPrinter::Visit(CaseExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  if (node.test) {
    Line("Test");
    IndentGuard test_guard(*this);
    VisitMaybe(node.test);
  }
  for (const auto &alt : node.alternatives) {
    Line("Alternative");
    IndentGuard alt_guard(*this);
    Line("When");
    {
      IndentGuard when_guard(*this);
      VisitMaybe(alt.first);
    }
    Line("Then");
    {
      IndentGuard then_guard(*this);
      VisitMaybe(alt.second);
    }
  }
  if (node.else_expr) {
    Line("Else");
    IndentGuard else_guard(*this);
    VisitMaybe(node.else_expr);
  }
}

void ASTPrinter::Visit(ParenthesizedExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.expr);
}

void ASTPrinter::Visit(ListComprehension &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " variable=" << node.variable;
  Line(oss.str());
  IndentGuard guard(*this);
  Line("List");
  {
    IndentGuard list_guard(*this);
    VisitMaybe(node.list_expr);
  }
  if (node.where_expr) {
    Line("Where");
    IndentGuard where_guard(*this);
    VisitMaybe(node.where_expr);
  }
  if (node.eval_expr) {
    Line("Eval");
    IndentGuard eval_guard(*this);
    VisitMaybe(node.eval_expr);
  }
}

void ASTPrinter::Visit(PatternComprehension &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " variable=" << node.variable;
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.relationships_pattern);
  if (node.where_expr) {
    Line("Where");
    IndentGuard where_guard(*this);
    VisitMaybe(node.where_expr);
  }
  Line("Eval");
  {
    IndentGuard eval_guard(*this);
    VisitMaybe(node.eval_expr);
  }
}

void ASTPrinter::Visit(PatternPredicateExpression &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.relationships_pattern);
}

void ASTPrinter::Visit(Quantifier &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " variable=" << node.variable;
  Line(oss.str());
  IndentGuard guard(*this);
  Line("List");
  {
    IndentGuard list_guard(*this);
    VisitMaybe(node.list_expr);
  }
  if (node.predicate) {
    Line("Predicate");
    IndentGuard predicate_guard(*this);
    VisitMaybe(node.predicate);
  }
}

void ASTPrinter::Visit(AllQuantifier &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  Quantifier &base = node;
  Visit(base);
}

void ASTPrinter::Visit(AnyQuantifier &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  Quantifier &base = node;
  Visit(base);
}

void ASTPrinter::Visit(NoneQuantifier &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  Quantifier &base = node;
  Visit(base);
}

void ASTPrinter::Visit(SingleQuantifier &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  Quantifier &base = node;
  Visit(base);
}

void ASTPrinter::Visit(ExistentialSubquery &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.query);
  VisitMaybe(node.pattern);
  VisitMaybe(node.where_expr);
}

void ASTPrinter::Visit(Pattern &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitList(node.parts);
}

void ASTPrinter::Visit(PatternPart &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type);
  if (!node.variable.empty()) {
    oss << " variable=" << node.variable;
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.element);
}

void ASTPrinter::Visit(PatternElement &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.node_pattern);
  for (const auto &entry : node.chain) {
    Line("Chain");
    IndentGuard chain_guard(*this);
    VisitMaybe(entry.first);
    VisitMaybe(entry.second);
  }
}

void ASTPrinter::Visit(RelationshipsPattern &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.node_pattern);
  for (const auto &entry : node.chain) {
    Line("Chain");
    IndentGuard chain_guard(*this);
    VisitMaybe(entry.first);
    VisitMaybe(entry.second);
  }
}

void ASTPrinter::Visit(NodePattern &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type);
  if (!node.variable.empty()) {
    oss << " variable=" << node.variable;
  }
  if (!node.labels.empty()) {
    oss << " labels=";
    for (size_t i = 0; i < node.labels.size(); ++i) {
      if (i > 0) {
        oss << ",";
      }
      oss << node.labels[i];
    }
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.properties);
}

void ASTPrinter::Visit(RelationshipPattern &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " left=" << (node.left_arrow ? "true" : "false")
      << " right=" << (node.right_arrow ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.detail);
}

void ASTPrinter::Visit(RelationshipDetail &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type);
  if (!node.variable.empty()) {
    oss << " variable=" << node.variable;
  }
  if (!node.types.empty()) {
    oss << " types=";
    for (size_t i = 0; i < node.types.size(); ++i) {
      if (i > 0) {
        oss << "|";
      }
      oss << node.types[i];
    }
  }
  if (node.range) {
    oss << " range=";
    if (node.range->min) {
      oss << *node.range->min;
    }
    oss << "..";
    if (node.range->max) {
      oss << *node.range->max;
    }
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.properties);
}

void ASTPrinter::Visit(Clause &node) { LineNodeType(node); }

void ASTPrinter::Visit(ReadingClause &node) { LineNodeType(node); }

void ASTPrinter::Visit(Match &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " optional=" << (node.optional_match ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.pattern);
  VisitMaybe(node.where);
}

void ASTPrinter::Visit(Unwind &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " variable=" << node.variable;
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.expression);
}

void ASTPrinter::Visit(InQueryCall &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " procedure=" << node.procedure_name;
  Line(oss.str());
  IndentGuard guard(*this);
  VisitList(node.arguments);
  PrintYieldItems(node.yield_items);
  VisitMaybe(node.yield_where);
}

void ASTPrinter::Visit(UpdatingClause &node) { LineNodeType(node); }

void ASTPrinter::Visit(Create &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.pattern);
}

void ASTPrinter::Visit(Merge &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.pattern_part);
  for (const auto &action : node.actions) {
    std::ostringstream oss;
    oss << (action.first ? "OnMatch" : "OnCreate");
    Line(oss.str());
    IndentGuard action_guard(*this);
    VisitMaybe(action.second);
  }
}

void ASTPrinter::Visit(Delete &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " detach=" << (node.detach ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitList(node.expressions);
}

void ASTPrinter::Visit(Set &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitList(node.items);
}

void ASTPrinter::Visit(SetItem &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " type=" << node.type;
  if (node.plus_equal) {
    oss << " plus_equal=true";
  }
  if (!node.labels.empty()) {
    oss << " labels=";
    for (size_t i = 0; i < node.labels.size(); ++i) {
      if (i > 0) {
        oss << ",";
      }
      oss << node.labels[i];
    }
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.target);
  VisitMaybe(node.value);
}

void ASTPrinter::Visit(Remove &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitList(node.items);
}

void ASTPrinter::Visit(RemoveItem &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type) << " type=" << node.type;
  if (!node.labels.empty()) {
    oss << " labels=";
    for (size_t i = 0; i < node.labels.size(); ++i) {
      if (i > 0) {
        oss << ",";
      }
      oss << node.labels[i];
    }
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.target);
}

void ASTPrinter::Visit(ProjectionClause &node) { LineNodeType(node); }

void ASTPrinter::Visit(ProjectionBody &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " distinct=" << (node.distinct ? "true" : "false")
      << " star=" << (node.star ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitList(node.items);
  VisitList(node.order_by);
  VisitMaybe(node.skip);
  VisitMaybe(node.limit);
}

void ASTPrinter::Visit(ProjectionItem &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type);
  if (!node.alias.empty()) {
    oss << " alias=" << node.alias;
  }
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.expression);
}

void ASTPrinter::Visit(SortItem &node) {
  std::ostringstream oss;
  oss << ToString(node.node_type)
      << " ascending=" << (node.ascending ? "true" : "false");
  Line(oss.str());
  IndentGuard guard(*this);
  VisitMaybe(node.expression);
}

void ASTPrinter::Visit(With &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.body);
  VisitMaybe(node.where);
}

void ASTPrinter::Visit(Return &node) {
  LineNodeType(node);
  IndentGuard guard(*this);
  VisitMaybe(node.body);
}

}  // namespace ast
