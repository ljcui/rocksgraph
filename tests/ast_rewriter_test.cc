#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_equal.h"
#include "ast/ast_exception.h"
#include "ast/rewriters/anonymous_pattern_name_rewriter.h"
#include "ast/rewriters/comparison_chain_rewriter.h"
#include "ast/rewriters/count_star_rewriter.h"
#include "ast/rewriters/existential_subquery_rewriter.h"
#include "ast/rewriters/order_by_alias_rewriter.h"
#include "ast/rewriters/parenthesized_expression_rewriter.h"
#include "ast/rewriters/pattern_predicate_rewriter.h"
#include "ast/rewriters/projection_alias_rewriter.h"
#include "ast/rewriters/return_star_rewriter.h"
#include "ast/rewriters/rewriter_pipeline.h"

namespace {

std::unique_ptr<ast::Statement> parseOrFail(const std::string &query) {
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
void expectRewriteEqualsWith(const std::string &input,
                             const std::string &expected) {
  auto statement = parseOrFail(input);
  auto expected_statement = parseOrFail(expected);

  Rewriter rewriter;
  rewriter.rewrite(*statement);

  EXPECT_TRUE(ast::ASTEqual::equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for input: " << input;
}

}  // namespace

TEST(ReturnStarRewriterTest, MatchReturnStar) {
  expectRewriteEqualsWith<ast::ReturnStarRewriter>("MATCH (n) RETURN *",
                                                   "MATCH (n) RETURN n");
}

TEST(ReturnStarRewriterTest, UnwindReturnStar) {
  expectRewriteEqualsWith<ast::ReturnStarRewriter>(
      "UNWIND [1, 2] AS x RETURN *", "UNWIND [1, 2] AS x RETURN x");
}

TEST(ReturnStarRewriterTest, WithStarThenReturnStar) {
  expectRewriteEqualsWith<ast::ReturnStarRewriter>("MATCH (n) WITH * RETURN *",
                                                   "MATCH (n) WITH n RETURN n");
}

TEST(ReturnStarRewriterTest, WithAliasReturnStar) {
  expectRewriteEqualsWith<ast::ReturnStarRewriter>(
      "MATCH (n) WITH n AS m RETURN *", "MATCH (n) WITH n AS m RETURN m");
}

TEST(ComparisonChainRewriterTest, ReturnChain) {
  expectRewriteEqualsWith<ast::ComparisonChainRewriter>(
      "RETURN 1 < 2 < 3", "RETURN 1 < 2 AND 2 < 3");
}

TEST(ComparisonChainRewriterTest, WhereChain) {
  expectRewriteEqualsWith<ast::ComparisonChainRewriter>(
      "MATCH (n) WHERE 1 < n.age <= 10 RETURN n",
      "MATCH (n) WHERE 1 < n.age AND n.age <= 10 RETURN n");
}

TEST(ParenthesizedExpressionRewriterTest, ReturnNested) {
  expectRewriteEqualsWith<ast::ParenthesizedExpressionRewriter>("RETURN ((1))",
                                                                "RETURN 1");
}

TEST(ParenthesizedExpressionRewriterTest, WhereExpression) {
  expectRewriteEqualsWith<ast::ParenthesizedExpressionRewriter>(
      "MATCH (n) WHERE ((n.age > 1)) RETURN n",
      "MATCH (n) WHERE n.age > 1 RETURN n");
}

TEST(PatternPredicateRewriterTest, WherePattern) {
  expectRewriteEqualsWith<ast::PatternPredicateRewriter>(
      "MATCH (n) WHERE (n)-[:R]->(m) RETURN n",
      "MATCH (n) WHERE EXISTS { (n)-[:R]->(m) } RETURN n");
}

TEST(ExistentialSubqueryRewriterTest, PatternToMatchQuery) {
  expectRewriteEqualsWith<ast::ExistentialSubqueryRewriter>(
      "MATCH (n) WHERE EXISTS { (n)-[:R]->(m) } RETURN n",
      "MATCH (n) WHERE EXISTS { MATCH (n)-[:R]->(m) RETURN 1 } RETURN n");
}

TEST(OrderByAliasRewriterTest, RewritesExpressionToAlias) {
  expectRewriteEqualsWith<ast::OrderByAliasRewriter>(
      "MATCH (n) RETURN n.age AS age ORDER BY n.age",
      "MATCH (n) RETURN n.age AS age ORDER BY age");
}

TEST(CountStarRewriterTest, CountStarToFunction) {
  expectRewriteEqualsWith<ast::CountStarRewriter>("RETURN count(*)",
                                                  "RETURN count(1)");
}

TEST(ProjectionAliasRewriterTest, FillsAliasFromProperty) {
  expectRewriteEqualsWith<ast::ProjectionAliasRewriter>(
      "MATCH (n) RETURN n.name", "MATCH (n) RETURN n.name AS `n.name`");
}

TEST(ProjectionAliasRewriterTest, FillsAliasFromAddExpression) {
  expectRewriteEqualsWith<ast::ProjectionAliasRewriter>(
      "MATCH (n) RETURN n.age + 1",
      "MATCH (n) RETURN n.age + 1 AS `n.age + 1`");
}

TEST(AnonymousPatternNameRewriterTest, NamesUnnamedNodes) {
  expectRewriteEqualsWith<ast::AnonymousPatternNameRewriter>(
      "MATCH ()-[r]-() RETURN r", "MATCH (anon_0)-[r]-(anon_1) RETURN r");
}

TEST(AnonymousPatternNameRewriterTest, NamesUnnamedRelationships) {
  expectRewriteEqualsWith<ast::AnonymousPatternNameRewriter>(
      "MATCH ()--() RETURN 1", "MATCH (anon_0)-[anon_1]-(anon_2) RETURN 1");
}

TEST(RewriterPipelineTest, DefaultPipelineUsesReturnStar) {
  auto statement = parseOrFail("MATCH (n) RETURN *");
  auto expected_statement = parseOrFail("MATCH (n) RETURN n");

  ast::applyDefaultRewriters(*statement);

  EXPECT_TRUE(ast::ASTEqual::equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for pipeline";
}

TEST(RewriterPipelineTest, ParseAndRewriteUsesDefaultPipeline) {
  std::unique_ptr<ast::Statement> statement;
  ASSERT_NO_THROW(
      statement = ast::parseCypherAndRewrite("MATCH (n) RETURN *"));
  auto expected_statement = parseOrFail("MATCH (n) RETURN n");

  EXPECT_TRUE(ast::ASTEqual::equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for parseCypherAndRewrite";
}
