#include "expression_to_string.h"

#include <string>
#include <utility>
#include <vector>

namespace ast {

std::string expressionToString(const Expression &expr);

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

std::string join(const std::vector<std::string> &items,
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

std::string escapeStringLiteral(const std::string &value) {
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

std::string wrapIfNeeded(const ExprText &child, int parent_prec,
                         bool wrap_equal) {
  if (child.text.empty()) {
    return {};
  }
  if (child.prec < parent_prec || (wrap_equal && child.prec == parent_prec)) {
    return "(" + child.text + ")";
  }
  return child.text;
}

ExprText expressionText(const Expression &expr);
std::string patternToString(const Pattern &pattern);
std::string patternPartToString(const PatternPart &part);
std::string patternElementToString(const PatternElement &element);
std::string relationshipsPatternToString(const RelationshipsPattern &pattern);
std::string nodePatternToString(const NodePattern &node);
std::string relationshipPatternToString(const RelationshipPattern &pattern);
std::string relationshipDetailToString(const RelationshipDetail &detail);
std::string propertiesToString(const Properties &properties);
std::string projectionBodyToString(const ProjectionBody &body);
std::string regularQueryToString(const RegularQuery &query);

ExprText renderBinary(const Expression *left, const Expression *right, int prec,
                      const std::string &op, bool wrap_equal_left = false,
                      bool wrap_equal_right = true) {
  if (!left || !right) {
    return {};
  }
  const ExprText left_text = expressionText(*left);
  const ExprText right_text = expressionText(*right);
  const std::string left_rendered =
      wrapIfNeeded(left_text, prec, wrap_equal_left);
  const std::string right_rendered =
      wrapIfNeeded(right_text, prec, wrap_equal_right);
  if (left_rendered.empty() || right_rendered.empty()) {
    return {};
  }
  return {left_rendered + " " + op + " " + right_rendered, prec};
}

ExprText renderUnary(const Expression *operand, int prec,
                     const std::string &prefix) {
  if (!operand) {
    return {};
  }
  const ExprText operand_text = expressionText(*operand);
  const std::string operand_rendered =
      wrapIfNeeded(operand_text, prec, false);
  if (operand_rendered.empty()) {
    return {};
  }
  return {prefix + operand_rendered, prec};
}

ExprText expressionText(const Expression &expr) {
  if (const auto *node = dynamic_cast<const OrExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kOr, "OR");
  }
  if (const auto *node = dynamic_cast<const XorExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kXor, "XOR");
  }
  if (const auto *node = dynamic_cast<const AndExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kAnd, "AND");
  }
  if (const auto *node = dynamic_cast<const ComparisonExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kComparison,
                        node->op);
  }
  if (const auto *node =
          dynamic_cast<const ComparisonChainExpression *>(&expr)) {
    if (!node->left || node->rights.empty()) {
      return {};
    }
    std::string out = expressionToString(*node->left);
    if (out.empty()) {
      return {};
    }
    for (const auto &entry : node->rights) {
      if (!entry.second) {
        return {};
      }
      const std::string right = expressionToString(*entry.second);
      if (right.empty()) {
        return {};
      }
      out += " " + entry.first + " " + right;
    }
    return {out, kComparison};
  }
  if (const auto *node = dynamic_cast<const AddExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kAdd, "+");
  }
  if (const auto *node = dynamic_cast<const SubtractExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kAdd, "-");
  }
  if (const auto *node = dynamic_cast<const MultiplyExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kMultiply, "*");
  }
  if (const auto *node = dynamic_cast<const DivideExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kMultiply, "/");
  }
  if (const auto *node = dynamic_cast<const ModuloExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kMultiply, "%");
  }
  if (const auto *node = dynamic_cast<const PowerExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kPower, "^", true,
                        false);
  }
  if (const auto *node = dynamic_cast<const NotExpression *>(&expr)) {
    return renderUnary(node->operand.get(), kUnary, "NOT ");
  }
  if (const auto *node = dynamic_cast<const UnaryPlusExpression *>(&expr)) {
    return renderUnary(node->operand.get(), kUnary, "+");
  }
  if (const auto *node = dynamic_cast<const UnaryMinusExpression *>(&expr)) {
    return renderUnary(node->operand.get(), kUnary, "-");
  }
  if (const auto *node =
          dynamic_cast<const StringPredicateExpression *>(&expr)) {
    return renderBinary(node->left.get(), node->right.get(), kComparison,
                        node->op);
  }
  if (const auto *node = dynamic_cast<const ListPredicateExpression *>(&expr)) {
    return renderBinary(node->element.get(), node->list.get(), kComparison,
                        "IN");
  }
  if (const auto *node = dynamic_cast<const NullPredicateExpression *>(&expr)) {
    if (!node->operand) {
      return {};
    }
    const ExprText operand = expressionText(*node->operand);
    const std::string operand_text = wrapIfNeeded(operand, kComparison, false);
    if (operand_text.empty()) {
      return {};
    }
    return {operand_text + (node->is_null ? " IS NULL" : " IS NOT NULL"),
            kComparison};
  }
  if (const auto *node =
          dynamic_cast<const LabelPredicateExpression *>(&expr)) {
    if (!node->expr || node->labels.empty()) {
      return {};
    }
    const ExprText base = expressionText(*node->expr);
    const std::string base_text = wrapIfNeeded(base, kPostfix, false);
    if (base_text.empty()) {
      return {};
    }
    std::string out = base_text;
    for (const auto &label : node->labels) {
      out += ":" + label;
    }
    return {out, kPostfix};
  }
  if (const auto *node = dynamic_cast<const BooleanLiteral *>(&expr)) {
    return {node->value ? "true" : "false", kPrimary};
  }
  if (const auto *node = dynamic_cast<const IntegerLiteral *>(&expr)) {
    return {std::to_string(node->value), kPrimary};
  }
  if (const auto *node = dynamic_cast<const DoubleLiteral *>(&expr)) {
    return {std::to_string(node->value), kPrimary};
  }
  if (const auto *node = dynamic_cast<const StringLiteral *>(&expr)) {
    return {escapeStringLiteral(node->value), kPrimary};
  }
  if (dynamic_cast<const NullLiteral *>(&expr)) {
    return {"NULL", kPrimary};
  }
  if (const auto *node = dynamic_cast<const ListLiteral *>(&expr)) {
    std::vector<std::string> elements;
    elements.reserve(node->elements.size());
    for (const auto &elem : node->elements) {
      if (!elem) {
        return {};
      }
      const std::string text = expressionToString(*elem);
      if (text.empty()) {
        return {};
      }
      elements.push_back(text);
    }
    return {"[" + join(elements, ", ") + "]", kPrimary};
  }
  if (const auto *node = dynamic_cast<const MapLiteral *>(&expr)) {
    std::vector<std::string> entries;
    entries.reserve(node->entries.size());
    for (const auto &entry : node->entries) {
      if (!entry.second) {
        return {};
      }
      const std::string value = expressionToString(*entry.second);
      if (value.empty()) {
        return {};
      }
      entries.push_back(entry.first + ": " + value);
    }
    return {"{" + join(entries, ", ") + "}", kPrimary};
  }
  if (const auto *node = dynamic_cast<const Variable *>(&expr)) {
    return {node->name, kPrimary};
  }
  if (const auto *node = dynamic_cast<const Parameter *>(&expr)) {
    if (node->name.empty()) {
      return {};
    }
    return {"$" + node->name, kPrimary};
  }
  if (const auto *node = dynamic_cast<const PropertyExpression *>(&expr)) {
    if (!node->object || node->property_key.empty()) {
      return {};
    }
    const ExprText base = expressionText(*node->object);
    const std::string base_text = wrapIfNeeded(base, kPostfix, false);
    if (base_text.empty()) {
      return {};
    }
    return {base_text + "." + node->property_key, kPostfix};
  }
  if (const auto *node = dynamic_cast<const ListIndexExpression *>(&expr)) {
    if (!node->list || !node->index) {
      return {};
    }
    const ExprText list = expressionText(*node->list);
    const std::string list_text = wrapIfNeeded(list, kPostfix, false);
    if (list_text.empty()) {
      return {};
    }
    const std::string index = expressionToString(*node->index);
    if (index.empty()) {
      return {};
    }
    return {list_text + "[" + index + "]", kPostfix};
  }
  if (const auto *node = dynamic_cast<const ListSliceExpression *>(&expr)) {
    if (!node->list) {
      return {};
    }
    const ExprText list = expressionText(*node->list);
    const std::string list_text = wrapIfNeeded(list, kPostfix, false);
    if (list_text.empty()) {
      return {};
    }
    std::string start_text;
    std::string end_text;
    if (node->start_index) {
      start_text = expressionToString(*node->start_index);
      if (start_text.empty()) {
        return {};
      }
    }
    if (node->end_index) {
      end_text = expressionToString(*node->end_index);
      if (end_text.empty()) {
        return {};
      }
    }
    return {list_text + "[" + start_text + ".." + end_text + "]", kPostfix};
  }
  if (const auto *node = dynamic_cast<const FunctionInvocation *>(&expr)) {
    if (node->function_name.empty()) {
      return {};
    }
    std::vector<std::string> args;
    args.reserve(node->arguments.size());
    for (const auto &arg : node->arguments) {
      if (!arg) {
        return {};
      }
      const std::string arg_text = expressionToString(*arg);
      if (arg_text.empty()) {
        return {};
      }
      args.push_back(arg_text);
    }
    std::string arg_text = join(args, ", ");
    if (node->distinct) {
      if (!arg_text.empty()) {
        arg_text = "DISTINCT " + arg_text;
      } else {
        arg_text = "DISTINCT";
      }
    }
    return {node->function_name + "(" + arg_text + ")", kPrimary};
  }
  if (dynamic_cast<const CountStarExpression *>(&expr)) {
    return {"count(*)", kPrimary};
  }
  if (const auto *node = dynamic_cast<const CaseExpression *>(&expr)) {
    std::string out = "CASE";
    if (node->test) {
      const std::string test = expressionToString(*node->test);
      if (test.empty()) {
        return {};
      }
      out += " " + test;
    }
    for (const auto &alt : node->alternatives) {
      if (!alt.first || !alt.second) {
        return {};
      }
      const std::string when_text = expressionToString(*alt.first);
      const std::string then_text = expressionToString(*alt.second);
      if (when_text.empty() || then_text.empty()) {
        return {};
      }
      out += " WHEN " + when_text + " THEN " + then_text;
    }
    if (node->else_expr) {
      const std::string else_text = expressionToString(*node->else_expr);
      if (else_text.empty()) {
        return {};
      }
      out += " ELSE " + else_text;
    }
    out += " END";
    return {out, kPrimary};
  }
  if (const auto *node = dynamic_cast<const ParenthesizedExpression *>(&expr)) {
    if (!node->expr) {
      return {};
    }
    const std::string inner = expressionToString(*node->expr);
    if (inner.empty()) {
      return {};
    }
    return {"(" + inner + ")", kPrimary};
  }
  if (const auto *node = dynamic_cast<const ListComprehension *>(&expr)) {
    if (!node->list_expr) {
      return {};
    }
    const std::string list_text = expressionToString(*node->list_expr);
    if (list_text.empty()) {
      return {};
    }
    std::string out = "[" + node->variable + " IN " + list_text;
    if (node->where_expr) {
      const std::string where_text = expressionToString(*node->where_expr);
      if (where_text.empty()) {
        return {};
      }
      out += " WHERE " + where_text;
    }
    if (node->eval_expr) {
      const std::string eval_text = expressionToString(*node->eval_expr);
      if (eval_text.empty()) {
        return {};
      }
      out += " | " + eval_text;
    }
    out += "]";
    return {out, kPrimary};
  }
  if (const auto *node = dynamic_cast<const PatternComprehension *>(&expr)) {
    if (!node->relationships_pattern || !node->eval_expr) {
      return {};
    }
    const std::string pattern_text =
        relationshipsPatternToString(*node->relationships_pattern);
    if (pattern_text.empty()) {
      return {};
    }
    std::string out = "[";
    if (!node->variable.empty()) {
      out += node->variable + " = ";
    }
    out += pattern_text;
    if (node->where_expr) {
      const std::string where_text = expressionToString(*node->where_expr);
      if (where_text.empty()) {
        return {};
      }
      out += " WHERE " + where_text;
    }
    const std::string eval_text = expressionToString(*node->eval_expr);
    if (eval_text.empty()) {
      return {};
    }
    out += " | " + eval_text + "]";
    return {out, kPrimary};
  }
  if (const auto *node =
          dynamic_cast<const PatternPredicateExpression *>(&expr)) {
    if (!node->relationships_pattern) {
      return {};
    }
    const std::string pattern_text =
        relationshipsPatternToString(*node->relationships_pattern);
    if (pattern_text.empty()) {
      return {};
    }
    return {pattern_text, kPrimary};
  }
  if (const auto *node = dynamic_cast<const AllQuantifier *>(&expr)) {
    if (!node->list_expr) {
      return {};
    }
    const std::string list_text = expressionToString(*node->list_expr);
    if (list_text.empty()) {
      return {};
    }
    std::string out = "ALL(" + node->variable + " IN " + list_text;
    if (node->predicate) {
      const std::string pred_text = expressionToString(*node->predicate);
      if (pred_text.empty()) {
        return {};
      }
      out += " WHERE " + pred_text;
    }
    out += ")";
    return {out, kPrimary};
  }
  if (const auto *node = dynamic_cast<const AnyQuantifier *>(&expr)) {
    if (!node->list_expr) {
      return {};
    }
    const std::string list_text = expressionToString(*node->list_expr);
    if (list_text.empty()) {
      return {};
    }
    std::string out = "ANY(" + node->variable + " IN " + list_text;
    if (node->predicate) {
      const std::string pred_text = expressionToString(*node->predicate);
      if (pred_text.empty()) {
        return {};
      }
      out += " WHERE " + pred_text;
    }
    out += ")";
    return {out, kPrimary};
  }
  if (const auto *node = dynamic_cast<const NoneQuantifier *>(&expr)) {
    if (!node->list_expr) {
      return {};
    }
    const std::string list_text = expressionToString(*node->list_expr);
    if (list_text.empty()) {
      return {};
    }
    std::string out = "NONE(" + node->variable + " IN " + list_text;
    if (node->predicate) {
      const std::string pred_text = expressionToString(*node->predicate);
      if (pred_text.empty()) {
        return {};
      }
      out += " WHERE " + pred_text;
    }
    out += ")";
    return {out, kPrimary};
  }
  if (const auto *node = dynamic_cast<const SingleQuantifier *>(&expr)) {
    if (!node->list_expr) {
      return {};
    }
    const std::string list_text = expressionToString(*node->list_expr);
    if (list_text.empty()) {
      return {};
    }
    std::string out = "SINGLE(" + node->variable + " IN " + list_text;
    if (node->predicate) {
      const std::string pred_text = expressionToString(*node->predicate);
      if (pred_text.empty()) {
        return {};
      }
      out += " WHERE " + pred_text;
    }
    out += ")";
    return {out, kPrimary};
  }
  if (const auto *node = dynamic_cast<const ExistentialSubquery *>(&expr)) {
    std::string out = "EXISTS { ";
    if (node->query) {
      const std::string query_text = regularQueryToString(*node->query);
      if (query_text.empty()) {
        return {};
      }
      out += query_text;
      out += " }";
      return {out, kPrimary};
    }
    if (node->pattern) {
      const std::string pattern_text = patternToString(*node->pattern);
      if (pattern_text.empty()) {
        return {};
      }
      out += pattern_text;
      if (node->where_expr) {
        const std::string where_text = expressionToString(*node->where_expr);
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
  return {};
}

std::string propertiesToString(const Properties &properties) {
  if (properties.map) {
    return expressionToString(*properties.map);
  }
  if (properties.parameter) {
    if (properties.parameter->name.empty()) {
      return {};
    }
    return "$" + properties.parameter->name;
  }
  return {};
}

std::string nodePatternToString(const NodePattern &node) {
  std::string out = "(";
  if (!node.variable.empty()) {
    out += node.variable;
  }
  for (const auto &label : node.labels) {
    out += ":" + label;
  }
  if (node.properties) {
    const std::string props = propertiesToString(*node.properties);
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

std::string relationshipDetailToString(const RelationshipDetail &detail) {
  std::string out;
  if (!detail.variable.empty()) {
    out += detail.variable;
  }
  if (!detail.types.empty()) {
    out += ":" + join(detail.types, "|");
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
    const std::string props = propertiesToString(*detail.properties);
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

std::string relationshipPatternToString(const RelationshipPattern &pattern) {
  const std::string left = pattern.left_arrow ? "<-" : "-";
  const std::string right = pattern.right_arrow ? "->" : "-";
  std::string middle;
  if (pattern.detail) {
    middle = relationshipDetailToString(*pattern.detail);
    if (middle.empty()) {
      return {};
    }
  }
  return left + middle + right;
}

std::string patternElementToString(const PatternElement &element) {
  if (!element.node_pattern) {
    return {};
  }
  std::string out = nodePatternToString(*element.node_pattern);
  if (out.empty()) {
    return {};
  }
  for (const auto &link : element.chain) {
    if (!link.first || !link.second) {
      return {};
    }
    const std::string rel = relationshipPatternToString(*link.first);
    const std::string node = nodePatternToString(*link.second);
    if (rel.empty() || node.empty()) {
      return {};
    }
    out += rel + node;
  }
  return out;
}

std::string relationshipsPatternToString(const RelationshipsPattern &pattern) {
  if (!pattern.node_pattern) {
    return {};
  }
  std::string out = nodePatternToString(*pattern.node_pattern);
  if (out.empty()) {
    return {};
  }
  for (const auto &link : pattern.chain) {
    if (!link.first || !link.second) {
      return {};
    }
    const std::string rel = relationshipPatternToString(*link.first);
    const std::string node = nodePatternToString(*link.second);
    if (rel.empty() || node.empty()) {
      return {};
    }
    out += rel + node;
  }
  return out;
}

std::string patternPartToString(const PatternPart &part) {
  if (!part.element) {
    return {};
  }
  const std::string element = patternElementToString(*part.element);
  if (element.empty()) {
    return {};
  }
  if (part.variable.empty()) {
    return element;
  }
  return part.variable + " = " + element;
}

std::string patternToString(const Pattern &pattern) {
  std::vector<std::string> parts;
  parts.reserve(pattern.parts.size());
  for (const auto &part : pattern.parts) {
    if (!part) {
      return {};
    }
    const std::string text = patternPartToString(*part);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  return join(parts, ", ");
}

std::string projectionItemToString(const ProjectionItem &item) {
  if (!item.expression) {
    return {};
  }
  std::string out = expressionToString(*item.expression);
  if (out.empty()) {
    return {};
  }
  if (!item.alias.empty()) {
    out += " AS " + item.alias;
  }
  return out;
}

std::string sortItemToString(const SortItem &item) {
  if (!item.expression) {
    return {};
  }
  std::string out = expressionToString(*item.expression);
  if (out.empty()) {
    return {};
  }
  if (!item.ascending) {
    out += " DESC";
  }
  return out;
}

std::string projectionBodyToString(const ProjectionBody &body) {
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
      const std::string text = projectionItemToString(*item);
      if (text.empty()) {
        return {};
      }
      items.push_back(text);
    }
    if (!items_text.empty()) {
      items_text += ", ";
    }
    items_text += join(items, ", ");
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
      const std::string text = sortItemToString(*item);
      if (text.empty()) {
        return {};
      }
      order_items.push_back(text);
    }
    out += " ORDER BY " + join(order_items, ", ");
  }
  if (body.skip) {
    const std::string skip = expressionToString(*body.skip);
    if (skip.empty()) {
      return {};
    }
    out += " SKIP " + skip;
  }
  if (body.limit) {
    const std::string limit = expressionToString(*body.limit);
    if (limit.empty()) {
      return {};
    }
    out += " LIMIT " + limit;
  }
  return out;
}

std::string setItemToString(const SetItem &item) {
  if (!item.target) {
    return {};
  }
  const std::string target = expressionToString(*item.target);
  if (target.empty()) {
    return {};
  }
  switch (item.type) {
    case SetItem::Type::Property: {
      if (!item.value) {
        return {};
      }
      const std::string value = expressionToString(*item.value);
      if (value.empty()) {
        return {};
      }
      return target + " = " + value;
    }
    case SetItem::Type::Variable: {
      if (!item.value) {
        return {};
      }
      const std::string value = expressionToString(*item.value);
      if (value.empty()) {
        return {};
      }
      if (item.plus_equal) {
        return target + " += " + value;
      }
      return target + " = " + value;
    }
    case SetItem::Type::Labels: {
      if (item.labels.empty()) {
        return {};
      }
      return target + ":" + join(item.labels, ":");
    }
  }
  return {};
}

std::string removeItemToString(const RemoveItem &item) {
  if (!item.target) {
    return {};
  }
  const std::string target = expressionToString(*item.target);
  if (target.empty()) {
    return {};
  }
  switch (item.type) {
    case RemoveItem::Type::Property:
      return target;
    case RemoveItem::Type::Labels:
      if (item.labels.empty()) {
        return {};
      }
      return target + ":" + join(item.labels, ":");
  }
  return {};
}

std::string readingClauseToString(const ReadingClause &clause) {
  if (const auto *match = dynamic_cast<const Match *>(&clause)) {
    if (!match->pattern) {
      return {};
    }
    const std::string pattern = patternToString(*match->pattern);
    if (pattern.empty()) {
      return {};
    }
    std::string out = match->optional_match ? "OPTIONAL MATCH " : "MATCH ";
    out += pattern;
    if (match->where) {
      const std::string where_text = expressionToString(*match->where);
      if (where_text.empty()) {
        return {};
      }
      out += " WHERE " + where_text;
    }
    return out;
  }
  if (const auto *unwind = dynamic_cast<const Unwind *>(&clause)) {
    if (!unwind->expression) {
      return {};
    }
    const std::string expr = expressionToString(*unwind->expression);
    if (expr.empty()) {
      return {};
    }
    return "UNWIND " + expr + " AS " + unwind->variable;
  }
  if (const auto *call = dynamic_cast<const InQueryCall *>(&clause)) {
    std::vector<std::string> args;
    args.reserve(call->arguments.size());
    for (const auto &arg : call->arguments) {
      if (!arg) {
        return {};
      }
      const std::string text = expressionToString(*arg);
      if (text.empty()) {
        return {};
      }
      args.push_back(text);
    }
    std::string out =
        "CALL " + call->procedure_name + "(" + join(args, ", ") + ")";
    if (!call->yield_items.empty()) {
      std::vector<std::string> items;
      items.reserve(call->yield_items.size());
      for (const auto &item : call->yield_items) {
        if (item.result_field) {
          items.push_back(*item.result_field + " AS " + item.variable);
        } else {
          items.push_back(item.variable);
        }
      }
      out += " YIELD " + join(items, ", ");
    }
    if (call->yield_where) {
      const std::string where_text = expressionToString(*call->yield_where);
      if (where_text.empty()) {
        return {};
      }
      out += " WHERE " + where_text;
    }
    return out;
  }
  return {};
}

std::string updatingClauseToString(const UpdatingClause &clause) {
  if (const auto *create = dynamic_cast<const Create *>(&clause)) {
    if (!create->pattern) {
      return {};
    }
    const std::string pattern = patternToString(*create->pattern);
    if (pattern.empty()) {
      return {};
    }
    return "CREATE " + pattern;
  }
  if (const auto *merge = dynamic_cast<const Merge *>(&clause)) {
    if (!merge->pattern_part) {
      return {};
    }
    const std::string part = patternPartToString(*merge->pattern_part);
    if (part.empty()) {
      return {};
    }
    std::string out = "MERGE " + part;
    for (const auto &action : merge->actions) {
      if (!action.second) {
        return {};
      }
      std::vector<std::string> items;
      items.reserve(action.second->items.size());
      for (const auto &item : action.second->items) {
        if (!item) {
          return {};
        }
        const std::string text = setItemToString(*item);
        if (text.empty()) {
          return {};
        }
        items.push_back(text);
      }
      out += action.first ? " ON MATCH SET " : " ON CREATE SET ";
      out += join(items, ", ");
    }
    return out;
  }
  if (const auto *del = dynamic_cast<const Delete *>(&clause)) {
    if (del->expressions.empty()) {
      return {};
    }
    std::vector<std::string> items;
    items.reserve(del->expressions.size());
    for (const auto &expr : del->expressions) {
      if (!expr) {
        return {};
      }
      const std::string text = expressionToString(*expr);
      if (text.empty()) {
        return {};
      }
      items.push_back(text);
    }
    return std::string(del->detach ? "DETACH DELETE " : "DELETE ") +
           join(items, ", ");
  }
  if (const auto *set = dynamic_cast<const Set *>(&clause)) {
    if (set->items.empty()) {
      return {};
    }
    std::vector<std::string> items;
    items.reserve(set->items.size());
    for (const auto &item : set->items) {
      if (!item) {
        return {};
      }
      const std::string text = setItemToString(*item);
      if (text.empty()) {
        return {};
      }
      items.push_back(text);
    }
    return "SET " + join(items, ", ");
  }
  if (const auto *remove = dynamic_cast<const Remove *>(&clause)) {
    if (remove->items.empty()) {
      return {};
    }
    std::vector<std::string> items;
    items.reserve(remove->items.size());
    for (const auto &item : remove->items) {
      if (!item) {
        return {};
      }
      const std::string text = removeItemToString(*item);
      if (text.empty()) {
        return {};
      }
      items.push_back(text);
    }
    return "REMOVE " + join(items, ", ");
  }
  return {};
}

std::string projectionClauseToString(const ProjectionClause &clause) {
  if (!clause.body) {
    return {};
  }
  const std::string body = projectionBodyToString(*clause.body);
  if (body.empty()) {
    return {};
  }
  if (const auto *with = dynamic_cast<const With *>(&clause)) {
    std::string out = "WITH " + body;
    if (with->where) {
      const std::string where_text = expressionToString(*with->where);
      if (where_text.empty()) {
        return {};
      }
      out += " WHERE " + where_text;
    }
    return out;
  }
  if (dynamic_cast<const Return *>(&clause)) {
    return "RETURN " + body;
  }
  return {};
}

std::string singlePartQueryToString(const SinglePartQuery &query) {
  std::vector<std::string> parts;
  parts.reserve(query.reading_clauses.size() + query.updating_clauses.size() +
                1);
  for (const auto &clause : query.reading_clauses) {
    if (!clause) {
      return {};
    }
    const std::string text = readingClauseToString(*clause);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  for (const auto &clause : query.updating_clauses) {
    if (!clause) {
      return {};
    }
    const std::string text = updatingClauseToString(*clause);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  if (query.return_clause) {
    const std::string text = projectionClauseToString(*query.return_clause);
    if (text.empty()) {
      return {};
    }
    parts.push_back(text);
  }
  return join(parts, " ");
}

std::string multiPartQueryToString(const MultiPartQuery &query) {
  std::vector<std::string> parts;
  for (const auto &part : query.parts) {
    std::vector<std::string> segment;
    for (const auto &clause : part.reading_clauses) {
      if (!clause) {
        return {};
      }
      const std::string text = readingClauseToString(*clause);
      if (text.empty()) {
        return {};
      }
      segment.push_back(text);
    }
    for (const auto &clause : part.updating_clauses) {
      if (!clause) {
        return {};
      }
      const std::string text = updatingClauseToString(*clause);
      if (text.empty()) {
        return {};
      }
      segment.push_back(text);
    }
    if (!part.with_clause) {
      return {};
    }
    const std::string with_text = projectionClauseToString(*part.with_clause);
    if (with_text.empty()) {
      return {};
    }
    segment.push_back(with_text);
    parts.push_back(join(segment, " "));
  }
  if (!query.final_single_part_query) {
    return {};
  }
  const std::string final_part =
      singlePartQueryToString(*query.final_single_part_query);
  if (final_part.empty()) {
    return {};
  }
  parts.push_back(final_part);
  return join(parts, " ");
}

std::string singleQueryToString(const SingleQuery &query) {
  if (const auto *single = dynamic_cast<const SinglePartQuery *>(&query)) {
    return singlePartQueryToString(*single);
  }
  if (const auto *multi = dynamic_cast<const MultiPartQuery *>(&query)) {
    return multiPartQueryToString(*multi);
  }
  return {};
}

std::string regularQueryToString(const RegularQuery &query) {
  if (!query.single_query) {
    return {};
  }
  std::string out = singleQueryToString(*query.single_query);
  if (out.empty()) {
    return {};
  }
  for (const auto &part : query.unions) {
    if (!part || !part->query) {
      return {};
    }
    const std::string sub = singleQueryToString(*part->query);
    if (sub.empty()) {
      return {};
    }
    out += part->all ? " UNION ALL " : " UNION ";
    out += sub;
  }
  return out;
}

}  // namespace

std::string expressionToString(const Expression &expr) {
  return expressionText(expr).text;
}

}  // namespace ast
