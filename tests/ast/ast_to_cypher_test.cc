#include "ast/ast_to_cypher.h"

#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_node.h"
#include "common/exception.h"

namespace {

std::string ToCypherOrFail(const std::string &query) {
  try {
    auto statement = ast::ParseCypher(query);
    return ast::ToCypher(*statement);
  } catch (const common::Exception &e) {
    ADD_FAILURE() << "query error for query: " << query
                  << " message: " << e.what();
  }
  return {};
}

}  // namespace

TEST(AstToCypherTest, BasicMatchReturn) {
  const std::string query = "MATCH (n:Person {name: 'Bob'}) RETURN n";
  EXPECT_EQ(ToCypherOrFail(query), "MATCH (n:Person {name: 'Bob'}) RETURN n");
}

TEST(AstToCypherTest, ExplainQuery) {
  const std::string query = "EXPLAIN MATCH (n) RETURN n";
  auto statement = ast::ParseCypher(query);
  const auto *regular =
      dynamic_cast<const ast::RegularQuery *>(statement.get());
  ASSERT_NE(regular, nullptr);
  EXPECT_TRUE(regular->explain);
  EXPECT_EQ(ast::ToCypher(*statement), query);
}

TEST(AstToCypherTest, BinaryExpressionAddsParentheses) {
  const std::string query = "RETURN 1 + 2 * 3";
  EXPECT_EQ(ToCypherOrFail(query), "RETURN (1 + (2 * 3))");
}

TEST(AstToCypherTest, ListComprehensionWithWhereAndEval) {
  const std::string query = "RETURN [x IN [1,2,3] WHERE x > 1 | x * 2]";
  EXPECT_EQ(ToCypherOrFail(query),
            "RETURN [x IN [1, 2, 3] WHERE (x > 1) | (x * 2)]");
}

TEST(AstToCypherTest, ListComprehensionWithoutEval) {
  const std::string query = "RETURN [x IN [1,2,3]]";
  EXPECT_EQ(ToCypherOrFail(query), "RETURN [x IN [1, 2, 3]]");
}

TEST(AstToCypherTest, ReduceExpression) {
  const std::string query = "RETURN reduce(total=0, x IN [1,2,3] | total+x)";
  EXPECT_EQ(ToCypherOrFail(query),
            "RETURN reduce(total = 0, x IN [1, 2, 3] | (total + x))");
}

TEST(AstToCypherTest, RelationshipPatternDetails) {
  const std::string query =
      "MATCH (a)-[r:KNOWS|LIKES*1..3 {since: 2020}]->(b) RETURN r";
  EXPECT_EQ(ToCypherOrFail(query),
            "MATCH (a)-[r:KNOWS|LIKES*1..3 {since: 2020}]->(b) RETURN r");
}

TEST(AstToCypherTest, ShortestPathPatterns) {
  EXPECT_EQ(ToCypherOrFail(
                "MATCH p = shortestPath((a)-[r:KNOWS*1..3]->(b)) RETURN p"),
            "MATCH p = shortestPath((a)-[r:KNOWS*1..3]->(b)) RETURN p");
  EXPECT_EQ(ToCypherOrFail(
                "MATCH p = allShortestPaths((a)-[:KNOWS*0..]-(b)) RETURN p"),
            "MATCH p = allShortestPaths((a)-[:KNOWS*0..]-(b)) RETURN p");
}

TEST(AstToCypherTest, EscapesSymbolicNames) {
  const std::string query = "MATCH (`a-b` {`k-1`: 1}) RETURN `a-b`";
  EXPECT_EQ(ToCypherOrFail(query), "MATCH (`a-b` {`k-1`: 1}) RETURN `a-b`");
}
