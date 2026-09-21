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
  RG_CHECK(item.expression != nullptr, common::ErrorCode::InvalidParameter,
           "projection item expression is null");
  const std::string text = ExpressionToString(*item.expression);
  RG_CHECK(!text.empty(), common::ErrorCode::InvalidParameter,
           "projection item alias stringify failed");
  item.alias = text;
  return true;
}

}  // namespace

void ProjectionAliasRewriter::Visit(ProjectionBody &node) {
  ASTRewriter::Visit(node);
  for (const auto &item : node.items) {
    RG_CHECK(item != nullptr, common::ErrorCode::InvalidParameter,
             "projection item is null");
    FillAliasFromExpression(*item);
  }
}

}  // namespace ast
