#include "projection_alias_rewriter.h"

namespace ast {
namespace {

bool fillAliasFromExpression(ProjectionItem &item) {
  if (item.alias.empty() == false || !item.expression) {
    return false;
  }
  if (const auto *var =
          dynamic_cast<const Variable *>(item.expression.get())) {
    if (var->name.empty()) {
      return false;
    }
    item.alias = var->name;
    return true;
  }
  if (const auto *prop =
          dynamic_cast<const PropertyExpression *>(item.expression.get())) {
    if (prop->property_key.empty()) {
      return false;
    }
    item.alias = prop->property_key;
    return true;
  }
  if (const auto *fn =
          dynamic_cast<const FunctionInvocation *>(item.expression.get())) {
    if (fn->function_name.empty()) {
      return false;
    }
    item.alias = fn->function_name;
    return true;
  }
  return false;
}

}  // namespace

void ProjectionAliasRewriter::visit(ProjectionBody &node) {
  ASTRewriter::visit(node);
  for (const auto &item : node.items) {
    if (item) {
      fillAliasFromExpression(*item);
    }
  }
}

}  // namespace ast
