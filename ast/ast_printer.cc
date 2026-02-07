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

void ASTPrinter::Indent() { ++indent_; }

void ASTPrinter::Dedent() {
  if (indent_ > 0) {
    --indent_;
  }
}

void ASTPrinter::Visit(Statement &node) {
  (void)node;
  Line("Statement");
}

void ASTPrinter::Visit(Query &node) {
  (void)node;
  Line("Query");
}

void ASTPrinter::Visit(RegularQuery &node) {
  Line("RegularQuery");
  Indent();
  VisitMaybe(node.single_query);
  for (const auto &part : node.unions) {
    VisitMaybe(part);
  }
  Dedent();
}

void ASTPrinter::Visit(StandaloneCall &node) {
  std::ostringstream oss;
  oss << "StandaloneCall procedure=" << node.procedure_name;
  if (node.yield_star) {
    oss << " yield=*";
  }
  Line(oss.str());
  Indent();
  for (const auto &arg : node.arguments) {
    VisitMaybe(arg);
  }
  for (const auto &item : node.yield_items) {
    std::ostringstream item_line;
    item_line << "YieldItem ";
    if (item.result_field) {
      item_line << *item.result_field << " AS ";
    }
    item_line << item.variable;
    Line(item_line.str());
  }
  VisitMaybe(node.yield_where);
  Dedent();
}

void ASTPrinter::Visit(SingleQuery &node) {
  (void)node;
  Line("SingleQuery");
}

void ASTPrinter::Visit(SinglePartQuery &node) {
  Line("SinglePartQuery");
  Indent();
  for (const auto &rc : node.reading_clauses) {
    VisitMaybe(rc);
  }
  for (const auto &uc : node.updating_clauses) {
    VisitMaybe(uc);
  }
  VisitMaybe(node.return_clause);
  Dedent();
}

void ASTPrinter::Visit(MultiPartQuery &node) {
  Line("MultiPartQuery");
  Indent();
  for (const auto &part : node.parts) {
    Line("WithPart");
    Indent();
    for (const auto &rc : part.reading_clauses) {
      VisitMaybe(rc);
    }
    for (const auto &uc : part.updating_clauses) {
      VisitMaybe(uc);
    }
    VisitMaybe(part.with_clause);
    Dedent();
  }
  VisitMaybe(node.final_single_part_query);
  Dedent();
}

void ASTPrinter::Visit(UnionPart &node) {
  std::ostringstream oss;
  oss << "UnionPart all=" << (node.all ? "true" : "false");
  Line(oss.str());
  Indent();
  VisitMaybe(node.query);
  Dedent();
}

void ASTPrinter::Visit(Expression &node) {
  (void)node;
  Line("Expression");
}

void ASTPrinter::Visit(BinaryExpression &node) {
  Line("BinaryExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(OrExpression &node) {
  Line("OrExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(XorExpression &node) {
  Line("XorExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(AndExpression &node) {
  Line("AndExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(ComparisonExpression &node) {
  std::ostringstream oss;
  oss << "ComparisonExpression op=" << node.op;
  Line(oss.str());
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(ComparisonChainExpression &node) {
  Line("ComparisonChainExpression");
  Indent();
  VisitMaybe(node.left);
  for (const auto &entry : node.rights) {
    std::ostringstream oss;
    oss << "Op " << entry.first;
    Line(oss.str());
    Indent();
    VisitMaybe(entry.second);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(AddExpression &node) {
  Line("AddExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(SubtractExpression &node) {
  Line("SubtractExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(MultiplyExpression &node) {
  Line("MultiplyExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(DivideExpression &node) {
  Line("DivideExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(ModuloExpression &node) {
  Line("ModuloExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(PowerExpression &node) {
  Line("PowerExpression");
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(UnaryExpression &node) {
  Line("UnaryExpression");
  Indent();
  VisitMaybe(node.operand);
  Dedent();
}

void ASTPrinter::Visit(NotExpression &node) {
  Line("NotExpression");
  Indent();
  VisitMaybe(node.operand);
  Dedent();
}

void ASTPrinter::Visit(UnaryPlusExpression &node) {
  Line("UnaryPlusExpression");
  Indent();
  VisitMaybe(node.operand);
  Dedent();
}

void ASTPrinter::Visit(UnaryMinusExpression &node) {
  Line("UnaryMinusExpression");
  Indent();
  VisitMaybe(node.operand);
  Dedent();
}

void ASTPrinter::Visit(StringPredicateExpression &node) {
  std::ostringstream oss;
  oss << "StringPredicateExpression op=" << node.op;
  Line(oss.str());
  Indent();
  VisitMaybe(node.left);
  VisitMaybe(node.right);
  Dedent();
}

void ASTPrinter::Visit(ListPredicateExpression &node) {
  Line("ListPredicateExpression IN");
  Indent();
  VisitMaybe(node.element);
  VisitMaybe(node.list);
  Dedent();
}

void ASTPrinter::Visit(LabelPredicateExpression &node) {
  std::ostringstream oss;
  oss << "LabelPredicateExpression labels=";
  for (size_t i = 0; i < node.labels.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << node.labels[i];
  }
  Line(oss.str());
  Indent();
  VisitMaybe(node.expr);
  Dedent();
}

void ASTPrinter::Visit(NullPredicateExpression &node) {
  std::ostringstream oss;
  oss << "NullPredicateExpression is_null="
      << (node.is_null ? "true" : "false");
  Line(oss.str());
  Indent();
  VisitMaybe(node.operand);
  Dedent();
}

void ASTPrinter::Visit(Literal &node) {
  (void)node;
  Line("Literal");
}

void ASTPrinter::Visit(BooleanLiteral &node) {
  Line(std::string("BooleanLiteral value=") + (node.value ? "true" : "false"));
}

void ASTPrinter::Visit(IntegerLiteral &node) {
  std::ostringstream oss;
  oss << "IntegerLiteral value=" << node.value;
  Line(oss.str());
}

void ASTPrinter::Visit(DoubleLiteral &node) {
  std::ostringstream oss;
  oss << "DoubleLiteral value=" << node.value;
  Line(oss.str());
}

void ASTPrinter::Visit(StringLiteral &node) {
  std::ostringstream oss;
  oss << "StringLiteral value=\"" << node.value << "\"";
  Line(oss.str());
}

void ASTPrinter::Visit(NullLiteral &node) {
  (void)node;
  Line("NullLiteral");
}

void ASTPrinter::Visit(ListLiteral &node) {
  Line("ListLiteral");
  Indent();
  for (const auto &elem : node.elements) {
    VisitMaybe(elem);
  }
  Dedent();
}

void ASTPrinter::Visit(MapLiteral &node) {
  Line("MapLiteral");
  Indent();
  for (const auto &entry : node.entries) {
    std::ostringstream oss;
    oss << "Entry key=" << entry.first;
    Line(oss.str());
    Indent();
    VisitMaybe(entry.second);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(Properties &node) {
  Line("Properties");
  Indent();
  VisitMaybe(node.map);
  VisitMaybe(node.parameter);
  Dedent();
}

void ASTPrinter::Visit(Variable &node) {
  Line(std::string("Variable name=") + node.name);
}

void ASTPrinter::Visit(Parameter &node) {
  Line(std::string("Parameter name=") + node.name);
}

void ASTPrinter::Visit(PropertyExpression &node) {
  std::ostringstream oss;
  oss << "PropertyExpression key=" << node.property_key;
  Line(oss.str());
  Indent();
  VisitMaybe(node.object);
  Dedent();
}

void ASTPrinter::Visit(ListIndexExpression &node) {
  Line("ListIndexExpression");
  Indent();
  VisitMaybe(node.list);
  VisitMaybe(node.index);
  Dedent();
}

void ASTPrinter::Visit(ListSliceExpression &node) {
  Line("ListSliceExpression");
  Indent();
  VisitMaybe(node.list);
  if (node.start_index) {
    Line("StartIndex");
    Indent();
    VisitMaybe(node.start_index);
    Dedent();
  }
  if (node.end_index) {
    Line("EndIndex");
    Indent();
    VisitMaybe(node.end_index);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(FunctionInvocation &node) {
  std::ostringstream oss;
  oss << "FunctionInvocation name=" << node.function_name
      << " distinct=" << (node.distinct ? "true" : "false");
  Line(oss.str());
  Indent();
  for (const auto &arg : node.arguments) {
    VisitMaybe(arg);
  }
  Dedent();
}

void ASTPrinter::Visit(CountStarExpression &node) {
  (void)node;
  Line("CountStarExpression");
}

void ASTPrinter::Visit(CaseExpression &node) {
  Line("CaseExpression");
  Indent();
  if (node.test) {
    Line("Test");
    Indent();
    VisitMaybe(node.test);
    Dedent();
  }
  for (const auto &alt : node.alternatives) {
    Line("Alternative");
    Indent();
    Line("When");
    Indent();
    VisitMaybe(alt.first);
    Dedent();
    Line("Then");
    Indent();
    VisitMaybe(alt.second);
    Dedent();
    Dedent();
  }
  if (node.else_expr) {
    Line("Else");
    Indent();
    VisitMaybe(node.else_expr);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(ParenthesizedExpression &node) {
  Line("ParenthesizedExpression");
  Indent();
  VisitMaybe(node.expr);
  Dedent();
}

void ASTPrinter::Visit(ListComprehension &node) {
  std::ostringstream oss;
  oss << "ListComprehension variable=" << node.variable;
  Line(oss.str());
  Indent();
  Line("List");
  Indent();
  VisitMaybe(node.list_expr);
  Dedent();
  if (node.where_expr) {
    Line("Where");
    Indent();
    VisitMaybe(node.where_expr);
    Dedent();
  }
  if (node.eval_expr) {
    Line("Eval");
    Indent();
    VisitMaybe(node.eval_expr);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(PatternComprehension &node) {
  std::ostringstream oss;
  oss << "PatternComprehension variable=" << node.variable;
  Line(oss.str());
  Indent();
  VisitMaybe(node.relationships_pattern);
  if (node.where_expr) {
    Line("Where");
    Indent();
    VisitMaybe(node.where_expr);
    Dedent();
  }
  Line("Eval");
  Indent();
  VisitMaybe(node.eval_expr);
  Dedent();
  Dedent();
}

void ASTPrinter::Visit(PatternPredicateExpression &node) {
  Line("PatternPredicateExpression");
  Indent();
  VisitMaybe(node.relationships_pattern);
  Dedent();
}

void ASTPrinter::Visit(Quantifier &node) {
  std::ostringstream oss;
  oss << "Quantifier variable=" << node.variable;
  Line(oss.str());
  Indent();
  Line("List");
  Indent();
  VisitMaybe(node.list_expr);
  Dedent();
  if (node.predicate) {
    Line("Predicate");
    Indent();
    VisitMaybe(node.predicate);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(AllQuantifier &node) {
  Line("AllQuantifier");
  Indent();
  Quantifier &base = node;
  Visit(base);
  Dedent();
}

void ASTPrinter::Visit(AnyQuantifier &node) {
  Line("AnyQuantifier");
  Indent();
  Quantifier &base = node;
  Visit(base);
  Dedent();
}

void ASTPrinter::Visit(NoneQuantifier &node) {
  Line("NoneQuantifier");
  Indent();
  Quantifier &base = node;
  Visit(base);
  Dedent();
}

void ASTPrinter::Visit(SingleQuantifier &node) {
  Line("SingleQuantifier");
  Indent();
  Quantifier &base = node;
  Visit(base);
  Dedent();
}

void ASTPrinter::Visit(ExistentialSubquery &node) {
  Line("ExistentialSubquery");
  Indent();
  VisitMaybe(node.query);
  VisitMaybe(node.pattern);
  VisitMaybe(node.where_expr);
  Dedent();
}

void ASTPrinter::Visit(Pattern &node) {
  Line("Pattern");
  Indent();
  for (const auto &part : node.parts) {
    VisitMaybe(part);
  }
  Dedent();
}

void ASTPrinter::Visit(PatternPart &node) {
  std::ostringstream oss;
  oss << "PatternPart";
  if (!node.variable.empty()) {
    oss << " variable=" << node.variable;
  }
  Line(oss.str());
  Indent();
  VisitMaybe(node.element);
  Dedent();
}

void ASTPrinter::Visit(PatternElement &node) {
  Line("PatternElement");
  Indent();
  VisitMaybe(node.node_pattern);
  for (const auto &entry : node.chain) {
    Line("Chain");
    Indent();
    VisitMaybe(entry.first);
    VisitMaybe(entry.second);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(RelationshipsPattern &node) {
  Line("RelationshipsPattern");
  Indent();
  VisitMaybe(node.node_pattern);
  for (const auto &entry : node.chain) {
    Line("Chain");
    Indent();
    VisitMaybe(entry.first);
    VisitMaybe(entry.second);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(NodePattern &node) {
  std::ostringstream oss;
  oss << "NodePattern";
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
  Indent();
  VisitMaybe(node.properties);
  Dedent();
}

void ASTPrinter::Visit(RelationshipPattern &node) {
  std::ostringstream oss;
  oss << "RelationshipPattern left=" << (node.left_arrow ? "true" : "false")
      << " right=" << (node.right_arrow ? "true" : "false");
  Line(oss.str());
  Indent();
  VisitMaybe(node.detail);
  Dedent();
}

void ASTPrinter::Visit(RelationshipDetail &node) {
  std::ostringstream oss;
  oss << "RelationshipDetail";
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
  Indent();
  VisitMaybe(node.properties);
  Dedent();
}

void ASTPrinter::Visit(Clause &node) {
  (void)node;
  Line("Clause");
}

void ASTPrinter::Visit(ReadingClause &node) {
  (void)node;
  Line("ReadingClause");
}

void ASTPrinter::Visit(Match &node) {
  std::ostringstream oss;
  oss << "Match optional=" << (node.optional_match ? "true" : "false");
  Line(oss.str());
  Indent();
  VisitMaybe(node.pattern);
  VisitMaybe(node.where);
  Dedent();
}

void ASTPrinter::Visit(Unwind &node) {
  std::ostringstream oss;
  oss << "Unwind variable=" << node.variable;
  Line(oss.str());
  Indent();
  VisitMaybe(node.expression);
  Dedent();
}

void ASTPrinter::Visit(InQueryCall &node) {
  std::ostringstream oss;
  oss << "InQueryCall procedure=" << node.procedure_name;
  Line(oss.str());
  Indent();
  for (const auto &arg : node.arguments) {
    VisitMaybe(arg);
  }
  for (const auto &item : node.yield_items) {
    std::ostringstream item_line;
    item_line << "YieldItem ";
    if (item.result_field) {
      item_line << *item.result_field << " AS ";
    }
    item_line << item.variable;
    Line(item_line.str());
  }
  VisitMaybe(node.yield_where);
  Dedent();
}

void ASTPrinter::Visit(UpdatingClause &node) {
  (void)node;
  Line("UpdatingClause");
}

void ASTPrinter::Visit(Create &node) {
  Line("Create");
  Indent();
  VisitMaybe(node.pattern);
  Dedent();
}

void ASTPrinter::Visit(Merge &node) {
  Line("Merge");
  Indent();
  VisitMaybe(node.pattern_part);
  for (const auto &action : node.actions) {
    std::ostringstream oss;
    oss << (action.first ? "OnMatch" : "OnCreate");
    Line(oss.str());
    Indent();
    VisitMaybe(action.second);
    Dedent();
  }
  Dedent();
}

void ASTPrinter::Visit(Delete &node) {
  std::ostringstream oss;
  oss << "Delete detach=" << (node.detach ? "true" : "false");
  Line(oss.str());
  Indent();
  for (const auto &expr : node.expressions) {
    VisitMaybe(expr);
  }
  Dedent();
}

void ASTPrinter::Visit(Set &node) {
  Line("Set");
  Indent();
  for (const auto &item : node.items) {
    VisitMaybe(item);
  }
  Dedent();
}

void ASTPrinter::Visit(SetItem &node) {
  std::ostringstream oss;
  oss << "SetItem type=";
  switch (node.type) {
    case SetItem::Type::kProperty:
      oss << "Property";
      break;
    case SetItem::Type::kVariable:
      oss << "Variable";
      break;
    case SetItem::Type::kLabels:
      oss << "Labels";
      break;
  }
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
  Indent();
  VisitMaybe(node.target);
  VisitMaybe(node.value);
  Dedent();
}

void ASTPrinter::Visit(Remove &node) {
  Line("Remove");
  Indent();
  for (const auto &item : node.items) {
    VisitMaybe(item);
  }
  Dedent();
}

void ASTPrinter::Visit(RemoveItem &node) {
  std::ostringstream oss;
  oss << "RemoveItem type=";
  switch (node.type) {
    case RemoveItem::Type::kProperty:
      oss << "Property";
      break;
    case RemoveItem::Type::kLabels:
      oss << "Labels";
      break;
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
  Indent();
  VisitMaybe(node.target);
  Dedent();
}

void ASTPrinter::Visit(ProjectionClause &node) {
  (void)node;
  Line("ProjectionClause");
}

void ASTPrinter::Visit(ProjectionBody &node) {
  std::ostringstream oss;
  oss << "ProjectionBody distinct=" << (node.distinct ? "true" : "false")
      << " star=" << (node.star ? "true" : "false");
  Line(oss.str());
  Indent();
  for (const auto &item : node.items) {
    VisitMaybe(item);
  }
  for (const auto &item : node.order_by) {
    VisitMaybe(item);
  }
  VisitMaybe(node.skip);
  VisitMaybe(node.limit);
  Dedent();
}

void ASTPrinter::Visit(ProjectionItem &node) {
  std::ostringstream oss;
  oss << "ProjectionItem";
  if (!node.alias.empty()) {
    oss << " alias=" << node.alias;
  }
  Line(oss.str());
  Indent();
  VisitMaybe(node.expression);
  Dedent();
}

void ASTPrinter::Visit(SortItem &node) {
  std::ostringstream oss;
  oss << "SortItem ascending=" << (node.ascending ? "true" : "false");
  Line(oss.str());
  Indent();
  VisitMaybe(node.expression);
  Dedent();
}

void ASTPrinter::Visit(With &node) {
  Line("With");
  Indent();
  VisitMaybe(node.body);
  VisitMaybe(node.where);
  Dedent();
}

void ASTPrinter::Visit(Return &node) {
  Line("Return");
  Indent();
  VisitMaybe(node.body);
  Dedent();
}

}  // namespace ast
