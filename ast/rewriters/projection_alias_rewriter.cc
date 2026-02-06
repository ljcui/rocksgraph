#include "projection_alias_rewriter.h"

#include <string>

#include "../expression_to_string.h"

namespace ast {
namespace {

bool FillAliasFromExpression(ProjectionItem &item) {
  if (!item.alias.empty() || !item.expression) {
    return false;
  }
  const std::string text = expressionToString(*item.expression);
  if (text.empty()) {
    return false;
  }
  item.alias = text;
  return true;
}

}  // namespace

void ProjectionAliasRewriter::visit(ProjectionBody &node) {
  ASTRewriter::visit(node);
  for (const auto &item : node.items) {
    if (item) {
      FillAliasFromExpression(*item);
    }
  }
}

}  // namespace ast
