#include "rewriter_registry.h"

#include "aggregation_expression_rewriter.h"
#include "anonymous_pattern_name_rewriter.h"
#include "comparison_chain_rewriter.h"
#include "existential_subquery_rewriter.h"
#include "parenthesized_expression_rewriter.h"
#include "pattern_predicate_normalization_rewriter.h"
#include "pattern_predicate_rewriter.h"
#include "projection_alias_reference_rewriter.h"
#include "projection_alias_rewriter.h"
#include "return_star_rewriter.h"
#include "uniqueness_predicates_rewriter.h"

namespace ast {

std::vector<std::unique_ptr<ASTRewriter>> MakeDefaultRewriters() {
  std::vector<std::unique_ptr<ASTRewriter>> rewriters;
  // Normalize expression forms before scope-dependent rewrites.
  rewriters.emplace_back(std::make_unique<ParenthesizedExpressionRewriter>());
  rewriters.emplace_back(std::make_unique<ComparisonChainRewriter>());
  rewriters.emplace_back(std::make_unique<PatternPredicateRewriter>());
  rewriters.emplace_back(std::make_unique<ExistentialSubqueryRewriter>());
  rewriters.emplace_back(std::make_unique<ProjectionAliasRewriter>());
  rewriters.emplace_back(std::make_unique<ProjectionAliasReferenceRewriter>());
  // Projection expansion should run after expression normalization.
  rewriters.emplace_back(std::make_unique<ReturnStarRewriter>());
  // Aggregation isolation needs explicit grouping items from star expansion.
  rewriters.emplace_back(std::make_unique<AggregationExpressionRewriter>());
  // Assign anonymous names after scope-expanding rewrites.
  rewriters.emplace_back(std::make_unique<AnonymousPatternNameRewriter>());
  // Pull inline pattern predicates into WHERE after names are assigned.
  rewriters.emplace_back(
      std::make_unique<PatternPredicateNormalizationRewriter>());
  // Add relationship uniqueness predicates for trail semantics.
  rewriters.emplace_back(std::make_unique<AddUniquenessPredicatesRewriter>());
  return rewriters;
}

}  // namespace ast
