#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_equal.h"
#include "ast/rewriters/return_star_rewriter.h"
#include "ast/rewriters/rewriter_pipeline.h"

namespace {

std::unique_ptr<ast::Statement> parseOrFail(const std::string &query) {
  auto result = ast::parseCypher(query);
  EXPECT_TRUE(result.errors.empty()) << "parse errors for query: " << query;
  EXPECT_TRUE(result.statement != nullptr) << "null statement for query: " << query;
  return std::move(result.statement);
}

void expectRewriteEquals(const std::string &input,
                         const std::string &expected) {
  auto statement = parseOrFail(input);
  auto expected_statement = parseOrFail(expected);

  ast::ReturnStarRewriter rewriter;
  rewriter.rewrite(*statement);

  EXPECT_TRUE(ast::ASTEqual::equal(statement.get(), expected_statement.get()))
      << "rewrite mismatch for input: " << input;
}

}  // namespace

TEST(ReturnStarRewriterTest, MatchReturnStar) {
  expectRewriteEquals("MATCH (n) RETURN *", "MATCH (n) RETURN n");
}

TEST(ReturnStarRewriterTest, UnwindReturnStar) {
  expectRewriteEquals("UNWIND [1, 2] AS x RETURN *", "UNWIND [1, 2] AS x RETURN x");
}

TEST(ReturnStarRewriterTest, WithStarThenReturnStar) {
  expectRewriteEquals("MATCH (n) WITH * RETURN *",
                      "MATCH (n) WITH n RETURN n");
}

TEST(ReturnStarRewriterTest, WithAliasReturnStar) {
  expectRewriteEquals("MATCH (n) WITH n AS m RETURN *",
                      "MATCH (n) WITH n AS m RETURN m");
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
