#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"

namespace {

bool HasError(const std::vector<std::string> &errors,
              const std::string &expected) {
  return std::find(errors.begin(), errors.end(), expected) != errors.end();
}

}  // namespace

TEST(SemanticValidatorTest, UndefinedReturnVariable) {
  try {
    (void)ast::ParseCypher("MATCH (n) RETURN m");
    FAIL() << "expected semantic error";
  } catch (const ast::SemanticError &e) {
    EXPECT_TRUE(HasError(e.Errors(), "undefined variable: m"));
  } catch (const ast::ParseError &e) {
    FAIL() << "unexpected parse error: " << e.what();
  }
}

TEST(SemanticValidatorTest, WithProjectionScopes) {
  EXPECT_NO_THROW(ast::ParseCypher("MATCH (n) WITH n AS m RETURN m"));

  try {
    (void)ast::ParseCypher("MATCH (n) WITH n AS m RETURN n");
    FAIL() << "expected semantic error";
  } catch (const ast::SemanticError &e) {
    EXPECT_TRUE(HasError(e.Errors(), "undefined variable: n"));
  } catch (const ast::ParseError &e) {
    FAIL() << "unexpected parse error: " << e.what();
  }
}

TEST(SemanticValidatorTest, ComprehensionUsesOuterScope) {
  EXPECT_NO_THROW(
      ast::ParseCypher("MATCH (n) RETURN [x IN [1,2] WHERE x > n.age | x]"));
}
