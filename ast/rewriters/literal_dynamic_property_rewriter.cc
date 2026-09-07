#include "literal_dynamic_property_rewriter.h"

#include <memory>
#include <utility>

#include "common/exception.h"

namespace ast {

void LiteralDynamicPropertyRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expression) {
  if (!expression) {
    return;
  }
  ASTRewriter::RewriteExpression(expression);
  if (!expression->Is(ASTNodeType::kListIndexExpression)) {
    return;
  }

  auto *index = CastAst<ListIndexExpression>(expression.get());
  if (index->index == nullptr ||
      !index->index->Is(ASTNodeType::kStringLiteral)) {
    return;
  }
  CHECK(index->list != nullptr, common::InvalidArgumentError,
        "dynamic property object is null");

  auto property = std::make_unique<PropertyExpression>();
  property->object = std::move(index->list);
  property->property_key = CastAst<StringLiteral>(*index->index).value;
  expression = std::move(property);
}

}  // namespace ast
