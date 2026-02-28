#include "pattern_predicate_rewriter.h"

#include <utility>

#include "common/exception.h"

namespace ast {

void PatternPredicateRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  if (!expr->Is(ASTNodeType::kPatternPredicateExpression)) {
    return;
  }
  auto *pattern_predicate = CastAst<PatternPredicateExpression>(expr.get());
  CHECK(pattern_predicate->relationships_pattern != nullptr,
        common::InvalidArgumentError,
        "pattern predicate relationships pattern is null");

  auto relationships = std::move(pattern_predicate->relationships_pattern);
  auto element = std::make_unique<PatternElement>();
  element->node_pattern = std::move(relationships->node_pattern);
  element->chain = std::move(relationships->chain);

  auto part = std::make_unique<PatternPart>();
  part->element = std::move(element);

  auto pattern = std::make_unique<Pattern>();
  pattern->parts.push_back(std::move(part));

  auto exists = std::make_unique<ExistentialSubquery>();
  exists->pattern = std::move(pattern);
  expr = std::move(exists);
}

}  // namespace ast
