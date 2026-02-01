#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "ast/ast_builder.h"

namespace {

bool hasError(const std::vector<std::string> &errors,
              const std::string &expected) {
  return std::find(errors.begin(), errors.end(), expected) != errors.end();
}

}  // namespace

TEST(SemanticValidatorTest, UndefinedReturnVariable) {
  auto result = ast::parseCypher("MATCH (n) RETURN m");
  EXPECT_FALSE(result.errors.empty());
  EXPECT_TRUE(hasError(result.errors, "undefined variable: m"));
}

TEST(SemanticValidatorTest, WithProjectionScopes) {
  auto ok = ast::parseCypher("MATCH (n) WITH n AS m RETURN m");
  EXPECT_TRUE(ok.errors.empty());

  auto bad = ast::parseCypher("MATCH (n) WITH n AS m RETURN n");
  EXPECT_FALSE(bad.errors.empty());
  EXPECT_TRUE(hasError(bad.errors, "undefined variable: n"));
}

TEST(SemanticValidatorTest, ComprehensionUsesOuterScope) {
  auto result = ast::parseCypher(
      "MATCH (n) RETURN [x IN [1,2] WHERE x > n.age | x]");
  EXPECT_TRUE(result.errors.empty());
}
