#include "pattern_predicate_rewriter.h"

#include <utility>

namespace ast {

void PatternPredicateRewriter::RewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::RewriteExpression(expr);
  auto *pattern_predicate =
      dynamic_cast<PatternPredicateExpression *>(expr.get());
  if ((pattern_predicate == nullptr) ||
      !pattern_predicate->relationships_pattern) {
    return;
  }

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
