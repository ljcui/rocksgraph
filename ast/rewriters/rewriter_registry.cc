#include "rewriter_registry.h"

#include "comparison_chain_rewriter.h"
#include "parenthesized_expression_rewriter.h"
#include "pattern_predicate_rewriter.h"
#include "return_star_rewriter.h"

namespace ast {

std::vector<std::unique_ptr<ASTRewriter>> makeDefaultRewriters() {
  std::vector<std::unique_ptr<ASTRewriter>> rewriters;
  // Normalize expression forms before scope-dependent rewrites.
  rewriters.emplace_back(std::make_unique<ParenthesizedExpressionRewriter>());
  rewriters.emplace_back(std::make_unique<ComparisonChainRewriter>());
  rewriters.emplace_back(std::make_unique<PatternPredicateRewriter>());
  // Projection expansion should run after expression normalization.
  rewriters.emplace_back(std::make_unique<ReturnStarRewriter>());
  return rewriters;
}

}  // namespace ast
