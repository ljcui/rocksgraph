#include "ast_to_cypher.h"

#include <array>
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
  std::string Print(ASTNode &node) {
    stack_.clear();
    node.Accept(*this);
    if (stack_.empty()) {
      return {};
    }
    std::string out = std::move(stack_.back());
    stack_.pop_back();
    return out;
  }

  void Visit(Statement &node) override {
    (void)node;
    Push("");
  }

  void Visit(Query &node) override {
    (void)node;
    Push("");
  }

  void Visit(RegularQuery &node) override {
    std::vector<std::string> parts;
    auto main = RenderMaybe(node.single_query);
    if (!main.empty()) {
      parts.push_back(std::move(main));
    }
    for (const auto &part : node.unions) {
      auto piece = RenderMaybe(part);
      if (!piece.empty()) {
        parts.push_back(std::move(piece));
      }
    }
    Push(Join(parts, " "));
  }

  void Visit(StandaloneCall &node) override {
    std::string out = "CALL ";
    out += RenderQualifiedName(node.procedure_name);
    out += "(";
    out += Join(RenderList(node.arguments), ", ");
    out += ")";
    if (node.yield_star) {
      out += " YIELD *";
    } else if (!node.yield_items.empty()) {
      out += " YIELD ";
      out += Join(RenderYieldItems(node.yield_items), ", ");
      if (node.yield_where) {
        out += " WHERE ";
        out += RenderMaybe(node.yield_where);
      }
    }
    Push(out);
  }

  void Visit(SingleQuery &node) override {
    (void)node;
    Push("");
  }

  void Visit(SinglePartQuery &node) override {
    std::vector<std::string> clauses;
    AppendClauses(clauses, node.reading_clauses);
    AppendClauses(clauses, node.updating_clauses);
    auto ret = RenderMaybe(node.return_clause);
    if (!ret.empty()) {
      clauses.push_back(std::move(ret));
    }
    Push(Join(clauses, " "));
  }

  void Visit(MultiPartQuery &node) override {
    std::vector<std::string> clauses;
    for (const auto &part : node.parts) {
      std::vector<std::string> with_part;
      AppendClauses(with_part, part.reading_clauses);
      AppendClauses(with_part, part.updating_clauses);
      auto with_clause = RenderMaybe(part.with_clause);
      if (!with_clause.empty()) {
        with_part.push_back(std::move(with_clause));
      }
      if (!with_part.empty()) {
        clauses.push_back(Join(with_part, " "));
      }
    }
    auto final_part = RenderMaybe(node.final_single_part_query);
    if (!final_part.empty()) {
      clauses.push_back(std::move(final_part));
    }
    Push(Join(clauses, " "));
  }

  void Visit(UnionPart &node) override {
    std::string out = "UNION";
    if (node.all) {
      out += " ALL";
    }
    auto query = RenderMaybe(node.query);
    if (!query.empty()) {
      out += " ";
      out += std::move(query);
    }
    Push(out);
  }

  void Visit(Expression &node) override {
    (void)node;
    Push("");
  }

  void Visit(BinaryExpression &node) override {
    (void)node;
    Push("");
  }

  void Visit(OrExpression &node) override {
    Push(WrapBinary("OR", node.left, node.right));
  }

  void Visit(XorExpression &node) override {
    Push(WrapBinary("XOR", node.left, node.right));
  }

  void Visit(AndExpression &node) override {
    Push(WrapBinary("AND", node.left, node.right));
  }

  void Visit(ComparisonExpression &node) override {
    auto left = RenderMaybe(node.left);
    auto right = RenderMaybe(node.right);
    Push(Wrap(left + " " + node.op + " " + right));
  }

  void Visit(ComparisonChainExpression &node) override {
    auto left = RenderMaybe(node.left);
    std::ostringstream oss;
    oss << left;
    for (const auto &entry : node.rights) {
      oss << " " << entry.first << " " << RenderMaybe(entry.second);
    }
    Push(Wrap(oss.str()));
  }

  void Visit(AddExpression &node) override {
    Push(WrapBinary("+", node.left, node.right));
  }

  void Visit(SubtractExpression &node) override {
    Push(WrapBinary("-", node.left, node.right));
  }

  void Visit(MultiplyExpression &node) override {
    Push(WrapBinary("*", node.left, node.right));
  }

  void Visit(DivideExpression &node) override {
    Push(WrapBinary("/", node.left, node.right));
  }

  void Visit(ModuloExpression &node) override {
    Push(WrapBinary("%", node.left, node.right));
  }

  void Visit(PowerExpression &node) override {
    Push(WrapBinary("^", node.left, node.right));
  }

  void Visit(UnaryExpression &node) override {
    (void)node;
    Push("");
  }

  void Visit(NotExpression &node) override {
    auto operand = RenderMaybe(node.operand);
    Push(Wrap(std::string("NOT ") + operand));
  }

  void Visit(UnaryPlusExpression &node) override {
    auto operand = RenderMaybe(node.operand);
    Push(Wrap(std::string("+") + operand));
  }

  void Visit(UnaryMinusExpression &node) override {
    auto operand = RenderMaybe(node.operand);
    Push(Wrap(std::string("-") + operand));
  }

  void Visit(StringPredicateExpression &node) override {
    auto left = RenderMaybe(node.left);
    auto right = RenderMaybe(node.right);
    Push(Wrap(left + " " + node.op + " " + right));
  }

  void Visit(ListPredicateExpression &node) override {
    auto element = RenderMaybe(node.element);
    auto list = RenderMaybe(node.list);
    Push(Wrap(element + " IN " + list));
  }

  void Visit(LabelPredicateExpression &node) override {
    auto expr = RenderMaybe(node.expr);
    std::ostringstream oss;
    oss << expr;
    for (const auto &label : node.labels) {
      oss << ":" << RenderSymbolicName(label);
    }
    Push(oss.str());
  }

  void Visit(NullPredicateExpression &node) override {
    auto operand = RenderMaybe(node.operand);
    if (node.is_null) {
      Push(Wrap(operand + " IS NULL"));
    } else {
      Push(Wrap(operand + " IS NOT NULL"));
    }
  }

  void Visit(Literal &node) override {
    (void)node;
    Push("");
  }

  void Visit(BooleanLiteral &node) override {
    Push(node.value ? "true" : "false");
  }

  void Visit(IntegerLiteral &node) override {
    Push(std::to_string(node.value));
  }

  void Visit(DoubleLiteral &node) override {
    std::ostringstream oss;
    oss << node.value;
    Push(oss.str());
  }

  void Visit(StringLiteral &node) override {
    std::string out = "'";
    out += EscapeStringLiteral(node.value);
    out += "'";
    Push(out);
  }

  void Visit(NullLiteral &node) override {
    (void)node;
    Push("NULL");
  }

  void Visit(ListLiteral &node) override {
    std::string out = "[";
    out += Join(RenderList(node.elements), ", ");
    out += "]";
    Push(out);
  }

  void Visit(MapLiteral &node) override {
    std::vector<std::string> entries;
    entries.reserve(node.entries.size());
    for (const auto &entry : node.entries) {
      std::string item = RenderSymbolicName(entry.first);
      item += ": ";
      item += RenderMaybe(entry.second);
      entries.push_back(std::move(item));
    }
    std::string out = "{";
    out += Join(entries, ", ");
    out += "}";
    Push(out);
  }

  void Visit(Properties &node) override {
    if (node.map) {
      Push(RenderMaybe(node.map));
      return;
    }
    Push(RenderMaybe(node.parameter));
  }

  void Visit(Variable &node) override { Push(RenderSymbolicName(node.name)); }

  void Visit(Parameter &node) override {
    if (IsDecimal(node.name)) {
      Push("$" + node.name);
      return;
    }
    Push("$" + RenderSymbolicName(node.name));
  }

  void Visit(PropertyExpression &node) override {
    auto object = RenderMaybe(node.object);
    std::string out = object;
    out += ".";
    out += RenderSymbolicName(node.property_key);
    Push(out);
  }

  void Visit(ListIndexExpression &node) override {
    auto list = RenderMaybe(node.list);
    auto index = RenderMaybe(node.index);
    std::string out = list + "[" + index + "]";
    Push(out);
  }

  void Visit(ListSliceExpression &node) override {
    auto list = RenderMaybe(node.list);
    auto start = RenderMaybe(node.start_index);
    auto end = RenderMaybe(node.end_index);
    std::string out = list + "[" + start + ".." + end + "]";
    Push(out);
  }

  void Visit(FunctionInvocation &node) override {
    std::string out = RenderQualifiedName(node.function_name);
    out += "(";
    if (node.distinct) {
      out += "DISTINCT ";
    }
    out += Join(RenderList(node.arguments), ", ");
    out += ")";
    Push(out);
  }

  void Visit(CountStarExpression &node) override {
    (void)node;
    Push("count(*)");
  }

  void Visit(CaseExpression &node) override {
    std::ostringstream oss;
    oss << "CASE";
    if (node.test) {
      oss << " " << RenderMaybe(node.test);
    }
    for (const auto &alt : node.alternatives) {
      oss << " WHEN " << RenderMaybe(alt.first);
      oss << " THEN " << RenderMaybe(alt.second);
    }
    if (node.else_expr) {
      oss << " ELSE " << RenderMaybe(node.else_expr);
    }
    oss << " END";
    Push(oss.str());
  }

  void Visit(ParenthesizedExpression &node) override {
    Push(Wrap(RenderMaybe(node.expr)));
  }

  void Visit(ListComprehension &node) override {
    std::string out = "[";
    out +=
        RenderFilterExpression(node.variable, node.list_expr, node.where_expr);
    if (node.eval_expr) {
      out += " | ";
      out += RenderMaybe(node.eval_expr);
    }
    out += "]";
    Push(out);
  }

  void Visit(PatternComprehension &node) override {
    std::string out = "[";
    if (!node.variable.empty()) {
      out += RenderSymbolicName(node.variable);
      out += " = ";
    }
    out += RenderMaybe(node.relationships_pattern);
    if (node.where_expr) {
      out += " WHERE ";
      out += RenderMaybe(node.where_expr);
    }
    out += " | ";
    out += RenderMaybe(node.eval_expr);
    out += "]";
    Push(out);
  }

  void Visit(PatternPredicateExpression &node) override {
    Push(RenderMaybe(node.relationships_pattern));
  }

  void Visit(Quantifier &node) override {
    Push(RenderFilterExpression(node.variable, node.list_expr, node.predicate));
  }

  void Visit(AllQuantifier &node) override {
    Push(RenderQuantifier("ALL", node));
  }

  void Visit(AnyQuantifier &node) override {
    Push(RenderQuantifier("ANY", node));
  }

  void Visit(NoneQuantifier &node) override {
    Push(RenderQuantifier("NONE", node));
  }

  void Visit(SingleQuantifier &node) override {
    Push(RenderQuantifier("SINGLE", node));
  }

  void Visit(ExistentialSubquery &node) override {
    std::string out = "EXISTS { ";
    if (node.query) {
      out += RenderMaybe(node.query);
    } else {
      out += RenderMaybe(node.pattern);
      if (node.where_expr) {
        out += " WHERE ";
        out += RenderMaybe(node.where_expr);
      }
    }
    out += " }";
    Push(out);
  }

  void Visit(Pattern &node) override {
    std::string out = Join(RenderList(node.parts), ", ");
    Push(out);
  }

  void Visit(PatternPart &node) override {
    auto element = RenderMaybe(node.element);
    if (node.variable.empty()) {
      Push(element);
      return;
    }
    std::string out = RenderSymbolicName(node.variable);
    out += " = ";
    out += element;
    Push(out);
  }

  void Visit(PatternElement &node) override {
    std::ostringstream oss;
    oss << RenderMaybe(node.node_pattern);
    for (const auto &entry : node.chain) {
      oss << RenderMaybe(entry.first);
      oss << RenderMaybe(entry.second);
    }
    Push(oss.str());
  }

  void Visit(RelationshipsPattern &node) override {
    std::ostringstream oss;
    oss << RenderMaybe(node.node_pattern);
    for (const auto &entry : node.chain) {
      oss << RenderMaybe(entry.first);
      oss << RenderMaybe(entry.second);
    }
    Push(oss.str());
  }

  void Visit(NodePattern &node) override {
    std::string out = "(";
    std::string inside;
    if (!node.variable.empty()) {
      inside += RenderSymbolicName(node.variable);
    }
    for (const auto &label : node.labels) {
      inside += ":";
      inside += RenderSymbolicName(label);
    }
    if (node.properties) {
      if (!inside.empty()) {
        inside += " ";
      }
      inside += RenderMaybe(node.properties);
    }
    out += inside;
    out += ")";
    Push(out);
  }

  void Visit(RelationshipPattern &node) override {
    std::string out;
    out += node.left_arrow ? "<-" : "-";
    if (node.detail) {
      out += "[";
      out += RenderMaybe(node.detail);
      out += "]";
    }
    out += node.right_arrow ? "->" : "-";
    Push(out);
  }

  void Visit(RelationshipDetail &node) override {
    std::string out;
    if (!node.variable.empty()) {
      out += RenderSymbolicName(node.variable);
    }
    if (!node.types.empty()) {
      out += ":";
      for (size_t i = 0; i < node.types.size(); ++i) {
        if (i > 0) {
          out += "|";
        }
        out += RenderSymbolicName(node.types[i]);
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
      out += RenderMaybe(node.properties);
    }
    Push(out);
  }

  void Visit(Clause &node) override {
    (void)node;
    Push("");
  }

  void Visit(ReadingClause &node) override {
    (void)node;
    Push("");
  }

  void Visit(Match &node) override {
    std::string out = node.optional_match ? "OPTIONAL MATCH " : "MATCH ";
    out += RenderMaybe(node.pattern);
    if (node.where) {
      out += " WHERE ";
      out += RenderMaybe(node.where);
    }
    Push(out);
  }

  void Visit(Unwind &node) override {
    std::string out = "UNWIND ";
    out += RenderMaybe(node.expression);
    out += " AS ";
    out += RenderSymbolicName(node.variable);
    Push(out);
  }

  void Visit(InQueryCall &node) override {
    std::string out = "CALL ";
    out += RenderQualifiedName(node.procedure_name);
    out += "(";
    out += Join(RenderList(node.arguments), ", ");
    out += ")";
    if (!node.yield_items.empty()) {
      out += " YIELD ";
      out += Join(RenderYieldItems(node.yield_items), ", ");
      if (node.yield_where) {
        out += " WHERE ";
        out += RenderMaybe(node.yield_where);
      }
    }
    Push(out);
  }

  void Visit(UpdatingClause &node) override {
    (void)node;
    Push("");
  }

  void Visit(Create &node) override {
    std::string out = "CREATE ";
    out += RenderMaybe(node.pattern);
    Push(out);
  }

  void Visit(Merge &node) override {
    std::string out = "MERGE ";
    out += RenderMaybe(node.pattern_part);
    for (const auto &action : node.actions) {
      out += action.first ? " ON MATCH " : " ON CREATE ";
      out += RenderMaybe(action.second);
    }
    Push(out);
  }

  void Visit(Delete &node) override {
    std::string out = node.detach ? "DETACH DELETE " : "DELETE ";
    out += Join(RenderList(node.expressions), ", ");
    Push(out);
  }

  void Visit(Set &node) override {
    std::string out = "SET ";
    out += Join(RenderList(node.items), ", ");
    Push(out);
  }

  void Visit(SetItem &node) override {
    std::string target = RenderMaybe(node.target);
    std::string out;
    switch (node.type) {
      case SetItem::Type::kProperty:
        out = target + " = " + RenderMaybe(node.value);
        break;
      case SetItem::Type::kVariable:
        out = target + (node.plus_equal ? " += " : " = ") +
              RenderMaybe(node.value);
        break;
      case SetItem::Type::kLabels:
        out = target;
        for (const auto &label : node.labels) {
          out += ":";
          out += RenderSymbolicName(label);
        }
        break;
    }
    Push(out);
  }

  void Visit(Remove &node) override {
    std::string out = "REMOVE ";
    out += Join(RenderList(node.items), ", ");
    Push(out);
  }

  void Visit(RemoveItem &node) override {
    std::string target = RenderMaybe(node.target);
    std::string out;
    switch (node.type) {
      case RemoveItem::Type::kProperty:
        out = target;
        break;
      case RemoveItem::Type::kLabels:
        out = target;
        for (const auto &label : node.labels) {
          out += ":";
          out += RenderSymbolicName(label);
        }
        break;
    }
    Push(out);
  }

  void Visit(ProjectionClause &node) override {
    (void)node;
    Push("");
  }

  void Visit(ProjectionBody &node) override {
    std::string out;
    if (node.distinct) {
      out += "DISTINCT ";
    }
    if (node.star || node.empty_star_expansion) {
      out += "*";
    } else {
      out += Join(RenderList(node.items), ", ");
    }
    if (!node.order_by.empty()) {
      out += " ORDER BY ";
      out += Join(RenderList(node.order_by), ", ");
    }
    if (node.skip) {
      out += " SKIP ";
      out += RenderMaybe(node.skip);
    }
    if (node.limit) {
      out += " LIMIT ";
      out += RenderMaybe(node.limit);
    }
    Push(out);
  }

  void Visit(ProjectionItem &node) override {
    std::string out = RenderMaybe(node.expression);
    if (!node.alias.empty()) {
      out += " AS ";
      out += RenderSymbolicName(node.alias);
    }
    Push(out);
  }

  void Visit(SortItem &node) override {
    std::string out = RenderMaybe(node.expression);
    out += node.ascending ? " ASC" : " DESC";
    Push(out);
  }

  void Visit(With &node) override {
    std::string out = "WITH ";
    out += RenderMaybe(node.body);
    if (node.where) {
      out += " WHERE ";
      out += RenderMaybe(node.where);
    }
    Push(out);
  }

  void Visit(Return &node) override {
    std::string out = "RETURN ";
    out += RenderMaybe(node.body);
    Push(out);
  }

 private:
  std::vector<std::string> stack_;

  void Push(std::string value) { stack_.push_back(std::move(value)); }

  std::string Pop() {
    assert(!stack_.empty());
    std::string out = std::move(stack_.back());
    stack_.pop_back();
    return out;
  }

  template <typename T>
  std::string RenderMaybe(const std::unique_ptr<T> &ptr) {
    if (!ptr) {
      return {};
    }
    ptr->Accept(*this);
    return Pop();
  }

  template <typename T>
  std::vector<std::string> RenderList(
      const std::vector<std::unique_ptr<T>> &items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const auto &item : items) {
      auto rendered = RenderMaybe(item);
      if (!rendered.empty()) {
        out.push_back(std::move(rendered));
      }
    }
    return out;
  }

  template <typename T>
  void AppendClauses(std::vector<std::string> &out,
                     const std::vector<std::unique_ptr<T>> &clauses) {
    for (const auto &clause : clauses) {
      auto rendered = RenderMaybe(clause);
      if (!rendered.empty()) {
        out.push_back(std::move(rendered));
      }
    }
  }

  static std::vector<std::string> RenderYieldItems(
      const std::vector<StandaloneCall::YieldItem> &items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const auto &item : items) {
      std::string rendered;
      if (item.result_field) {
        rendered += RenderSymbolicName(*item.result_field);
        rendered += " AS ";
      }
      rendered += RenderSymbolicName(item.variable);
      out.push_back(std::move(rendered));
    }
    return out;
  }

  static std::string Join(const std::vector<std::string> &items,
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

  static std::string Wrap(const std::string &text) { return "(" + text + ")"; }

  std::string WrapBinary(const std::string &op,
                         const std::unique_ptr<Expression> &left,
                         const std::unique_ptr<Expression> &right) {
    auto lhs = RenderMaybe(left);
    auto rhs = RenderMaybe(right);
    return Wrap(lhs + " " + op + " " + rhs);
  }

  std::string RenderFilterExpression(const std::string &variable,
                                     const std::unique_ptr<Expression> &list,
                                     const std::unique_ptr<Expression> &where) {
    std::string out = RenderSymbolicName(variable);
    out += " IN ";
    out += RenderMaybe(list);
    if (where) {
      out += " WHERE ";
      out += RenderMaybe(where);
    }
    return out;
  }

  std::string RenderQuantifier(const std::string &keyword,
                               const Quantifier &node) {
    std::string out = keyword;
    out += "(";
    out +=
        RenderFilterExpression(node.variable, node.list_expr, node.predicate);
    out += ")";
    return out;
  }

  static bool IsAsciiLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }

  static bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

  static bool IsSimpleSymbolicName(const std::string &name) {
    if (name.empty()) {
      return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!IsAsciiLetter(static_cast<char>(first)) && first != '_') {
      return false;
    }
    for (char c : name) {
      if (static_cast<unsigned char>(c) >= 0x80) {
        return false;
      }
      if (!IsAsciiLetter(c) && !IsAsciiDigit(c) && c != '_') {
        return false;
      }
    }
    return true;
  }

  static std::string EscapeSymbolicName(const std::string &name) {
    if (IsSimpleSymbolicName(name)) {
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

  static std::string RenderSymbolicName(const std::string &name) {
    return EscapeSymbolicName(name);
  }

  static std::string RenderQualifiedName(const std::string &name) {
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
      part = RenderSymbolicName(part);
    }
    return Join(parts, ".");
  }

  static bool IsDecimal(const std::string &text) {
    if (text.empty()) {
      return false;
    }
    for (char c : text) {
      if (!IsAsciiDigit(c)) {
        return false;
      }
    }
    return true;
  }

  static std::string EscapeStringLiteral(const std::string &value) {
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
          auto uc = static_cast<unsigned char>(c);
          if (uc < 0x20) {
            std::array<char, 7> buf{};
            std::snprintf(buf.data(), buf.size(), "\\u%04X", uc);
            out += buf.data();
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

std::string ToCypher(ASTNode &node) {
  CypherPrinter printer;
  return printer.Print(node);
}

}  // namespace ast
