#include "ast_printer.h"

#include <sstream>

namespace cypher {

ASTPrinter::ASTPrinter(std::ostream &out) : out_(out) {}

void ASTPrinter::print(ASTNode &node) { node.accept(*this); }

void ASTPrinter::line(const std::string &text) {
  for (int i = 0; i < indent_; ++i) {
    out_ << "  ";
  }
  out_ << text << "\n";
}

void ASTPrinter::indent() { ++indent_; }

void ASTPrinter::dedent() {
  if (indent_ > 0) {
    --indent_;
  }
}

void ASTPrinter::visit(Statement &node) {
  (void)node;
  line("Statement");
}

void ASTPrinter::visit(Query &node) {
  (void)node;
  line("Query");
}

void ASTPrinter::visit(RegularQuery &node) {
  line("RegularQuery");
  indent();
  visitMaybe(node.single_query);
  for (const auto &part : node.unions) {
    visitMaybe(part);
  }
  dedent();
}

void ASTPrinter::visit(StandaloneCall &node) {
  std::ostringstream oss;
  oss << "StandaloneCall procedure=" << node.procedure_name;
  if (node.yield_star) {
    oss << " yield=*";
  }
  line(oss.str());
  indent();
  for (const auto &arg : node.arguments) {
    visitMaybe(arg);
  }
  for (const auto &item : node.yield_items) {
    std::ostringstream item_line;
    item_line << "YieldItem ";
    if (item.result_field) {
      item_line << *item.result_field << " AS ";
    }
    item_line << item.variable;
    line(item_line.str());
  }
  visitMaybe(node.yield_where);
  dedent();
}

void ASTPrinter::visit(SingleQuery &node) {
  (void)node;
  line("SingleQuery");
}

void ASTPrinter::visit(SinglePartQuery &node) {
  line("SinglePartQuery");
  indent();
  for (const auto &rc : node.reading_clauses) {
    visitMaybe(rc);
  }
  for (const auto &uc : node.updating_clauses) {
    visitMaybe(uc);
  }
  visitMaybe(node.return_clause);
  dedent();
}

void ASTPrinter::visit(MultiPartQuery &node) {
  line("MultiPartQuery");
  indent();
  for (const auto &part : node.parts) {
    line("WithPart");
    indent();
    for (const auto &rc : part.reading_clauses) {
      visitMaybe(rc);
    }
    for (const auto &uc : part.updating_clauses) {
      visitMaybe(uc);
    }
    visitMaybe(part.with_clause);
    dedent();
  }
  visitMaybe(node.final_single_part_query);
  dedent();
}

void ASTPrinter::visit(UnionPart &node) {
  std::ostringstream oss;
  oss << "UnionPart all=" << (node.all ? "true" : "false");
  line(oss.str());
  indent();
  visitMaybe(node.query);
  dedent();
}

void ASTPrinter::visit(Expression &node) {
  (void)node;
  line("Expression");
}

void ASTPrinter::visit(BinaryExpression &node) {
  line("BinaryExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(OrExpression &node) {
  line("OrExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(XorExpression &node) {
  line("XorExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(AndExpression &node) {
  line("AndExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(ComparisonExpression &node) {
  std::ostringstream oss;
  oss << "ComparisonExpression op=" << node.op;
  line(oss.str());
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(ComparisonChainExpression &node) {
  line("ComparisonChainExpression");
  indent();
  visitMaybe(node.left);
  for (const auto &entry : node.rights) {
    std::ostringstream oss;
    oss << "Op " << entry.first;
    line(oss.str());
    indent();
    visitMaybe(entry.second);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(AddExpression &node) {
  line("AddExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(SubtractExpression &node) {
  line("SubtractExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(MultiplyExpression &node) {
  line("MultiplyExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(DivideExpression &node) {
  line("DivideExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(ModuloExpression &node) {
  line("ModuloExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(PowerExpression &node) {
  line("PowerExpression");
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(UnaryExpression &node) {
  line("UnaryExpression");
  indent();
  visitMaybe(node.operand);
  dedent();
}

void ASTPrinter::visit(NotExpression &node) {
  line("NotExpression");
  indent();
  visitMaybe(node.operand);
  dedent();
}

void ASTPrinter::visit(UnaryPlusExpression &node) {
  line("UnaryPlusExpression");
  indent();
  visitMaybe(node.operand);
  dedent();
}

void ASTPrinter::visit(UnaryMinusExpression &node) {
  line("UnaryMinusExpression");
  indent();
  visitMaybe(node.operand);
  dedent();
}

void ASTPrinter::visit(StringPredicateExpression &node) {
  std::ostringstream oss;
  oss << "StringPredicateExpression op=" << node.op;
  line(oss.str());
  indent();
  visitMaybe(node.left);
  visitMaybe(node.right);
  dedent();
}

void ASTPrinter::visit(ListPredicateExpression &node) {
  line("ListPredicateExpression IN");
  indent();
  visitMaybe(node.element);
  visitMaybe(node.list);
  dedent();
}

void ASTPrinter::visit(LabelPredicateExpression &node) {
  std::ostringstream oss;
  oss << "LabelPredicateExpression labels=";
  for (size_t i = 0; i < node.labels.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << node.labels[i];
  }
  line(oss.str());
  indent();
  visitMaybe(node.expr);
  dedent();
}

void ASTPrinter::visit(NullPredicateExpression &node) {
  std::ostringstream oss;
  oss << "NullPredicateExpression is_null="
      << (node.is_null ? "true" : "false");
  line(oss.str());
  indent();
  visitMaybe(node.operand);
  dedent();
}

void ASTPrinter::visit(Literal &node) {
  (void)node;
  line("Literal");
}

void ASTPrinter::visit(BooleanLiteral &node) {
  line(std::string("BooleanLiteral value=") +
       (node.value ? "true" : "false"));
}

void ASTPrinter::visit(IntegerLiteral &node) {
  std::ostringstream oss;
  oss << "IntegerLiteral value=" << node.value;
  line(oss.str());
}

void ASTPrinter::visit(DoubleLiteral &node) {
  std::ostringstream oss;
  oss << "DoubleLiteral value=" << node.value;
  line(oss.str());
}

void ASTPrinter::visit(StringLiteral &node) {
  std::ostringstream oss;
  oss << "StringLiteral value=\"" << node.value << "\"";
  line(oss.str());
}

void ASTPrinter::visit(NullLiteral &node) {
  (void)node;
  line("NullLiteral");
}

void ASTPrinter::visit(ListLiteral &node) {
  line("ListLiteral");
  indent();
  for (const auto &elem : node.elements) {
    visitMaybe(elem);
  }
  dedent();
}

void ASTPrinter::visit(MapLiteral &node) {
  line("MapLiteral");
  indent();
  for (const auto &entry : node.entries) {
    std::ostringstream oss;
    oss << "Entry key=" << entry.first;
    line(oss.str());
    indent();
    visitMaybe(entry.second);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(Properties &node) {
  line("Properties");
  indent();
  visitMaybe(node.map);
  visitMaybe(node.parameter);
  dedent();
}

void ASTPrinter::visit(Variable &node) {
  line(std::string("Variable name=") + node.name);
}

void ASTPrinter::visit(Parameter &node) {
  line(std::string("Parameter name=") + node.name);
}

void ASTPrinter::visit(PropertyExpression &node) {
  std::ostringstream oss;
  oss << "PropertyExpression key=" << node.property_key;
  line(oss.str());
  indent();
  visitMaybe(node.object);
  dedent();
}

void ASTPrinter::visit(ListIndexExpression &node) {
  line("ListIndexExpression");
  indent();
  visitMaybe(node.list);
  visitMaybe(node.index);
  dedent();
}

void ASTPrinter::visit(ListSliceExpression &node) {
  line("ListSliceExpression");
  indent();
  visitMaybe(node.list);
  if (node.start_index) {
    line("StartIndex");
    indent();
    visitMaybe(node.start_index);
    dedent();
  }
  if (node.end_index) {
    line("EndIndex");
    indent();
    visitMaybe(node.end_index);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(FunctionInvocation &node) {
  std::ostringstream oss;
  oss << "FunctionInvocation name=" << node.function_name
      << " distinct=" << (node.distinct ? "true" : "false");
  line(oss.str());
  indent();
  for (const auto &arg : node.arguments) {
    visitMaybe(arg);
  }
  dedent();
}

void ASTPrinter::visit(CountStarExpression &node) {
  (void)node;
  line("CountStarExpression");
}

void ASTPrinter::visit(CaseExpression &node) {
  line("CaseExpression");
  indent();
  if (node.test) {
    line("Test");
    indent();
    visitMaybe(node.test);
    dedent();
  }
  for (const auto &alt : node.alternatives) {
    line("Alternative");
    indent();
    line("When");
    indent();
    visitMaybe(alt.first);
    dedent();
    line("Then");
    indent();
    visitMaybe(alt.second);
    dedent();
    dedent();
  }
  if (node.else_expr) {
    line("Else");
    indent();
    visitMaybe(node.else_expr);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(ParenthesizedExpression &node) {
  line("ParenthesizedExpression");
  indent();
  visitMaybe(node.expr);
  dedent();
}

void ASTPrinter::visit(ListComprehension &node) {
  std::ostringstream oss;
  oss << "ListComprehension variable=" << node.variable;
  line(oss.str());
  indent();
  line("List");
  indent();
  visitMaybe(node.list_expr);
  dedent();
  if (node.where_expr) {
    line("Where");
    indent();
    visitMaybe(node.where_expr);
    dedent();
  }
  if (node.eval_expr) {
    line("Eval");
    indent();
    visitMaybe(node.eval_expr);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(PatternComprehension &node) {
  std::ostringstream oss;
  oss << "PatternComprehension variable=" << node.variable;
  line(oss.str());
  indent();
  visitMaybe(node.relationships_pattern);
  if (node.where_expr) {
    line("Where");
    indent();
    visitMaybe(node.where_expr);
    dedent();
  }
  line("Eval");
  indent();
  visitMaybe(node.eval_expr);
  dedent();
  dedent();
}

void ASTPrinter::visit(PatternPredicateExpression &node) {
  line("PatternPredicateExpression");
  indent();
  visitMaybe(node.relationships_pattern);
  dedent();
}

void ASTPrinter::visit(Quantifier &node) {
  std::ostringstream oss;
  oss << "Quantifier variable=" << node.variable;
  line(oss.str());
  indent();
  line("List");
  indent();
  visitMaybe(node.list_expr);
  dedent();
  if (node.predicate) {
    line("Predicate");
    indent();
    visitMaybe(node.predicate);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(AllQuantifier &node) {
  line("AllQuantifier");
  indent();
  Quantifier &base = node;
  visit(base);
  dedent();
}

void ASTPrinter::visit(AnyQuantifier &node) {
  line("AnyQuantifier");
  indent();
  Quantifier &base = node;
  visit(base);
  dedent();
}

void ASTPrinter::visit(NoneQuantifier &node) {
  line("NoneQuantifier");
  indent();
  Quantifier &base = node;
  visit(base);
  dedent();
}

void ASTPrinter::visit(SingleQuantifier &node) {
  line("SingleQuantifier");
  indent();
  Quantifier &base = node;
  visit(base);
  dedent();
}

void ASTPrinter::visit(ExistentialSubquery &node) {
  line("ExistentialSubquery");
  indent();
  visitMaybe(node.query);
  visitMaybe(node.pattern);
  visitMaybe(node.where_expr);
  dedent();
}

void ASTPrinter::visit(Pattern &node) {
  line("Pattern");
  indent();
  for (const auto &part : node.parts) {
    visitMaybe(part);
  }
  dedent();
}

void ASTPrinter::visit(PatternPart &node) {
  std::ostringstream oss;
  oss << "PatternPart";
  if (!node.variable.empty()) {
    oss << " variable=" << node.variable;
  }
  line(oss.str());
  indent();
  visitMaybe(node.element);
  dedent();
}

void ASTPrinter::visit(PatternElement &node) {
  line("PatternElement");
  indent();
  visitMaybe(node.node_pattern);
  for (const auto &entry : node.chain) {
    line("Chain");
    indent();
    visitMaybe(entry.first);
    visitMaybe(entry.second);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(RelationshipsPattern &node) {
  line("RelationshipsPattern");
  indent();
  visitMaybe(node.node_pattern);
  for (const auto &entry : node.chain) {
    line("Chain");
    indent();
    visitMaybe(entry.first);
    visitMaybe(entry.second);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(NodePattern &node) {
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
  line(oss.str());
  indent();
  visitMaybe(node.properties);
  dedent();
}

void ASTPrinter::visit(RelationshipPattern &node) {
  std::ostringstream oss;
  oss << "RelationshipPattern left=" << (node.left_arrow ? "true" : "false")
      << " right=" << (node.right_arrow ? "true" : "false");
  line(oss.str());
  indent();
  visitMaybe(node.detail);
  dedent();
}

void ASTPrinter::visit(RelationshipDetail &node) {
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
  line(oss.str());
  indent();
  visitMaybe(node.properties);
  dedent();
}

void ASTPrinter::visit(Clause &node) {
  (void)node;
  line("Clause");
}

void ASTPrinter::visit(ReadingClause &node) {
  (void)node;
  line("ReadingClause");
}

void ASTPrinter::visit(Match &node) {
  std::ostringstream oss;
  oss << "Match optional=" << (node.optional_match ? "true" : "false");
  line(oss.str());
  indent();
  visitMaybe(node.pattern);
  visitMaybe(node.where);
  dedent();
}

void ASTPrinter::visit(Unwind &node) {
  std::ostringstream oss;
  oss << "Unwind variable=" << node.variable;
  line(oss.str());
  indent();
  visitMaybe(node.expression);
  dedent();
}

void ASTPrinter::visit(InQueryCall &node) {
  std::ostringstream oss;
  oss << "InQueryCall procedure=" << node.procedure_name;
  line(oss.str());
  indent();
  for (const auto &arg : node.arguments) {
    visitMaybe(arg);
  }
  for (const auto &item : node.yield_items) {
    std::ostringstream item_line;
    item_line << "YieldItem ";
    if (item.result_field) {
      item_line << *item.result_field << " AS ";
    }
    item_line << item.variable;
    line(item_line.str());
  }
  visitMaybe(node.yield_where);
  dedent();
}

void ASTPrinter::visit(UpdatingClause &node) {
  (void)node;
  line("UpdatingClause");
}

void ASTPrinter::visit(Create &node) {
  line("Create");
  indent();
  visitMaybe(node.pattern);
  dedent();
}

void ASTPrinter::visit(Merge &node) {
  line("Merge");
  indent();
  visitMaybe(node.pattern_part);
  for (const auto &action : node.actions) {
    std::ostringstream oss;
    oss << (action.first ? "OnMatch" : "OnCreate");
    line(oss.str());
    indent();
    visitMaybe(action.second);
    dedent();
  }
  dedent();
}

void ASTPrinter::visit(Delete &node) {
  std::ostringstream oss;
  oss << "Delete detach=" << (node.detach ? "true" : "false");
  line(oss.str());
  indent();
  for (const auto &expr : node.expressions) {
    visitMaybe(expr);
  }
  dedent();
}

void ASTPrinter::visit(Set &node) {
  line("Set");
  indent();
  for (const auto &item : node.items) {
    visitMaybe(item);
  }
  dedent();
}

void ASTPrinter::visit(SetItem &node) {
  std::ostringstream oss;
  oss << "SetItem type=";
  switch (node.type) {
    case SetItem::Type::Property:
      oss << "Property";
      break;
    case SetItem::Type::Variable:
      oss << "Variable";
      break;
    case SetItem::Type::Labels:
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
  line(oss.str());
  indent();
  visitMaybe(node.target);
  visitMaybe(node.value);
  dedent();
}

void ASTPrinter::visit(Remove &node) {
  line("Remove");
  indent();
  for (const auto &item : node.items) {
    visitMaybe(item);
  }
  dedent();
}

void ASTPrinter::visit(RemoveItem &node) {
  std::ostringstream oss;
  oss << "RemoveItem type=";
  switch (node.type) {
    case RemoveItem::Type::Property:
      oss << "Property";
      break;
    case RemoveItem::Type::Labels:
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
  line(oss.str());
  indent();
  visitMaybe(node.target);
  dedent();
}

void ASTPrinter::visit(ProjectionClause &node) {
  (void)node;
  line("ProjectionClause");
}

void ASTPrinter::visit(ProjectionBody &node) {
  std::ostringstream oss;
  oss << "ProjectionBody distinct=" << (node.distinct ? "true" : "false")
      << " star=" << (node.star ? "true" : "false");
  line(oss.str());
  indent();
  for (const auto &item : node.items) {
    visitMaybe(item);
  }
  for (const auto &item : node.order_by) {
    visitMaybe(item);
  }
  visitMaybe(node.skip);
  visitMaybe(node.limit);
  dedent();
}

void ASTPrinter::visit(ProjectionItem &node) {
  std::ostringstream oss;
  oss << "ProjectionItem";
  if (!node.alias.empty()) {
    oss << " alias=" << node.alias;
  }
  line(oss.str());
  indent();
  visitMaybe(node.expression);
  dedent();
}

void ASTPrinter::visit(SortItem &node) {
  std::ostringstream oss;
  oss << "SortItem ascending=" << (node.ascending ? "true" : "false");
  line(oss.str());
  indent();
  visitMaybe(node.expression);
  dedent();
}

void ASTPrinter::visit(With &node) {
  line("With");
  indent();
  visitMaybe(node.body);
  visitMaybe(node.where);
  dedent();
}

void ASTPrinter::visit(Return &node) {
  line("Return");
  indent();
  visitMaybe(node.body);
  dedent();
}

}  // namespace cypher
