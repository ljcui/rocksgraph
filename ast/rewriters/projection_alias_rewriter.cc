#include "projection_alias_rewriter.h"

#include <string>

#include "../expression_to_string.h"
#include "common/exception.h"

namespace ast {
namespace {

bool FillAliasFromExpression(ProjectionItem &item) {
  if (!item.alias.empty()) {
    return false;
  }
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "projection item expression is null");
  const std::string text = ExpressionToString(*item.expression);
  CHECK(!text.empty(), common::InvalidArgumentError,
        "projection item alias stringify failed");
  item.alias = text;
  return true;
}

}  // namespace

void ProjectionAliasRewriter::Visit(ProjectionBody &node) {
  ASTRewriter::Visit(node);
  for (const auto &item : node.items) {
    CHECK(item != nullptr, common::InvalidArgumentError,
          "projection item is null");
    FillAliasFromExpression(*item);
  }
}

}  // namespace ast
