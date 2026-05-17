#include "expression_to_string.h"

#include <string>
#include <utility>
#include <vector>

namespace ast {

namespace {

enum Precedence {
  kOr = 1,
  kXor = 2,
  kAnd = 3,
  kComparison = 4,
  kAdd = 5,
  kMultiply = 6,
  kPower = 7,
  kUnary = 8,
  kPostfix = 9,
  kPrimary = 10,
};

struct ExprText {
  std::string text;
  int prec = kPrimary;
};

std::string Join(const std::vector<std::string> &items,
                 const std::string &sep) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += sep;
    }
    out += items[i];
  }
  return out;
}

std::string EscapeStringLiteral(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (char c : value) {
    if (c == '\'') {
      out.push_back('\'');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

std::string WrapIfNeeded(const ExprText &child, int parent_prec,
                         bool wrap_equal) {
  if (child.text.empty()) {
    return {};
  }
  if (child.prec < parent_prec || (wrap_equal && child.prec == parent_prec)) {
    return "(" + child.text + ")";
  }
  return child.text;
}

ExprText ExpressionText(const Expression &expr);
std::string PatternToString(const Pattern &pattern);
std::string PatternPartToString(const PatternPart &part);
std::string PatternElementToString(const PatternElement &element);
std::string RelationshipsPatternToString(const RelationshipsPattern &pattern);
std::string NodePatternToString(const NodePattern &node);
std::string RelationshipPatternToString(const RelationshipPattern &pattern);
std::string RelationshipDetailToString(const RelationshipDetail &detail);
std::string PropertiesToString(const Properties &properties);
std::string ProjectionBodyToString(const ProjectionBody &body);
std::string RegularQueryToString(const RegularQuery &query);

ExprText RenderBinary(const Expression *left, const Expression *right, int prec,
                      const std::string &op, bool wrap_equal_left = false,
                      bool wrap_equal_right = true) {
  if ((left == nullptr) || (right == nullptr)) {
    return {};
  }
  const ExprText left_text = ExpressionText(*left);
  const ExprText right_text = ExpressionText(*right);
  const std::string left_rendered =
      WrapIfNeeded(left_text, prec, wrap_equal_left);
  const std::string right_rendered =
      WrapIfNeeded(right_text, prec, wrap_equal_right);
  if (left_rendered.empty() || right_rendered.empty()) {
    return {};
  }
  return {left_rendered + " " + op + " " + right_rendered, prec};
}

ExprText RenderUnary(const Expression *operand, int prec,
                     const std::string &prefix) {
  if (operand == nullptr) {
    return {};
  }
  const ExprText operand_text = ExpressionText(*operand);
  const std::string operand_rendered = WrapIfNeeded(operand_text, prec, false);
  if (operand_rendered.empty()) {
    return {};
  }
  return {prefix + operand_rendered, prec};
}

ExprText ExpressionText(const Expression &expr) {
  switch (expr.node_type) {
    case ASTNodeType::kOrExpression: {
      const auto &node = CastAst<OrExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kOr, "OR");
    }
    case ASTNodeType::kXorExpression: {
      const auto &node = CastAst<XorExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kXor, "XOR");
    }
    case ASTNodeType::kAndExpression: {
      const auto &node = CastAst<AndExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kAnd, "AND");
    }
    case ASTNodeType::kComparisonExpression: {
      const auto &node = CastAst<ComparisonExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kComparison,
                          node.op);
    }
    case ASTNodeType::kComparisonChainExpression: {
      const auto &node = CastAst<ComparisonChainExpression>(expr);
      if (!node.left || node.rights.empty()) {
        return {};
      }
      std::string out = ExpressionToString(*node.left);
      if (out.empty()) {
        return {};
      }
      for (const auto &entry : node.rights) {
        if (!entry.second) {
          return {};
        }
        const std::string right = ExpressionToString(*entry.second);
        if (right.empty()) {
          return {};
        }
        out += " " + entry.first + " " + right;
      }
      return {out, kComparison};
    }
    case ASTNodeType::kAddExpression: {
      const auto &node = CastAst<AddExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kAdd, "+");
    }
    case ASTNodeType::kSubtractExpression: {
      const auto &node = CastAst<SubtractExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kAdd, "-");
    }
    case ASTNodeType::kMultiplyExpression: {
      const auto &node = CastAst<MultiplyExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kMultiply, "*");
    }
    case ASTNodeType::kDivideExpression: {
      const auto &node = CastAst<DivideExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kMultiply, "/");
    }
    case ASTNodeType::kModuloExpression: {
      const auto &node = CastAst<ModuloExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kMultiply, "%");
    }
    case ASTNodeType::kPowerExpression: {
      const auto &node = CastAst<PowerExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kPower, "^", true,
                          false);
    }
    case ASTNodeType::kNotExpression: {
      const auto &node = CastAst<NotExpression>(expr);
      return RenderUnary(node.operand.get(), kUnary, "NOT ");
    }
    case ASTNodeType::kUnaryPlusExpression: {
      const auto &node = CastAst<UnaryPlusExpression>(expr);
      return RenderUnary(node.operand.get(), kUnary, "+");
    }
    case ASTNodeType::kUnaryMinusExpression: {
      const auto &node = CastAst<UnaryMinusExpression>(expr);
      return RenderUnary(node.operand.get(), kUnary, "-");
    }
    case ASTNodeType::kStringPredicateExpression: {
      const auto &node = CastAst<StringPredicateExpression>(expr);
      return RenderBinary(node.left.get(), node.right.get(), kComparison,
                          node.op);
    }
    case ASTNodeType::kListPredicateExpression: {
      const auto &node = CastAst<ListPredicateExpression>(expr);
      return RenderBinary(node.element.get(), node.list.get(), kComparison,
                          "IN");
    }
    case ASTNodeType::kNullPredicateExpression: {
      const auto &node = CastAst<NullPredicateExpression>(expr);
      if (!node.operand) {
        return {};
      }
      const ExprText operand = ExpressionText(*node.operand);
      const std::string operand_text =
          WrapIfNeeded(operand, kComparison, false);
      if (operand_text.empty()) {
        return {};
      }
      return {operand_text + (node.is_null ? " IS NULL" : " IS NOT NULL"),
              kComparison};
    }
    case ASTNodeType::kLabelPredicateExpression: {
      const auto &node = CastAst<LabelPredicateExpression>(expr);
      if (!node.expr || node.labels.empty()) {
        return {};
      }
      const ExprText base = ExpressionText(*node.expr);
      const std::string base_text = WrapIfNeeded(base, kPostfix, false);
      if (base_text.empty()) {
        return {};
      }
      std::string out = base_text;
      for (const auto &label : node.labels) {
        out += ":" + label;
      }
      return {out, kPostfix};
    }
    case ASTNodeType::kBooleanLiteral: {
      const auto &node = CastAst<BooleanLiteral>(expr);
      return {node.value ? "true" : "false", kPrimary};
    }
    case ASTNodeType::kIntegerLiteral: {
      const auto &node = CastAst<IntegerLiteral>(expr);
      return {std::to_string(node.value), kPrimary};
    }
    case ASTNodeType::kDoubleLiteral: {
      const auto &node = CastAst<DoubleLiteral>(expr);
      return {std::to_string(node.value), kPrimary};
    }
    case ASTNodeType::kStringLiteral: {
      const auto &node = CastAst<StringLiteral>(expr);
      return {EscapeStringLiteral(node.value), kPrimary};
    }
    case ASTNodeType::kNullLiteral:
      return {"NULL", kPrimary};
    case ASTNodeType::kListLiteral: {
      const auto &node = CastAst<ListLiteral>(expr);
      std::vector<std::string> elements;
      elements.reserve(node.elements.size());
      for (const auto &elem : node.elements) {
        if (!elem) {
          return {};
        }
        const std::string text = ExpressionToString(*elem);
        if (text.empty()) {
          return {};
        }
        elements.push_back(text);
      }
      return {"[" + Join(elements, ", ") + "]", kPrimary};
    }
    case ASTNodeType::kMapLiteral: {
      const auto &node = CastAst<MapLiteral>(expr);
      std::vector<std::string> entries;
      entries.reserve(node.entries.size());
      for (const auto &entry : node.entries) {
        if (!entry.second) {
          return {};
        }
        const std::string value = ExpressionToString(*entry.second);
        if (value.empty()) {
          return {};
        }
        entries.push_back(entry.first + ": " + value);
      }
      return {"{" + Join(entries, ", ") + "}", kPrimary};
    }
    case ASTNodeType::kVariable: {
      const auto &node = CastAst<Variable>(expr);
      return {node.name, kPrimary};
    }
    case ASTNodeType::kParameter: {
      const auto &node = CastAst<Parameter>(expr);
      if (node.name.empty()) {
        return {};
      }
      return {"$" + node.name, kPrimary};
    }
    case ASTNodeType::kPropertyExpression: {
      const auto &node = CastAst<PropertyExpression>(expr);
      if (!node.object || node.property_key.empty()) {
        return {};
      }
      const ExprText base = ExpressionText(*node.object);
      const std::string base_text = WrapIfNeeded(base, kPostfix, false);
      if (base_text.empty()) {
        return {};
      }
      return {base_text + "." + node.property_key, kPostfix};
    }
    case ASTNodeType::kListIndexExpression: {
      const auto &node = CastAst<ListIndexExpression>(expr);
      if (!node.list || !node.index) {
        return {};
      }
      const ExprText list = ExpressionText(*node.list);
      const std::string list_text = WrapIfNeeded(list, kPostfix, false);
      if (list_text.empty()) {
        return {};
      }
      const std::string index = ExpressionToString(*node.index);
      if (index.empty()) {
        return {};
      }
      return {list_text + "[" + index + "]", kPostfix};
    }
    case ASTNodeType::kListSliceExpression: {
      const auto &node = CastAst<ListSliceExpression>(expr);
      if (!node.list) {
        return {};
      }
      const ExprText list = ExpressionText(*node.list);
      const std::string list_text = WrapIfNeeded(list, kPostfix, false);
      if (list_text.empty()) {
        return {};
      }
      std::string start_text;
      std::string end_text;
      if (node.start_index) {
        start_text = ExpressionToString(*node.start_index);
        if (start_text.empty()) {
          return {};
        }
      }
      if (node.end_index) {
        end_text = ExpressionToString(*node.end_index);
        if (end_text.empty()) {
          return {};
        }
      }
      return {list_text + "[" + start_text + ".." + end_text + "]", kPostfix};
    }
    case ASTNodeType::kFunctionInvocation: {
      const auto &node = CastAst<FunctionInvocation>(expr);
      if (node.function_name.empty()) {
        return {};
      }
      std::vector<std::string> args;
      args.reserve(node.arguments.size());
      for (const auto &arg : node.arguments) {
        if (!arg) {
          return {};
        }
        const std::string arg_text = ExpressionToString(*arg);
        if (arg_text.empty()) {
          return {};
        }
        args.push_back(arg_text);
      }
      std::string arg_text = Join(args, ", ");
      if (node.distinct) {
        if (!arg_text.empty()) {
          arg_text = "DISTINCT " + arg_text;
        } else {
          arg_text = "DISTINCT";
        }
      }
      return {node.function_name + "(" + arg_text + ")", kPrimary};
    }
    case ASTNodeType::kCountStarExpression:
      return {"count(*)", kPrimary};
    case ASTNodeType::kCaseExpression: {
      const auto &node = CastAst<CaseExpression>(expr);
      std::string out = "CASE";
      if (node.test) {
        const std::string test = ExpressionToString(*node.test);
        if (test.empty()) {
          return {};
        }
        out += " " + test;
      }
      for (const auto &alt : node.alternatives) {
        if (!alt.first || !alt.second) {
          return {};
        }
        const std::string when_text = ExpressionToString(*alt.first);
        const std::string then_text = ExpressionToString(*alt.second);
        if (when_text.empty() || then_text.empty()) {
          return {};
        }
        out += " WHEN " + when_text + " THEN " + then_text;
      }
      if (node.else_expr) {
        const std::string else_text = ExpressionToString(*node.else_expr);
        if (else_text.empty()) {
          return {};
        }
        out += " ELSE " + else_text;
      }
      out += " END";
      return {out, kPrimary};
    }
    case ASTNodeType::kParenthesizedExpression: {
      const auto &node = CastAst<ParenthesizedExpression>(expr);
      if (!node.expr) {
        return {};
      }
      const std::string inner = ExpressionToString(*node.expr);
      if (inner.empty()) {
        return {};
      }
      return {"(" + inner + ")", kPrimary};
    }
    case ASTNodeType::kListComprehension: {
      const auto &node = CastAst<ListComprehension>(expr);
      if (!node.list_expr) {
        return {};
      }
      const std::string list_text = ExpressionToString(*node.list_expr);
      if (list_text.empty()) {
        return {};
      }
      std::string out = "[" + node.variable + " IN " + list_text;
      if (node.where_expr) {
        const std::string where_text = ExpressionToString(*node.where_expr);
        if (where_text.empty()) {
          return {};
        }
        out += " WHERE " + where_text;
      }
      if (node.eval_expr) {
        const std::string eval_text = ExpressionToString(*node.eval_expr);
        if (eval_text.empty()) {
          return {};
        }
        out += " | " + eval_text;
      }
      out += "]";
      return {out, kPrimary};
    }
    case ASTNodeType::kPatternComprehension: {
      const auto &node = CastAst<PatternComprehension>(expr);
      if (!node.relationships_pattern || !node.eval_expr) {
        return {};
      }
      const std::string pattern_text =
          RelationshipsPatternToString(*node.relationships_pattern);
      if (pattern_text.empty()) {
        return {};
      }
      std::string out = "[";
      if (!node.variable.empty()) {
        out += node.variable + " = ";
      }
      out += pattern_text;
      if (node.where_expr) {
        const std::string where_text = ExpressionToString(*node.where_expr);
        if (where_text.empty()) {
          return {};
        }
        out += " WHERE " + where_text;
      }
      const std::string eval_text = ExpressionToString(*node.eval_expr);
      if (eval_text.empty()) {
        return {};
      }
      out += " | " + eval_text + "]";
      return {out, kPrimary};
    }
    case ASTNodeType::kPatternPredicateExpression: {
      const auto &node = CastAst<PatternPredicateExpression>(expr);
      if (!node.relationships_pattern) {
        return {};
      }
      const std::string pattern_text =
          RelationshipsPatternToString(*node.relationships_pattern);
      if (pattern_text.empty()) {
        return {};
      }
      return {pattern_text, kPrimary};
    }
    case ASTNodeType::kAllQuantifier: {
      const auto &node = CastAst<AllQuantifier>(expr);
      if (!node.list_expr) {
        return {};
      }
      const std::string list_text = ExpressionToString(*node.list_expr);
      if (list_text.empty()) {
        return {};
      }
      std::string out = "ALL(" + node.variable + " IN " + list_text;
      if (node.predicate) {
        const std::string pred_text = ExpressionToString(*node.predicate);
        if (pred_text.empty()) {
          return {};
        }
        out += " WHERE " + pred_text;
      }
      out += ")";
      return {out, kPrimary};
    }
    case ASTNodeType::kAnyQuantifier: {
      const auto &node = CastAst<AnyQuantifier>(expr);
      if (!node.list_expr) {
        return {};
      }
      const std::string list_text = ExpressionToString(*node.list_expr);
      if (list_text.empty()) {
        return {};
      }
      std::string out = "ANY(" + node.variable + " IN " + list_text;
      if (node.predicate) {
        const std::string pred_text = ExpressionToString(*node.predicate);
        if (pred_text.empty()) {
          return {};
        }
        out += " WHERE " + pred_text;
      }
      out += ")";
      return {out, kPrimary};
    }
    case ASTNodeType::kNoneQuantifier: {
      const auto &node = CastAst<NoneQuantifier>(expr);
      if (!node.list_expr) {
        return {};
      }
      const std::string list_text = ExpressionToString(*node.list_expr);
      if (list_text.empty()) {
        return {};
      }
      std::string out = "NONE(" + node.variable + " IN " + list_text;
      if (node.predicate) {
        const std::string pred_text = ExpressionToString(*node.predicate);
        if (pred_text.empty()) {
          return {};
        }
        out += " WHERE " + pred_text;
      }
      out += ")";
      return {out, kPrimary};
    }
    case ASTNodeType::kSingleQuantifier: {
      const auto &node = CastAst<SingleQuantifier>(expr);
      if (!node.list_expr) {
        return {};
      }
      const std::string list_text = ExpressionToString(*node.list_expr);
      if (list_text.empty()) {
        return {};
      }
      std::string out = "SINGLE(" + node.variable + " IN " + list_text;
      if (node.predicate) {
        const std::string pred_text = ExpressionToString(*node.predicate);
        if (pred_text.empty()) {
          return {};
        }
        out += " WHERE " + pred_text;
      }
      out += ")";
      return {out, kPrimary};
    }
    case ASTNodeType::kExistentialSubquery: {
      const auto &node = CastAst<ExistentialSubquery>(expr);
      std::string out = "EXISTS { ";
      if (node.query) {
        const std::string query_text = RegularQueryToString(*node.query);
        if (query_text.empty()) {
          return {};
        }
        out += query_text;
        out += " }";
        return {out, kPrimary};
      }
      if (node.pattern) {
        const std::string pattern_text = PatternToString(*node.pattern);
        if (pattern_text.empty()) {
          return {};
        }
        out += pattern_text;
        if (node.where_expr) {
          const std::string where_text = ExpressionToString(*node.where_expr);
          if (where_text.empty()) {
            return {};
          }
          out += " WHERE " + where_text;
        }
        out += " }";
        return {out, kPrimary};
      }
      return {};
    }
    default:
      return {};
  }
}

std::string PropertiesToString(const Properties &properties) {
  if (properties.map) {
    return ExpressionToString(*properties.map);
  }
  if (properties.parameter) {
    if (properties.parameter->name.empty()) {
      return {};
    }
    return "$" + properties.parameter->name;
  }
  return {};
}

std::string NodePatternToString(const NodePattern &node) {
  std::string out = "(";
  if (!node.variable.empty()) {
    out += node.variable;
  }
  for (const auto &label : node.labels) {
    out += ":" + label;
  }
  if (node.properties) {
    const std::string props = PropertiesToString(*node.properties);
    if (props.empty()) {
      return {};
    }
    if (!node.variable.empty() || !node.labels.empty()) {
      out += " ";
    }
    out += props;
  }
  out += ")";
  return out;
}

std::string RelationshipDetailToString(const RelationshipDetail &detail) {
  std::string out;
  if (!detail.variable.empty()) {
    out += detail.variable;
  }
  if (!detail.types.empty()) {
    out += ":" + Join(detail.types, "|");
  }
  if (detail.range) {
    std::string range = "*";
    if (detail.range->min) {
      range += std::to_string(*detail.range->min);
    }
    if (detail.range->max || detail.range->min) {
      range += "..";
      if (detail.range->max) {
        range += std::to_string(*detail.range->max);
      }
    }
    out += range;
  }
  if (detail.properties) {
    const std::string props = PropertiesToString(*detail.properties);
    if (props.empty()) {
      return {};
    }
    if (!out.empty()) {
      out += " ";
    }
    out += props;
  }
  return "[" + out + "]";
}

std::string RelationshipPatternToString(const RelationshipPattern &pattern) {
  const std::string left = pattern.left_arrow ? "<-" : "-";
  const std::string right = pattern.right_arrow ? "->" : "-";
  std::string middle;
  if (pattern.detail) {
    middle = RelationshipDetailToString(*pattern.detail);
    if (middle.empty()) {
      return {};
    }
  }
  return left + middle + right;
}

std::string PatternElementToString(const PatternElement &element) {
  if (!element.node_pattern) {
    return {};
  }
  std::string out = NodePatternToString(*element.node_pattern);
  if (out.empty()) {
    return {};
  }
  for (const auto &link : element.chain) {
    if (!link.first || !link.second) {
      return {};
    }
    const std::string rel = RelationshipPatternToString(*link.first);
    const std::string node = NodePatternToString(*link.second);
    if (rel.empty() || node.empty()) {
      return {};
    }
    out += rel + node;
  }
  return out;
}

std::string RelationshipsPatternToString(const RelationshipsPattern &pattern) {
  if (!pattern.node_pattern) {
    return {};
  }
  std::string out = NodePatternToString(*pattern.node_pattern);
  if (out.empty()) {
    return {};
  }
  for (const auto &link : pattern.chain) {
    if (!link.first || !link.second) {
      return {};
    }
    const std::string rel = RelationshipPatternToString(*link.first);
    const std::string node = NodePatternToString(*link.second);
    if (rel.empty() || node.empty()) {
      return {};
    }
    out += rel + node;
  }
  return out;
}

std::string PatternPartToString(const PatternPart &part) {
  if (!part.element) {
    return {};
  }
  const std::string element = PatternElementToString(*part.element);
  if (element.empty()) {
    return {};
  }
  if (part.variable.empty()) {
    return element;
  }
  return part.variable + " = " + element;
}

std::string PatternToString(const Pattern &pattern) {
  std::vector<std::string> parts;
  parts.reserve(pattern.parts.size());
  for (const auto &part : pattern.parts) {
    if (!part) {
      return {};
    }
    const std::string text = PatternPartToString(*part);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  return Join(parts, ", ");
}

std::string ProjectionItemToString(const ProjectionItem &item) {
  if (!item.expression) {
    return {};
  }
  std::string out = ExpressionToString(*item.expression);
  if (out.empty()) {
    return {};
  }
  if (!item.alias.empty()) {
    out += " AS " + item.alias;
  }
  return out;
}

std::string SortItemToString(const SortItem &item) {
  if (!item.expression) {
    return {};
  }
  std::string out = ExpressionToString(*item.expression);
  if (out.empty()) {
    return {};
  }
  if (!item.ascending) {
    out += " DESC";
  }
  return out;
}

std::string ProjectionBodyToString(const ProjectionBody &body) {
  std::string items_text;
  if (body.star) {
    items_text = "*";
  }
  if (!body.items.empty()) {
    std::vector<std::string> items;
    items.reserve(body.items.size());
    for (const auto &item : body.items) {
      if (!item) {
        return {};
      }
      const std::string text = ProjectionItemToString(*item);
      if (text.empty()) {
        return {};
      }
      items.push_back(text);
    }
    if (!items_text.empty()) {
      items_text += ", ";
    }
    items_text += Join(items, ", ");
  }
  if (items_text.empty()) {
    return {};
  }
  std::string out;
  if (body.distinct) {
    out = "DISTINCT " + items_text;
  } else {
    out = items_text;
  }
  if (!body.order_by.empty()) {
    std::vector<std::string> order_items;
    order_items.reserve(body.order_by.size());
    for (const auto &item : body.order_by) {
      if (!item) {
        return {};
      }
      const std::string text = SortItemToString(*item);
      if (text.empty()) {
        return {};
      }
      order_items.push_back(text);
    }
    out += " ORDER BY " + Join(order_items, ", ");
  }
  if (body.skip) {
    const std::string skip = ExpressionToString(*body.skip);
    if (skip.empty()) {
      return {};
    }
    out += " SKIP " + skip;
  }
  if (body.limit) {
    const std::string limit = ExpressionToString(*body.limit);
    if (limit.empty()) {
      return {};
    }
    out += " LIMIT " + limit;
  }
  return out;
}

std::string SetItemToString(const SetItem &item) {
  if (!item.target) {
    return {};
  }
  const std::string target = ExpressionToString(*item.target);
  if (target.empty()) {
    return {};
  }
  switch (item.type) {
    case SetItem::Type::kProperty: {
      if (!item.value) {
        return {};
      }
      const std::string value = ExpressionToString(*item.value);
      if (value.empty()) {
        return {};
      }
      return target + " = " + value;
    }
    case SetItem::Type::kVariable: {
      if (!item.value) {
        return {};
      }
      const std::string value = ExpressionToString(*item.value);
      if (value.empty()) {
        return {};
      }
      if (item.plus_equal) {
        return target + " += " + value;
      }
      return target + " = " + value;
    }
    case SetItem::Type::kLabels: {
      if (item.labels.empty()) {
        return {};
      }
      return target + ":" + Join(item.labels, ":");
    }
  }
  return {};
}

std::string RemoveItemToString(const RemoveItem &item) {
  if (!item.target) {
    return {};
  }
  const std::string target = ExpressionToString(*item.target);
  if (target.empty()) {
    return {};
  }
  switch (item.type) {
    case RemoveItem::Type::kProperty:
      return target;
    case RemoveItem::Type::kLabels:
      if (item.labels.empty()) {
        return {};
      }
      return target + ":" + Join(item.labels, ":");
  }
  return {};
}

std::string ReadingClauseToString(const ReadingClause &clause) {
  switch (clause.node_type) {
    case ASTNodeType::kMatch: {
      const auto &match = CastAst<Match>(clause);
      if (!match.pattern) {
        return {};
      }
      const std::string pattern = PatternToString(*match.pattern);
      if (pattern.empty()) {
        return {};
      }
      std::string out = match.optional_match ? "OPTIONAL MATCH " : "MATCH ";
      out += pattern;
      if (match.where) {
        const std::string where_text = ExpressionToString(*match.where);
        if (where_text.empty()) {
          return {};
        }
        out += " WHERE " + where_text;
      }
      return out;
    }
    case ASTNodeType::kUnwind: {
      const auto &unwind = CastAst<Unwind>(clause);
      if (!unwind.expression) {
        return {};
      }
      const std::string expr = ExpressionToString(*unwind.expression);
      if (expr.empty()) {
        return {};
      }
      return "UNWIND " + expr + " AS " + unwind.variable;
    }
    case ASTNodeType::kInQueryCall: {
      const auto &call = CastAst<InQueryCall>(clause);
      std::vector<std::string> args;
      args.reserve(call.arguments.size());
      for (const auto &arg : call.arguments) {
        if (!arg) {
          return {};
        }
        const std::string text = ExpressionToString(*arg);
        if (text.empty()) {
          return {};
        }
        args.push_back(text);
      }
      std::string out =
          "CALL " + call.procedure_name + "(" + Join(args, ", ") + ")";
      if (!call.yield_items.empty()) {
        std::vector<std::string> items;
        items.reserve(call.yield_items.size());
        for (const auto &item : call.yield_items) {
          if (item.result_field) {
            items.push_back(*item.result_field + " AS " + item.variable);
          } else {
            items.push_back(item.variable);
          }
        }
        out += " YIELD " + Join(items, ", ");
      }
      if (call.yield_where) {
        const std::string where_text = ExpressionToString(*call.yield_where);
        if (where_text.empty()) {
          return {};
        }
        out += " WHERE " + where_text;
      }
      return out;
    }
    default:
      return {};
  }
}

std::string UpdatingClauseText(const UpdatingClause &clause) {
  switch (clause.node_type) {
    case ASTNodeType::kCreate: {
      const auto &create = CastAst<Create>(clause);
      if (!create.pattern) {
        return {};
      }
      const std::string pattern = PatternToString(*create.pattern);
      if (pattern.empty()) {
        return {};
      }
      return "CREATE " + pattern;
    }
    case ASTNodeType::kMerge: {
      const auto &merge = CastAst<Merge>(clause);
      if (!merge.pattern_part) {
        return {};
      }
      const std::string part = PatternPartToString(*merge.pattern_part);
      if (part.empty()) {
        return {};
      }
      std::string out = "MERGE " + part;
      for (const auto &action : merge.actions) {
        if (!action.second) {
          return {};
        }
        std::vector<std::string> items;
        items.reserve(action.second->items.size());
        for (const auto &item : action.second->items) {
          if (!item) {
            return {};
          }
          const std::string text = SetItemToString(*item);
          if (text.empty()) {
            return {};
          }
          items.push_back(text);
        }
        out += action.first ? " ON MATCH SET " : " ON CREATE SET ";
        out += Join(items, ", ");
      }
      return out;
    }
    case ASTNodeType::kDelete: {
      const auto &del = CastAst<Delete>(clause);
      if (del.expressions.empty()) {
        return {};
      }
      std::vector<std::string> items;
      items.reserve(del.expressions.size());
      for (const auto &expr : del.expressions) {
        if (!expr) {
          return {};
        }
        const std::string text = ExpressionToString(*expr);
        if (text.empty()) {
          return {};
        }
        items.push_back(text);
      }
      return std::string(del.detach ? "DETACH DELETE " : "DELETE ") +
             Join(items, ", ");
    }
    case ASTNodeType::kSet: {
      const auto &set = CastAst<Set>(clause);
      if (set.items.empty()) {
        return {};
      }
      std::vector<std::string> items;
      items.reserve(set.items.size());
      for (const auto &item : set.items) {
        if (!item) {
          return {};
        }
        const std::string text = SetItemToString(*item);
        if (text.empty()) {
          return {};
        }
        items.push_back(text);
      }
      return "SET " + Join(items, ", ");
    }
    case ASTNodeType::kRemove: {
      const auto &remove = CastAst<Remove>(clause);
      if (remove.items.empty()) {
        return {};
      }
      std::vector<std::string> items;
      items.reserve(remove.items.size());
      for (const auto &item : remove.items) {
        if (!item) {
          return {};
        }
        const std::string text = RemoveItemToString(*item);
        if (text.empty()) {
          return {};
        }
        items.push_back(text);
      }
      return "REMOVE " + Join(items, ", ");
    }
    default:
      return {};
  }
}

std::string ProjectionClauseToString(const ProjectionClause &clause) {
  if (!clause.body) {
    return {};
  }
  const std::string body = ProjectionBodyToString(*clause.body);
  if (body.empty()) {
    return {};
  }
  switch (clause.node_type) {
    case ASTNodeType::kWith: {
      const auto &with = CastAst<With>(clause);
      std::string out = "WITH " + body;
      if (with.where) {
        const std::string where_text = ExpressionToString(*with.where);
        if (where_text.empty()) {
          return {};
        }
        out += " WHERE " + where_text;
      }
      return out;
    }
    case ASTNodeType::kReturn:
      return "RETURN " + body;
    default:
      return {};
  }
}

std::string SinglePartQueryToString(const SinglePartQuery &query) {
  std::vector<std::string> parts;
  parts.reserve(query.reading_clauses.size() + query.updating_clauses.size() +
                1);
  for (const auto &clause : query.reading_clauses) {
    if (!clause) {
      return {};
    }
    const std::string text = ReadingClauseToString(*clause);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  for (const auto &clause : query.updating_clauses) {
    if (!clause) {
      return {};
    }
    const std::string text = UpdatingClauseText(*clause);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  if (query.return_clause) {
    const std::string text = ProjectionClauseToString(*query.return_clause);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  return Join(parts, " ");
}

std::string MultiPartQueryToString(const MultiPartQuery &query) {
  std::vector<std::string> parts;
  for (const auto &part : query.parts) {
    std::vector<std::string> segment;
    for (const auto &clause : part.reading_clauses) {
      if (!clause) {
        return {};
      }
      const std::string text = ReadingClauseToString(*clause);
      if (text.empty()) {
        return {};
      }
      segment.push_back(text);
    }
    for (const auto &clause : part.updating_clauses) {
      if (!clause) {
        return {};
      }
      const std::string text = UpdatingClauseText(*clause);
      if (text.empty()) {
        return {};
      }
      segment.push_back(text);
    }
    if (!part.with_clause) {
      return {};
    }
    const std::string with_text = ProjectionClauseToString(*part.with_clause);
    if (with_text.empty()) {
      return {};
    }
    segment.push_back(with_text);
    parts.push_back(Join(segment, " "));
  }
  if (!query.final_single_part_query) {
    return {};
  }
  const std::string final_part =
      SinglePartQueryToString(*query.final_single_part_query);
  if (final_part.empty()) {
    return {};
  }
  parts.push_back(final_part);
  return Join(parts, " ");
}

std::string SingleQueryToString(const SingleQuery &query) {
  switch (query.node_type) {
    case ASTNodeType::kSinglePartQuery:
      return SinglePartQueryToString(CastAst<SinglePartQuery>(query));
    case ASTNodeType::kMultiPartQuery:
      return MultiPartQueryToString(CastAst<MultiPartQuery>(query));
    default:
      return {};
  }
}

std::string RegularQueryToString(const RegularQuery &query) {
  if (!query.single_query) {
    return {};
  }
  std::string out = SingleQueryToString(*query.single_query);
  if (out.empty()) {
    return {};
  }
  for (const auto &part : query.unions) {
    if (!part || !part->query) {
      return {};
    }
    const std::string sub = SingleQueryToString(*part->query);
    if (sub.empty()) {
      return {};
    }
    out += part->all ? " UNION ALL " : " UNION ";
    out += sub;
  }
  return out;
}

}  // namespace

std::string ExpressionToString(const Expression &expr) {
  return ExpressionText(expr).text;
}

std::string UpdatingClauseToString(const UpdatingClause &clause) {
  return UpdatingClauseText(clause);
}

}  // namespace ast
