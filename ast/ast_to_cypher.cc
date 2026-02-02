#include "ast_to_cypher.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

namespace ast {
namespace {

class CypherPrinter : public ASTVisitor {
 public:
  std::string print(ASTNode &node) {
    stack_.clear();
    node.accept(*this);
    if (stack_.empty()) {
      return {};
    }
    std::string out = std::move(stack_.back());
    stack_.pop_back();
    return out;
  }

  void visit(Statement &node) override {
    (void)node;
    push("");
  }

  void visit(Query &node) override {
    (void)node;
    push("");
  }

  void visit(RegularQuery &node) override {
    std::vector<std::string> parts;
    auto main = renderMaybe(node.single_query);
    if (!main.empty()) {
      parts.push_back(std::move(main));
    }
    for (const auto &part : node.unions) {
      auto piece = renderMaybe(part);
      if (!piece.empty()) {
        parts.push_back(std::move(piece));
      }
    }
    push(join(parts, " "));
  }

  void visit(StandaloneCall &node) override {
    std::string out = "CALL ";
    out += renderQualifiedName(node.procedure_name);
    out += "(";
    out += join(renderList(node.arguments), ", ");
    out += ")";
    if (node.yield_star) {
      out += " YIELD *";
    } else if (!node.yield_items.empty()) {
      out += " YIELD ";
      out += join(renderYieldItems(node.yield_items), ", ");
      if (node.yield_where) {
        out += " WHERE ";
        out += renderMaybe(node.yield_where);
      }
    }
    push(out);
  }

  void visit(SingleQuery &node) override {
    (void)node;
    push("");
  }

  void visit(SinglePartQuery &node) override {
    std::vector<std::string> clauses;
    appendClauses(clauses, node.reading_clauses);
    appendClauses(clauses, node.updating_clauses);
    auto ret = renderMaybe(node.return_clause);
    if (!ret.empty()) {
      clauses.push_back(std::move(ret));
    }
    push(join(clauses, " "));
  }

  void visit(MultiPartQuery &node) override {
    std::vector<std::string> clauses;
    for (const auto &part : node.parts) {
      std::vector<std::string> with_part;
      appendClauses(with_part, part.reading_clauses);
      appendClauses(with_part, part.updating_clauses);
      auto with_clause = renderMaybe(part.with_clause);
      if (!with_clause.empty()) {
        with_part.push_back(std::move(with_clause));
      }
      if (!with_part.empty()) {
        clauses.push_back(join(with_part, " "));
      }
    }
    auto final_part = renderMaybe(node.final_single_part_query);
    if (!final_part.empty()) {
      clauses.push_back(std::move(final_part));
    }
    push(join(clauses, " "));
  }

  void visit(UnionPart &node) override {
    std::string out = "UNION";
    if (node.all) {
      out += " ALL";
    }
    auto query = renderMaybe(node.query);
    if (!query.empty()) {
      out += " ";
      out += std::move(query);
    }
    push(out);
  }

  void visit(Expression &node) override {
    (void)node;
    push("");
  }

  void visit(BinaryExpression &node) override {
    (void)node;
    push("");
  }

  void visit(OrExpression &node) override {
    push(wrapBinary("OR", node.left, node.right));
  }

  void visit(XorExpression &node) override {
    push(wrapBinary("XOR", node.left, node.right));
  }

  void visit(AndExpression &node) override {
    push(wrapBinary("AND", node.left, node.right));
  }

  void visit(ComparisonExpression &node) override {
    auto left = renderMaybe(node.left);
    auto right = renderMaybe(node.right);
    push(wrap(left + " " + node.op + " " + right));
  }

  void visit(ComparisonChainExpression &node) override {
    auto left = renderMaybe(node.left);
    std::ostringstream oss;
    oss << left;
    for (const auto &entry : node.rights) {
      oss << " " << entry.first << " " << renderMaybe(entry.second);
    }
    push(wrap(oss.str()));
  }

  void visit(AddExpression &node) override {
    push(wrapBinary("+", node.left, node.right));
  }

  void visit(SubtractExpression &node) override {
    push(wrapBinary("-", node.left, node.right));
  }

  void visit(MultiplyExpression &node) override {
    push(wrapBinary("*", node.left, node.right));
  }

  void visit(DivideExpression &node) override {
    push(wrapBinary("/", node.left, node.right));
  }

  void visit(ModuloExpression &node) override {
    push(wrapBinary("%", node.left, node.right));
  }

  void visit(PowerExpression &node) override {
    push(wrapBinary("^", node.left, node.right));
  }

  void visit(UnaryExpression &node) override {
    (void)node;
    push("");
  }

  void visit(NotExpression &node) override {
    auto operand = renderMaybe(node.operand);
    push(wrap(std::string("NOT ") + operand));
  }

  void visit(UnaryPlusExpression &node) override {
    auto operand = renderMaybe(node.operand);
    push(wrap(std::string("+") + operand));
  }

  void visit(UnaryMinusExpression &node) override {
    auto operand = renderMaybe(node.operand);
    push(wrap(std::string("-") + operand));
  }

  void visit(StringPredicateExpression &node) override {
    auto left = renderMaybe(node.left);
    auto right = renderMaybe(node.right);
    push(wrap(left + " " + node.op + " " + right));
  }

  void visit(ListPredicateExpression &node) override {
    auto element = renderMaybe(node.element);
    auto list = renderMaybe(node.list);
    push(wrap(element + " IN " + list));
  }

  void visit(LabelPredicateExpression &node) override {
    auto expr = renderMaybe(node.expr);
    std::ostringstream oss;
    oss << expr;
    for (const auto &label : node.labels) {
      oss << ":" << renderSymbolicName(label);
    }
    push(oss.str());
  }

  void visit(NullPredicateExpression &node) override {
    auto operand = renderMaybe(node.operand);
    if (node.is_null) {
      push(wrap(operand + " IS NULL"));
    } else {
      push(wrap(operand + " IS NOT NULL"));
    }
  }

  void visit(Literal &node) override {
    (void)node;
    push("");
  }

  void visit(BooleanLiteral &node) override {
    push(node.value ? "true" : "false");
  }

  void visit(IntegerLiteral &node) override {
    push(std::to_string(node.value));
  }

  void visit(DoubleLiteral &node) override {
    std::ostringstream oss;
    oss << node.value;
    push(oss.str());
  }

  void visit(StringLiteral &node) override {
    std::string out = "'";
    out += escapeStringLiteral(node.value);
    out += "'";
    push(out);
  }

  void visit(NullLiteral &node) override {
    (void)node;
    push("NULL");
  }

  void visit(ListLiteral &node) override {
    std::string out = "[";
    out += join(renderList(node.elements), ", ");
    out += "]";
    push(out);
  }

  void visit(MapLiteral &node) override {
    std::vector<std::string> entries;
    entries.reserve(node.entries.size());
    for (const auto &entry : node.entries) {
      std::string item = renderSymbolicName(entry.first);
      item += ": ";
      item += renderMaybe(entry.second);
      entries.push_back(std::move(item));
    }
    std::string out = "{";
    out += join(entries, ", ");
    out += "}";
    push(out);
  }

  void visit(Properties &node) override {
    if (node.map) {
      push(renderMaybe(node.map));
      return;
    }
    push(renderMaybe(node.parameter));
  }

  void visit(Variable &node) override { push(renderSymbolicName(node.name)); }

  void visit(Parameter &node) override {
    if (isDecimal(node.name)) {
      push("$" + node.name);
      return;
    }
    push("$" + renderSymbolicName(node.name));
  }

  void visit(PropertyExpression &node) override {
    auto object = renderMaybe(node.object);
    std::string out = object;
    out += ".";
    out += renderSymbolicName(node.property_key);
    push(out);
  }

  void visit(ListIndexExpression &node) override {
    auto list = renderMaybe(node.list);
    auto index = renderMaybe(node.index);
    std::string out = list + "[" + index + "]";
    push(out);
  }

  void visit(ListSliceExpression &node) override {
    auto list = renderMaybe(node.list);
    auto start = renderMaybe(node.start_index);
    auto end = renderMaybe(node.end_index);
    std::string out = list + "[" + start + ".." + end + "]";
    push(out);
  }

  void visit(FunctionInvocation &node) override {
    std::string out = renderQualifiedName(node.function_name);
    out += "(";
    if (node.distinct) {
      out += "DISTINCT ";
    }
    out += join(renderList(node.arguments), ", ");
    out += ")";
    push(out);
  }

  void visit(CountStarExpression &node) override {
    (void)node;
    push("count(*)");
  }

  void visit(CaseExpression &node) override {
    std::ostringstream oss;
    oss << "CASE";
    if (node.test) {
      oss << " " << renderMaybe(node.test);
    }
    for (const auto &alt : node.alternatives) {
      oss << " WHEN " << renderMaybe(alt.first);
      oss << " THEN " << renderMaybe(alt.second);
    }
    if (node.else_expr) {
      oss << " ELSE " << renderMaybe(node.else_expr);
    }
    oss << " END";
    push(oss.str());
  }

  void visit(ParenthesizedExpression &node) override {
    push(wrap(renderMaybe(node.expr)));
  }

  void visit(ListComprehension &node) override {
    std::string out = "[";
    out +=
        renderFilterExpression(node.variable, node.list_expr, node.where_expr);
    if (node.eval_expr) {
      out += " | ";
      out += renderMaybe(node.eval_expr);
    }
    out += "]";
    push(out);
  }

  void visit(PatternComprehension &node) override {
    std::string out = "[";
    if (!node.variable.empty()) {
      out += renderSymbolicName(node.variable);
      out += " = ";
    }
    out += renderMaybe(node.relationships_pattern);
    if (node.where_expr) {
      out += " WHERE ";
      out += renderMaybe(node.where_expr);
    }
    out += " | ";
    out += renderMaybe(node.eval_expr);
    out += "]";
    push(out);
  }

  void visit(PatternPredicateExpression &node) override {
    push(renderMaybe(node.relationships_pattern));
  }

  void visit(Quantifier &node) override {
    push(renderFilterExpression(node.variable, node.list_expr, node.predicate));
  }

  void visit(AllQuantifier &node) override {
    push(renderQuantifier("ALL", node));
  }

  void visit(AnyQuantifier &node) override {
    push(renderQuantifier("ANY", node));
  }

  void visit(NoneQuantifier &node) override {
    push(renderQuantifier("NONE", node));
  }

  void visit(SingleQuantifier &node) override {
    push(renderQuantifier("SINGLE", node));
  }

  void visit(ExistentialSubquery &node) override {
    std::string out = "EXISTS { ";
    if (node.query) {
      out += renderMaybe(node.query);
    } else {
      out += renderMaybe(node.pattern);
      if (node.where_expr) {
        out += " WHERE ";
        out += renderMaybe(node.where_expr);
      }
    }
    out += " }";
    push(out);
  }

  void visit(Pattern &node) override {
    std::string out = join(renderList(node.parts), ", ");
    push(out);
  }

  void visit(PatternPart &node) override {
    auto element = renderMaybe(node.element);
    if (node.variable.empty()) {
      push(element);
      return;
    }
    std::string out = renderSymbolicName(node.variable);
    out += " = ";
    out += element;
    push(out);
  }

  void visit(PatternElement &node) override {
    std::ostringstream oss;
    oss << renderMaybe(node.node_pattern);
    for (const auto &entry : node.chain) {
      oss << renderMaybe(entry.first);
      oss << renderMaybe(entry.second);
    }
    push(oss.str());
  }

  void visit(RelationshipsPattern &node) override {
    std::ostringstream oss;
    oss << renderMaybe(node.node_pattern);
    for (const auto &entry : node.chain) {
      oss << renderMaybe(entry.first);
      oss << renderMaybe(entry.second);
    }
    push(oss.str());
  }

  void visit(NodePattern &node) override {
    std::string out = "(";
    std::string inside;
    if (!node.variable.empty()) {
      inside += renderSymbolicName(node.variable);
    }
    for (const auto &label : node.labels) {
      inside += ":";
      inside += renderSymbolicName(label);
    }
    if (node.properties) {
      if (!inside.empty()) {
        inside += " ";
      }
      inside += renderMaybe(node.properties);
    }
    out += inside;
    out += ")";
    push(out);
  }

  void visit(RelationshipPattern &node) override {
    std::string out;
    out += node.left_arrow ? "<-" : "-";
    if (node.detail) {
      out += "[";
      out += renderMaybe(node.detail);
      out += "]";
    }
    out += node.right_arrow ? "->" : "-";
    push(out);
  }

  void visit(RelationshipDetail &node) override {
    std::string out;
    if (!node.variable.empty()) {
      out += renderSymbolicName(node.variable);
    }
    if (!node.types.empty()) {
      out += ":";
      for (size_t i = 0; i < node.types.size(); ++i) {
        if (i > 0) {
          out += "|";
        }
        out += renderSymbolicName(node.types[i]);
      }
    }
    if (node.range) {
      out += "*";
      if (node.range->min) {
        out += std::to_string(*node.range->min);
      }
      out += "..";
      if (node.range->max) {
        out += std::to_string(*node.range->max);
      }
    }
    if (node.properties) {
      if (!out.empty()) {
        out += " ";
      }
      out += renderMaybe(node.properties);
    }
    push(out);
  }

  void visit(Clause &node) override {
    (void)node;
    push("");
  }

  void visit(ReadingClause &node) override {
    (void)node;
    push("");
  }

  void visit(Match &node) override {
    std::string out = node.optional_match ? "OPTIONAL MATCH " : "MATCH ";
    out += renderMaybe(node.pattern);
    if (node.where) {
      out += " WHERE ";
      out += renderMaybe(node.where);
    }
    push(out);
  }

  void visit(Unwind &node) override {
    std::string out = "UNWIND ";
    out += renderMaybe(node.expression);
    out += " AS ";
    out += renderSymbolicName(node.variable);
    push(out);
  }

  void visit(InQueryCall &node) override {
    std::string out = "CALL ";
    out += renderQualifiedName(node.procedure_name);
    out += "(";
    out += join(renderList(node.arguments), ", ");
    out += ")";
    if (!node.yield_items.empty()) {
      out += " YIELD ";
      out += join(renderYieldItems(node.yield_items), ", ");
      if (node.yield_where) {
        out += " WHERE ";
        out += renderMaybe(node.yield_where);
      }
    }
    push(out);
  }

  void visit(UpdatingClause &node) override {
    (void)node;
    push("");
  }

  void visit(Create &node) override {
    std::string out = "CREATE ";
    out += renderMaybe(node.pattern);
    push(out);
  }

  void visit(Merge &node) override {
    std::string out = "MERGE ";
    out += renderMaybe(node.pattern_part);
    for (const auto &action : node.actions) {
      out += action.first ? " ON MATCH " : " ON CREATE ";
      out += renderMaybe(action.second);
    }
    push(out);
  }

  void visit(Delete &node) override {
    std::string out = node.detach ? "DETACH DELETE " : "DELETE ";
    out += join(renderList(node.expressions), ", ");
    push(out);
  }

  void visit(Set &node) override {
    std::string out = "SET ";
    out += join(renderList(node.items), ", ");
    push(out);
  }

  void visit(SetItem &node) override {
    std::string target = renderMaybe(node.target);
    std::string out;
    switch (node.type) {
      case SetItem::Type::Property:
        out = target + " = " + renderMaybe(node.value);
        break;
      case SetItem::Type::Variable:
        out = target + (node.plus_equal ? " += " : " = ") +
              renderMaybe(node.value);
        break;
      case SetItem::Type::Labels:
        out = target;
        for (const auto &label : node.labels) {
          out += ":";
          out += renderSymbolicName(label);
        }
        break;
    }
    push(out);
  }

  void visit(Remove &node) override {
    std::string out = "REMOVE ";
    out += join(renderList(node.items), ", ");
    push(out);
  }

  void visit(RemoveItem &node) override {
    std::string target = renderMaybe(node.target);
    std::string out;
    switch (node.type) {
      case RemoveItem::Type::Property:
        out = target;
        break;
      case RemoveItem::Type::Labels:
        out = target;
        for (const auto &label : node.labels) {
          out += ":";
          out += renderSymbolicName(label);
        }
        break;
    }
    push(out);
  }

  void visit(ProjectionClause &node) override {
    (void)node;
    push("");
  }

  void visit(ProjectionBody &node) override {
    std::string out;
    if (node.distinct) {
      out += "DISTINCT ";
    }
    if (node.star) {
      out += "*";
    } else {
      out += join(renderList(node.items), ", ");
    }
    if (!node.order_by.empty()) {
      out += " ORDER BY ";
      out += join(renderList(node.order_by), ", ");
    }
    if (node.skip) {
      out += " SKIP ";
      out += renderMaybe(node.skip);
    }
    if (node.limit) {
      out += " LIMIT ";
      out += renderMaybe(node.limit);
    }
    push(out);
  }

  void visit(ProjectionItem &node) override {
    std::string out = renderMaybe(node.expression);
    if (!node.alias.empty()) {
      out += " AS ";
      out += renderSymbolicName(node.alias);
    }
    push(out);
  }

  void visit(SortItem &node) override {
    std::string out = renderMaybe(node.expression);
    out += node.ascending ? " ASC" : " DESC";
    push(out);
  }

  void visit(With &node) override {
    std::string out = "WITH ";
    out += renderMaybe(node.body);
    if (node.where) {
      out += " WHERE ";
      out += renderMaybe(node.where);
    }
    push(out);
  }

  void visit(Return &node) override {
    std::string out = "RETURN ";
    out += renderMaybe(node.body);
    push(out);
  }

 private:
  std::vector<std::string> stack_;

  void push(std::string value) { stack_.push_back(std::move(value)); }

  std::string pop() {
    assert(!stack_.empty());
    std::string out = std::move(stack_.back());
    stack_.pop_back();
    return out;
  }

  template <typename T>
  std::string renderMaybe(const std::unique_ptr<T> &ptr) {
    if (!ptr) {
      return {};
    }
    ptr->accept(*this);
    return pop();
  }

  template <typename T>
  std::vector<std::string> renderList(
      const std::vector<std::unique_ptr<T>> &items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const auto &item : items) {
      auto rendered = renderMaybe(item);
      if (!rendered.empty()) {
        out.push_back(std::move(rendered));
      }
    }
    return out;
  }

  template <typename T>
  void appendClauses(std::vector<std::string> &out,
                     const std::vector<std::unique_ptr<T>> &clauses) {
    for (const auto &clause : clauses) {
      auto rendered = renderMaybe(clause);
      if (!rendered.empty()) {
        out.push_back(std::move(rendered));
      }
    }
  }

  std::vector<std::string> renderYieldItems(
      const std::vector<StandaloneCall::YieldItem> &items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const auto &item : items) {
      std::string rendered;
      if (item.result_field) {
        rendered += renderSymbolicName(*item.result_field);
        rendered += " AS ";
      }
      rendered += renderSymbolicName(item.variable);
      out.push_back(std::move(rendered));
    }
    return out;
  }

  static std::string join(const std::vector<std::string> &items,
                          const std::string &sep) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
      if (i > 0) {
        oss << sep;
      }
      oss << items[i];
    }
    return oss.str();
  }

  static std::string wrap(const std::string &text) { return "(" + text + ")"; }

  std::string wrapBinary(const std::string &op,
                         const std::unique_ptr<Expression> &left,
                         const std::unique_ptr<Expression> &right) {
    auto lhs = renderMaybe(left);
    auto rhs = renderMaybe(right);
    return wrap(lhs + " " + op + " " + rhs);
  }

  std::string renderFilterExpression(const std::string &variable,
                                     const std::unique_ptr<Expression> &list,
                                     const std::unique_ptr<Expression> &where) {
    std::string out = renderSymbolicName(variable);
    out += " IN ";
    out += renderMaybe(list);
    if (where) {
      out += " WHERE ";
      out += renderMaybe(where);
    }
    return out;
  }

  std::string renderQuantifier(const std::string &keyword,
                               const Quantifier &node) {
    std::string out = keyword;
    out += "(";
    out +=
        renderFilterExpression(node.variable, node.list_expr, node.predicate);
    out += ")";
    return out;
  }

  static bool isAsciiLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }

  static bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

  static bool isSimpleSymbolicName(const std::string &name) {
    if (name.empty()) {
      return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!(isAsciiLetter(static_cast<char>(first)) || first == '_')) {
      return false;
    }
    for (char c : name) {
      if (static_cast<unsigned char>(c) >= 0x80) {
        return false;
      }
      if (!(isAsciiLetter(c) || isAsciiDigit(c) || c == '_')) {
        return false;
      }
    }
    return true;
  }

  static std::string escapeSymbolicName(const std::string &name) {
    if (isSimpleSymbolicName(name)) {
      return name;
    }
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('`');
    for (char c : name) {
      if (c == '`') {
        out.push_back('`');
      }
      out.push_back(c);
    }
    out.push_back('`');
    return out;
  }

  static std::string renderSymbolicName(const std::string &name) {
    return escapeSymbolicName(name);
  }

  static std::string renderQualifiedName(const std::string &name) {
    if (name.empty()) {
      return {};
    }
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
      if (i == name.size() || name[i] == '.') {
        parts.push_back(name.substr(start, i - start));
        start = i + 1;
      }
    }
    for (auto &part : parts) {
      part = renderSymbolicName(part);
    }
    return join(parts, ".");
  }

  static bool isDecimal(const std::string &text) {
    if (text.empty()) {
      return false;
    }
    for (char c : text) {
      if (!isAsciiDigit(c)) {
        return false;
      }
    }
    return true;
  }

  static std::string escapeStringLiteral(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
      switch (c) {
        case '\\':
          out += "\\\\";
          break;
        case '\'':
          out += "\\'";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        case '\b':
          out += "\\b";
          break;
        case '\f':
          out += "\\f";
          break;
        default: {
          unsigned char uc = static_cast<unsigned char>(c);
          if (uc < 0x20) {
            char buf[7];
            std::snprintf(buf, sizeof(buf), "\\u%04X", uc);
            out += buf;
          } else {
            out.push_back(c);
          }
          break;
        }
      }
    }
    return out;
  }
};

}  // namespace

std::string toCypher(ASTNode &node) {
  CypherPrinter printer;
  return printer.print(node);
}

}  // namespace ast
