#include "rewriter_registry.h"

#include "anonymous_pattern_name_rewriter.h"
#include "comparison_chain_rewriter.h"
#include "count_star_rewriter.h"
#include "existential_subquery_rewriter.h"
#include "order_by_alias_rewriter.h"
#include "parenthesized_expression_rewriter.h"
#include "pattern_predicate_rewriter.h"
#include "projection_alias_rewriter.h"
#include "return_star_rewriter.h"

namespace ast {

std::vector<std::unique_ptr<ASTRewriter>> makeDefaultRewriters() {
  std::vector<std::unique_ptr<ASTRewriter>> rewriters;
  // Normalize expression forms before scope-dependent rewrites.
  rewriters.emplace_back(std::make_unique<ParenthesizedExpressionRewriter>());
  rewriters.emplace_back(std::make_unique<ComparisonChainRewriter>());
  rewriters.emplace_back(std::make_unique<PatternPredicateRewriter>());
  rewriters.emplace_back(std::make_unique<ExistentialSubqueryRewriter>());
  rewriters.emplace_back(std::make_unique<CountStarRewriter>());
  rewriters.emplace_back(std::make_unique<ProjectionAliasRewriter>());
  rewriters.emplace_back(std::make_unique<OrderByAliasRewriter>());
  // Projection expansion should run after expression normalization.
  rewriters.emplace_back(std::make_unique<ReturnStarRewriter>());
  // Assign anonymous names after scope-expanding rewrites.
  rewriters.emplace_back(std::make_unique<AnonymousPatternNameRewriter>());
  return rewriters;
}

}  // namespace ast
