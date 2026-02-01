#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_equal.h"
#include "ast/rewriters/comparison_chain_rewriter.h"
#include "ast/rewriters/parenthesized_expression_rewriter.h"
#include "ast/rewriters/pattern_predicate_rewriter.h"
#include "ast/rewriters/return_star_rewriter.h"
#include "ast/rewriters/rewriter_pipeline.h"

namespace {

std::unique_ptr<ast::Statement> parseOrFail(const std::string &query) {
  auto result = ast::parseCypher(query);
  EXPECT_TRUE(result.errors.empty()) << "parse errors for query: " << query;
  EXPECT_TRUE(result.statement != nullptr) << "null statement for query: " << query;
  return std::move(result.statement);
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
  expectRewriteEqualsWith<ast::ReturnStarRewriter>(
      "MATCH (n) WITH * RETURN *", "MATCH (n) WITH n RETURN n");
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
  expectRewriteEqualsWith<ast::ParenthesizedExpressionRewriter>(
      "RETURN ((1))", "RETURN 1");
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

TEST(RewriterPipelineTest, DefaultPipelineUsesReturnStar) {
  auto statement = parseOrFail("MATCH (n) RETURN *");
  auto expected_statement = parseOrFail("MATCH (n) RETURN n");

  ast::applyDefaultRewriters(*statement);

  EXPECT_TRUE(ast::ASTEqual::equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for pipeline";
}

TEST(RewriterPipelineTest, ParseAndRewriteUsesDefaultPipeline) {
  auto result = ast::parseCypherAndRewrite("MATCH (n) RETURN *");
  EXPECT_TRUE(result.errors.empty()) << "parse errors for rewrite wrapper";
  ASSERT_TRUE(result.statement != nullptr);
  auto expected_statement = parseOrFail("MATCH (n) RETURN n");

  EXPECT_TRUE(ast::ASTEqual::equal(result.statement.get(), expected_statement.get()))
      << "rewrite mismatch for parseCypherAndRewrite";
}
