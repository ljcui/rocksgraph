#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_equal.h"
#include "ast/ast_exception.h"
#include "ast/rewriters/add_uniqueness_predicates_rewriter.h"
#include "ast/rewriters/anonymous_pattern_name_rewriter.h"
#include "ast/rewriters/comparison_chain_rewriter.h"
#include "ast/rewriters/count_star_rewriter.h"
#include "ast/rewriters/existential_subquery_rewriter.h"
#include "ast/rewriters/order_by_alias_rewriter.h"
#include "ast/rewriters/parenthesized_expression_rewriter.h"
#include "ast/rewriters/pattern_predicate_normalization_rewriter.h"
#include "ast/rewriters/pattern_predicate_rewriter.h"
#include "ast/rewriters/projection_alias_rewriter.h"
#include "ast/rewriters/return_star_rewriter.h"
#include "ast/rewriters/rewriter_pipeline.h"

namespace {

std::unique_ptr<ast::Statement> ParseOrFail(const std::string &query) {
  try {
    return ast::parseCypher(query);
  } catch (const ast::ParseError &e) {
    ADD_FAILURE() << "parse errors for query: " << query
                  << " message: " << e.what();
  } catch (const ast::SemanticError &e) {
    ADD_FAILURE() << "semantic errors for query: " << query
                  << " message: " << e.what();
  }
  return {};
}

template <typename Rewriter>
void ExpectRewriteEqualsWith(const std::string &input,
                             const std::string &expected) {
  auto statement = ParseOrFail(input);
  auto expected_statement = ParseOrFail(expected);

  Rewriter rewriter;
  rewriter.Rewrite(*statement);

  EXPECT_TRUE(ast::ASTEqual::Equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for input: " << input;
}

}  // namespace

TEST(ReturnStarRewriterTest, MatchReturnStar) {
  ExpectRewriteEqualsWith<ast::ReturnStarRewriter>("MATCH (n) RETURN *",
                                                   "MATCH (n) RETURN n");
}

TEST(ReturnStarRewriterTest, UnwindReturnStar) {
  ExpectRewriteEqualsWith<ast::ReturnStarRewriter>(
      "UNWIND [1, 2] AS x RETURN *", "UNWIND [1, 2] AS x RETURN x");
}

TEST(ReturnStarRewriterTest, WithStarThenReturnStar) {
  ExpectRewriteEqualsWith<ast::ReturnStarRewriter>("MATCH (n) WITH * RETURN *",
                                                   "MATCH (n) WITH n RETURN n");
}

TEST(ReturnStarRewriterTest, WithAliasReturnStar) {
  ExpectRewriteEqualsWith<ast::ReturnStarRewriter>(
      "MATCH (n) WITH n AS m RETURN *", "MATCH (n) WITH n AS m RETURN m");
}

TEST(ComparisonChainRewriterTest, ReturnChain) {
  ExpectRewriteEqualsWith<ast::ComparisonChainRewriter>(
      "RETURN 1 < 2 < 3", "RETURN 1 < 2 AND 2 < 3");
}

TEST(ComparisonChainRewriterTest, WhereChain) {
  ExpectRewriteEqualsWith<ast::ComparisonChainRewriter>(
      "MATCH (n) WHERE 1 < n.age <= 10 RETURN n",
      "MATCH (n) WHERE 1 < n.age AND n.age <= 10 RETURN n");
}

TEST(ParenthesizedExpressionRewriterTest, ReturnNested) {
  ExpectRewriteEqualsWith<ast::ParenthesizedExpressionRewriter>("RETURN ((1))",
                                                                "RETURN 1");
}

TEST(ParenthesizedExpressionRewriterTest, WhereExpression) {
  ExpectRewriteEqualsWith<ast::ParenthesizedExpressionRewriter>(
      "MATCH (n) WHERE ((n.age > 1)) RETURN n",
      "MATCH (n) WHERE n.age > 1 RETURN n");
}

TEST(PatternPredicateRewriterTest, WherePattern) {
  ExpectRewriteEqualsWith<ast::PatternPredicateRewriter>(
      "MATCH (n) WHERE (n)-[:R]->(m) RETURN n",
      "MATCH (n) WHERE EXISTS { (n)-[:R]->(m) } RETURN n");
}

TEST(PatternPredicateNormalizationRewriterTest, PullsNodePredicatesToWhere) {
  ExpectRewriteEqualsWith<ast::PatternPredicateNormalizationRewriter>(
      "MATCH (n:Person {name: 'Alice'}) WHERE n.age > 30 RETURN n",
      "MATCH (n) WHERE n:Person AND n.name = 'Alice' AND n.age > 30 RETURN n");
}

TEST(PatternPredicateNormalizationRewriterTest,
     PullsRelationshipPredicatesToWhere) {
  ExpectRewriteEqualsWith<ast::PatternPredicateNormalizationRewriter>(
      "MATCH (n)-[r:KNOWS {since: 2020}]->(m) RETURN r",
      "MATCH (n)-[r]->(m) WHERE r:KNOWS AND r.since = 2020 RETURN r");
}

TEST(ExistentialSubqueryRewriterTest, PatternToMatchQuery) {
  ExpectRewriteEqualsWith<ast::ExistentialSubqueryRewriter>(
      "MATCH (n) WHERE EXISTS { (n)-[:R]->(m) } RETURN n",
      "MATCH (n) WHERE EXISTS { MATCH (n)-[:R]->(m) RETURN 1 } RETURN n");
}

TEST(OrderByAliasRewriterTest, RewritesExpressionToAlias) {
  ExpectRewriteEqualsWith<ast::OrderByAliasRewriter>(
      "MATCH (n) RETURN n.age AS age ORDER BY n.age",
      "MATCH (n) RETURN n.age AS age ORDER BY age");
}

TEST(CountStarRewriterTest, CountStarToFunction) {
  ExpectRewriteEqualsWith<ast::CountStarRewriter>("RETURN count(*)",
                                                  "RETURN count(1)");
}

TEST(ProjectionAliasRewriterTest, FillsAliasFromProperty) {
  ExpectRewriteEqualsWith<ast::ProjectionAliasRewriter>(
      "MATCH (n) RETURN n.name", "MATCH (n) RETURN n.name AS `n.name`");
}

TEST(ProjectionAliasRewriterTest, FillsAliasFromAddExpression) {
  ExpectRewriteEqualsWith<ast::ProjectionAliasRewriter>(
      "MATCH (n) RETURN n.age + 1",
      "MATCH (n) RETURN n.age + 1 AS `n.age + 1`");
}

TEST(AnonymousPatternNameRewriterTest, NamesUnnamedNodes) {
  ExpectRewriteEqualsWith<ast::AnonymousPatternNameRewriter>(
      "MATCH ()-[r]-() RETURN r", "MATCH (anon_0)-[r]-(anon_1) RETURN r");
}

TEST(AnonymousPatternNameRewriterTest, NamesUnnamedRelationships) {
  ExpectRewriteEqualsWith<ast::AnonymousPatternNameRewriter>(
      "MATCH ()--() RETURN 1", "MATCH (anon_0)-[anon_1]-(anon_2) RETURN 1");
}

TEST(AddUniquenessPredicatesRewriterTest, AddsDifferentRelationshipPredicate) {
  ExpectRewriteEqualsWith<ast::AddUniquenessPredicatesRewriter>(
      "MATCH (a)-[r1]->(b)-[r2]->(c) RETURN *",
      "MATCH (a)-[r1]->(b)-[r2]->(c) WHERE NOT r1 = r2 RETURN *");
}

TEST(AddUniquenessPredicatesRewriterTest,
     SkipsDifferentRelationshipTypePredicate) {
  ExpectRewriteEqualsWith<ast::AddUniquenessPredicatesRewriter>(
      "MATCH (a)-[r1:X]->(b)-[r2:Y]->(c) RETURN *",
      "MATCH (a)-[r1:X]->(b)-[r2:Y]->(c) RETURN *");
}

TEST(AddUniquenessPredicatesRewriterTest, AddsUniquePredicateForVarLength) {
  ExpectRewriteEqualsWith<ast::AddUniquenessPredicatesRewriter>(
      "MATCH (a)-[r*0..1]->(b) RETURN *",
      "MATCH (a)-[r*0..1]->(b) WHERE ALL(__uniq_rel_0 IN r WHERE "
      "SINGLE(__uniq_rel_1 IN r WHERE __uniq_rel_0 = __uniq_rel_1)) RETURN *");
}

TEST(AddUniquenessPredicatesRewriterTest,
     AddsNoneInPredicateBetweenSimpleAndVarLength) {
  ExpectRewriteEqualsWith<ast::AddUniquenessPredicatesRewriter>(
      "MATCH (a)-[r1]->(b)-[r2*0..1]->(c) RETURN *",
      "MATCH (a)-[r1]->(b)-[r2*0..1]->(c) WHERE NOT r1 IN r2 AND "
      "ALL(__uniq_rel_0 IN r2 WHERE SINGLE(__uniq_rel_1 IN r2 WHERE "
      "__uniq_rel_0 = __uniq_rel_1)) RETURN *");
}

TEST(AddUniquenessPredicatesRewriterTest,
     RepeatedRelationshipVariableBecomesFalse) {
  ExpectRewriteEqualsWith<ast::AddUniquenessPredicatesRewriter>(
      "MATCH (a)-[r]->(b)-[r]->(c) RETURN *",
      "MATCH (a)-[r]->(b)-[r]->(c) WHERE false RETURN *");
}

TEST(AddUniquenessPredicatesRewriterTest,
     RepeatedNonEmptyVarLengthRelationshipIncludesFalse) {
  ExpectRewriteEqualsWith<ast::AddUniquenessPredicatesRewriter>(
      "MATCH (a)-[r*1..2]->(b)-[r*1..2]->(c) RETURN *",
      "MATCH (a)-[r*1..2]->(b)-[r*1..2]->(c) "
      "WHERE false AND "
      "ALL(__uniq_rel_0 IN r WHERE SINGLE(__uniq_rel_1 IN r WHERE __uniq_rel_0 "
      "= __uniq_rel_1)) AND "
      "ALL(__uniq_rel_2 IN r WHERE SINGLE(__uniq_rel_3 IN r WHERE __uniq_rel_2 "
      "= __uniq_rel_3)) "
      "RETURN *");
}

TEST(RewriterPipelineTest, DefaultPipelineUsesReturnStar) {
  auto statement = ParseOrFail("MATCH (n) RETURN *");
  auto expected_statement = ParseOrFail("MATCH (n) RETURN n");

  ast::applyDefaultRewriters(*statement);

  EXPECT_TRUE(ast::ASTEqual::Equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for pipeline";
}

TEST(RewriterPipelineTest, ParseAndRewriteUsesDefaultPipeline) {
  std::unique_ptr<ast::Statement> statement;
  ASSERT_NO_THROW(statement = ast::parseCypherAndRewrite("MATCH (n) RETURN *"));
  auto expected_statement = ParseOrFail("MATCH (n) RETURN n");

  EXPECT_TRUE(ast::ASTEqual::Equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for parseCypherAndRewrite";
}

TEST(RewriterPipelineTest, DefaultPipelineAddsUniquenessPredicates) {
  auto statement = ParseOrFail("MATCH (a)-[r1]->(b)-[r2]->(c) RETURN *");
  auto expected_statement = ParseOrFail(
      "MATCH (a)-[r1]->(b)-[r2]->(c) WHERE NOT r1 = r2 RETURN a, r1, b, r2, c");

  ast::applyDefaultRewriters(*statement);

  EXPECT_TRUE(ast::ASTEqual::Equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for uniqueness in pipeline";
}

TEST(RewriterPipelineTest, DefaultPipelineAddsFalseForRepeatedRelationship) {
  auto statement = ParseOrFail("MATCH (a)-[r]->(b)-[r]->(c) RETURN *");
  auto expected_statement =
      ParseOrFail("MATCH (a)-[r]->(b)-[r]->(c) WHERE false RETURN a, r, b, c");

  ast::applyDefaultRewriters(*statement);

  EXPECT_TRUE(ast::ASTEqual::Equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for repeated relationship variable";
}

TEST(RewriterPipelineTest,
     DefaultPipelineAddsFalseForRepeatedNonEmptyVarLengthRelationship) {
  auto statement =
      ParseOrFail("MATCH (a)-[r*1..2]->(b)-[r*1..2]->(c) RETURN *");
  auto expected_statement = ParseOrFail(
      "MATCH (a)-[r*1..2]->(b)-[r*1..2]->(c) "
      "WHERE false AND "
      "ALL(__uniq_rel_0 IN r WHERE SINGLE(__uniq_rel_1 IN r WHERE __uniq_rel_0 "
      "= __uniq_rel_1)) AND "
      "ALL(__uniq_rel_2 IN r WHERE SINGLE(__uniq_rel_3 IN r WHERE __uniq_rel_2 "
      "= __uniq_rel_3)) "
      "RETURN a, r, b, c");

  ast::applyDefaultRewriters(*statement);

  EXPECT_TRUE(ast::ASTEqual::Equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for repeated non-empty var-length relationship";
}
