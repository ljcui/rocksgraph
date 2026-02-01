#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_to_cypher.h"

namespace {

std::string toCypherOrFail(const std::string &query) {
  auto result = ast::parseCypher(query);
  EXPECT_TRUE(result.errors.empty()) << "parse errors for query: " << query;
  EXPECT_TRUE(result.statement != nullptr) << "null statement for query: " << query;
  if (!result.statement) {
    return {};
  }
  return ast::toCypher(*result.statement);
}

}  // namespace

TEST(AstToCypherTest, BasicMatchReturn) {
  const std::string query = "MATCH (n:Person {name: 'Bob'}) RETURN n";
  EXPECT_EQ(toCypherOrFail(query), "MATCH (n:Person {name: 'Bob'}) RETURN n");
}

TEST(AstToCypherTest, BinaryExpressionAddsParentheses) {
  const std::string query = "RETURN 1 + 2 * 3";
  EXPECT_EQ(toCypherOrFail(query), "RETURN (1 + (2 * 3))");
}

TEST(AstToCypherTest, ListComprehensionWithWhereAndEval) {
  const std::string query = "RETURN [x IN [1,2,3] WHERE x > 1 | x * 2]";
  EXPECT_EQ(toCypherOrFail(query),
            "RETURN [x IN [1, 2, 3] WHERE (x > 1) | (x * 2)]");
}

TEST(AstToCypherTest, RelationshipPatternDetails) {
  const std::string query =
      "MATCH (a)-[r:KNOWS|LIKES*1..3 {since: 2020}]->(b) RETURN r";
  EXPECT_EQ(toCypherOrFail(query),
            "MATCH (a)-[r:KNOWS|LIKES*1..3 {since: 2020}]->(b) RETURN r");
}

TEST(AstToCypherTest, EscapesSymbolicNames) {
  const std::string query = "MATCH (`a-b` {`k-1`: 1}) RETURN `a-b`";
  EXPECT_EQ(toCypherOrFail(query),
            "MATCH (`a-b` {`k-1`: 1}) RETURN `a-b`");
}
