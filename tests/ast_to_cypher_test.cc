#include "ast/ast_to_cypher.h"

#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"

namespace {

std::string ToCypherOrFail(const std::string &query) {
  try {
    auto statement = ast::parseCypher(query);
    return ast::toCypher(*statement);
  } catch (const ast::ParseError &e) {
    ADD_FAILURE() << "parse errors for query: " << query
                  << " message: " << e.what();
  } catch (const ast::SemanticError &e) {
    ADD_FAILURE() << "semantic errors for query: " << query
                  << " message: " << e.what();
  }
  return {};
}

}  // namespace

TEST(AstToCypherTest, BasicMatchReturn) {
  const std::string query = "MATCH (n:Person {name: 'Bob'}) RETURN n";
  EXPECT_EQ(ToCypherOrFail(query), "MATCH (n:Person {name: 'Bob'}) RETURN n");
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

TEST(AstToCypherTest, RelationshipPatternDetails) {
  const std::string query =
      "MATCH (a)-[r:KNOWS|LIKES*1..3 {since: 2020}]->(b) RETURN r";
  EXPECT_EQ(ToCypherOrFail(query),
            "MATCH (a)-[r:KNOWS|LIKES*1..3 {since: 2020}]->(b) RETURN r");
}

TEST(AstToCypherTest, EscapesSymbolicNames) {
  const std::string query = "MATCH (`a-b` {`k-1`: 1}) RETURN `a-b`";
  EXPECT_EQ(ToCypherOrFail(query), "MATCH (`a-b` {`k-1`: 1}) RETURN `a-b`");
}
