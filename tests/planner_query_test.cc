#include <gtest/gtest.h>

#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "planner/planner_query.h"

namespace {
std::unique_ptr<ast::Statement> parseOrFail(const std::string &query) {
  try {
    return ast::parseCypherAndRewrite(query);
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

TEST(PlannerQueryTest, BuildsGraphFromMatch) {
  auto statement = parseOrFail(
      "MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(b) "
      "WHERE a.age > 30 RETURN a, b");
  ASSERT_TRUE(statement);
}

TEST(PlannerQueryTest, AcceptsAnonymousPatternAfterRewrite) {
  auto statement = parseOrFail("MATCH ()-[]->() RETURN 1");
  ASSERT_TRUE(statement);
}
