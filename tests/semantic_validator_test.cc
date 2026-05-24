#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"

namespace {

bool HasError(const std::vector<std::string> &errors,
              const std::string &expected) {
  return std::find(errors.begin(), errors.end(), expected) != errors.end();
}

void ExpectSemanticError(const std::string &query,
                         const std::string &expected_error) {
  try {
    (void)ast::ParseCypher(query);
    FAIL() << "expected semantic error";
  } catch (const ast::SemanticError &e) {
    EXPECT_TRUE(HasError(e.Errors(), expected_error));
  } catch (const ast::ParseError &e) {
    FAIL() << "unexpected parse error: " << e.what();
  }
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

TEST(SemanticValidatorTest, RejectsUnionColumnCountMismatch) {
  try {
    (void)ast::ParseCypher("RETURN 1 AS a UNION RETURN 1 AS b, 2 AS c");
    FAIL() << "expected semantic error";
  } catch (const ast::SemanticError &e) {
    EXPECT_TRUE(HasError(
        e.Errors(), "UNION branches must return the same number of columns"));
  } catch (const ast::ParseError &e) {
    FAIL() << "unexpected parse error: " << e.what();
  }
}

TEST(SemanticValidatorTest, RejectsUnionColumnNameMismatch) {
  try {
    (void)ast::ParseCypher("RETURN 1 AS a UNION RETURN 1 AS b");
    FAIL() << "expected semantic error";
  } catch (const ast::SemanticError &e) {
    EXPECT_TRUE(HasError(
        e.Errors(),
        "UNION branches must return the same column names by position"));
  } catch (const ast::ParseError &e) {
    FAIL() << "unexpected parse error: " << e.what();
  }
}

TEST(SemanticValidatorTest, RejectsUnionMismatchAfterReturnStarRewrite) {
  try {
    (void)ast::ParseCypherAndRewrite(
        "MATCH (n) RETURN * UNION MATCH (m) RETURN m AS x");
    FAIL() << "expected semantic error";
  } catch (const ast::SemanticError &e) {
    EXPECT_TRUE(HasError(
        e.Errors(),
        "UNION branches must return the same column names by position"));
  } catch (const ast::ParseError &e) {
    FAIL() << "unexpected parse error: " << e.what();
  }
}

TEST(SemanticValidatorTest, AllowsTopLevelAggregationProjectionItems) {
  EXPECT_NO_THROW(ast::ParseCypher("MATCH (n) RETURN n, count(*) AS c"));
  EXPECT_NO_THROW(ast::ParseCypher("MATCH (n) RETURN count(n) AS c"));
}

TEST(SemanticValidatorTest, RejectsMixedAggregationProjectionExpression) {
  ExpectSemanticError("MATCH (n) RETURN n + count(*) AS bad",
                      "aggregation must be a top-level projection item");
}

TEST(SemanticValidatorTest, RejectsNestedAggregation) {
  ExpectSemanticError("MATCH (n) RETURN count(sum(n.age)) AS bad",
                      "nested aggregation is not allowed");
}

TEST(SemanticValidatorTest, RejectsAggregationInWhere) {
  ExpectSemanticError("MATCH (n) WHERE count(n) > 1 RETURN n",
                      "WHERE cannot contain aggregation");
}

TEST(SemanticValidatorTest, RejectsAggregationInWithWhere) {
  ExpectSemanticError("MATCH (n) WITH n WHERE count(n) > 1 RETURN n",
                      "WHERE cannot contain aggregation");
}

TEST(SemanticValidatorTest, RejectsAggregationInOrderBy) {
  ExpectSemanticError("MATCH (n) RETURN n ORDER BY count(n)",
                      "ORDER BY cannot contain aggregation");
}

TEST(SemanticValidatorTest, RestrictsOrderByAfterDistinct) {
  constexpr char kRestrictedOrderBy[] =
      "In a WITH/RETURN with DISTINCT or an aggregation, it is not possible to "
      "access variables declared before the WITH/RETURN: n";

  ExpectSemanticError("MATCH (n) RETURN DISTINCT n.name AS name ORDER BY n.age",
                      kRestrictedOrderBy);
  ExpectSemanticError(
      "MATCH (n) WITH DISTINCT n.name AS name ORDER BY n.age RETURN name",
      kRestrictedOrderBy);
}

TEST(SemanticValidatorTest, AllowsOrderByProjectedValuesAfterDistinct) {
  EXPECT_NO_THROW(ast::ParseCypher(
      "MATCH (n) RETURN DISTINCT n.name AS name ORDER BY name"));
  EXPECT_NO_THROW(ast::ParseCypherAndRewrite(
      "MATCH (n) RETURN DISTINCT n.name AS name ORDER BY n.name"));
  EXPECT_NO_THROW(ast::ParseCypherAndRewrite(
      "MATCH (n) RETURN DISTINCT n.name AS name ORDER BY size(n.name)"));
  EXPECT_NO_THROW(
      ast::ParseCypher("MATCH (n) RETURN DISTINCT n ORDER BY n.age"));
}

TEST(SemanticValidatorTest, RestrictsOrderByAfterAggregation) {
  ExpectSemanticError(
      "MATCH (n) RETURN n.name AS name, count(*) AS c ORDER BY n.age",
      "In a WITH/RETURN with DISTINCT or an aggregation, it is not possible to "
      "access variables declared before the WITH/RETURN: n");
}

TEST(SemanticValidatorTest, AllowsOrderByGroupingValuesAfterAggregation) {
  EXPECT_NO_THROW(ast::ParseCypherAndRewrite(
      "MATCH (n) RETURN n.name AS name, count(*) AS c ORDER BY n.name"));
  EXPECT_NO_THROW(ast::ParseCypher(
      "MATCH (n) RETURN n AS node, count(*) AS c ORDER BY node.age"));
}

TEST(SemanticValidatorTest, RejectsAggregationInPagination) {
  ExpectSemanticError("RETURN 1 AS x SKIP count(x)",
                      "SKIP cannot contain aggregation");
  ExpectSemanticError("RETURN 1 AS x LIMIT count(x)",
                      "LIMIT cannot contain aggregation");
}
