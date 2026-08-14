#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "ast/ast_node.h"
#include "ast/ast_to_cypher.h"

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

TEST(SemanticValidatorTest, RejectsAmbiguousAggregationProjectionExpression) {
  ExpectSemanticError("MATCH (n) RETURN n + count(*) AS bad",
                      "ambiguous aggregation expression");
}

TEST(SemanticValidatorTest, AllowsAggregateSubexpressions) {
  EXPECT_NO_THROW(ast::ParseCypher("MATCH (n) RETURN count(n) + 3 AS count"));
  EXPECT_NO_THROW(
      ast::ParseCypher("MATCH (n) RETURN n.age, n.age + count(*) AS count"));
  EXPECT_NO_THROW(ast::ParseCypher(
      "MATCH (n) RETURN ALL(ok IN collect(n.age > 40) WHERE ok) AS okay"));
}

TEST(SemanticValidatorTest, RejectsAggregationInsideListComprehension) {
  ExpectSemanticError(
      "MATCH (n) RETURN [x IN [1, 2, 3] | count(*)] AS bad",
      "aggregation is not allowed in a comprehension predicate or mapping "
      "expression");
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

TEST(SemanticValidatorTest, AllowsAggregationAliasInWithWhere) {
  EXPECT_NO_THROW(
      ast::ParseCypher("MATCH (n) WHERE exists { MATCH (n)-->(m) "
                       "WITH n, count(*) AS numConnections "
                       "WHERE numConnections = 3 RETURN true } RETURN n"));
}

TEST(SemanticValidatorTest, RejectsAggregationInOrderBy) {
  ExpectSemanticError("MATCH (n) RETURN n ORDER BY count(n)",
                      "ORDER BY cannot contain aggregation");
}

TEST(SemanticValidatorTest, AllowsAggregationInOrderByAfterAggregation) {
  EXPECT_NO_THROW(
      ast::ParseCypher("MATCH (person) RETURN avg(person.age) AS avgAge "
                       "ORDER BY $age + avg(person.age) - 1000"));
  EXPECT_NO_THROW(
      ast::ParseCypher("MATCH (me)--(you) "
                       "RETURN me.age AS age, count(you.age) AS cnt "
                       "ORDER BY age, me.age + count(you.age)"));
}

TEST(SemanticValidatorTest, RejectsAmbiguousAggregationInOrderBy) {
  ExpectSemanticError(
      "MATCH (me)--(you) "
      "RETURN me.age + you.age, count(*) AS cnt "
      "ORDER BY me.age + you.age + count(*)",
      "ambiguous aggregation expression");
}

TEST(SemanticValidatorTest, RestrictsOrderByAfterDistinct) {
  constexpr std::string_view k_restricted_order_by =
      "In a WITH/RETURN with DISTINCT or an aggregation, it is not possible to "
      "access variables declared before the WITH/RETURN: n";

  ExpectSemanticError("MATCH (n) RETURN DISTINCT n.name AS name ORDER BY n.age",
                      std::string(k_restricted_order_by));
  ExpectSemanticError(
      "MATCH (n) WITH DISTINCT n.name AS name ORDER BY n.age RETURN name",
      std::string(k_restricted_order_by));
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

TEST(SemanticValidatorTest, RestrictsUnprojectedOrderByAggregationInputs) {
  ExpectSemanticError(
      "MATCH (n) RETURN count(*) AS c ORDER BY max(n.age)",
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

TEST(SemanticValidatorTest, RejectsUnknownProcedure) {
  ExpectSemanticError("CALL db.unknown()", "unknown procedure: db.unknown");
}

TEST(SemanticValidatorTest, RejectsProcedureArgumentCountMismatch) {
  ExpectSemanticError("CALL db.labels(1)", "db.labels expects 0 arguments");
}

TEST(SemanticValidatorTest, RejectsUnknownProcedureYieldField) {
  ExpectSemanticError("CALL db.labels() YIELD missing RETURN missing",
                      "unknown yield field for db.labels: missing");
}

TEST(SemanticValidatorTest, AcceptsRegisteredFunctionContracts) {
  EXPECT_NO_THROW(ast::ParseCypher(
      "RETURN COALESCE(null, 1) AS value, range(1, 3) AS short_range, "
      "range(1, 3, 2) AS stepped_range, abs(-1) AS absolute, rand() AS random, "
      "tail([1, 2]) AS rest, last([1, 2]) AS final, "
      "reverse('abc') AS reversed, substring('abc', 1, 1) AS middle"));
  EXPECT_NO_THROW(
      ast::ParseCypher("UNWIND [1, 1] AS x RETURN count(DISTINCT x) AS c"));
}

TEST(SemanticValidatorTest, RejectsUnknownAndUnimplementedFunctions) {
  ExpectSemanticError("RETURN unknownFunction(1) AS value",
                      "unknown function: unknownFunction");
  ExpectSemanticError("RETURN startNode(null) AS value",
                      "unknown function: startNode");
}

TEST(SemanticValidatorTest, RejectsFunctionArgumentCountMismatch) {
  ExpectSemanticError("RETURN size() AS value", "size() expects 1 argument");
  ExpectSemanticError("RETURN range(1) AS value",
                      "range() expects 2 to 3 arguments");
  ExpectSemanticError("RETURN split('a', ',', 'extra') AS value",
                      "split() expects 2 arguments");
  ExpectSemanticError("RETURN coalesce() AS value",
                      "coalesce() expects at least 1 argument");
  ExpectSemanticError("RETURN count() AS value", "count() expects 1 argument");
  ExpectSemanticError("RETURN rand(1) AS value", "rand() expects 0 arguments");
  ExpectSemanticError("RETURN substring('abc') AS value",
                      "substring() expects 2 to 3 arguments");
}

TEST(SemanticValidatorTest, RejectsNonDeterministicFunctionInAggregation) {
  ExpectSemanticError(
      "RETURN count(rand())",
      "non-deterministic function is not allowed inside aggregation: rand");
}

TEST(SemanticValidatorTest, RejectsDistinctForScalarFunctions) {
  ExpectSemanticError("RETURN size(DISTINCT [1]) AS value",
                      "DISTINCT is only supported for aggregate functions");
}

TEST(SemanticValidatorTest, ParsesSmallestIntegerWithoutOverflow) {
  auto statement = ast::ParseCypher("RETURN -9223372036854775808 AS value");
  EXPECT_EQ(ast::ToCypher(*statement), "RETURN -9223372036854775808 AS value");
}

TEST(SemanticValidatorTest, RejectsIntegerLiteralOutsideInt64Range) {
  try {
    (void)ast::ParseCypher("RETURN 9223372036854775808 AS value");
    FAIL() << "expected parse error";
  } catch (const ast::ParseError &e) {
    EXPECT_TRUE(HasError(e.Errors(), "integer literal is out of range"));
  }

  try {
    (void)ast::ParseCypher("RETURN -9223372036854775809 AS value");
    FAIL() << "expected parse error";
  } catch (const ast::ParseError &e) {
    EXPECT_TRUE(HasError(e.Errors(), "integer literal is out of range"));
  }
}

TEST(SemanticValidatorTest, RejectsPatternVariableTypeConflicts) {
  ExpectSemanticError("MATCH (n) MATCH ()-[n]->() RETURN n",
                      "variable type conflict: n");
  ExpectSemanticError("MATCH p = ()-->() MATCH (p) RETURN p",
                      "variable type conflict: p");
  ExpectSemanticError("WITH 1 AS n MATCH (n) RETURN n",
                      "variable type conflict: n");
}

TEST(SemanticValidatorTest, RejectsRebindingInUpdatingPatterns) {
  ExpectSemanticError("MATCH (n) CREATE (n)", "variable already bound: n");
  ExpectSemanticError("MATCH ()-[r]->() CREATE ()-[r]->()",
                      "variable already bound: r");
  ExpectSemanticError("CREATE (n:Foo), (n:Bar)", "variable already bound: n");
  EXPECT_NO_THROW(ast::ParseCypher("MATCH (n) CREATE (n)-[:R]->(m)"));
}

TEST(SemanticValidatorTest, RejectsStaticallyInvalidArgumentTypes) {
  ExpectSemanticError("RETURN 1 AND true",
                      "invalid argument type: AND requires boolean operands");
  ExpectSemanticError(
      "RETURN NOT 'value'",
      "invalid argument type: NOT requires a boolean expression");
  ExpectSemanticError(
      "RETURN 1 IN true",
      "invalid argument type: IN requires a list on the right-hand side");
  ExpectSemanticError("WITH 1 AS value RETURN value.name",
                      "invalid argument type: property access requires a node, "
                      "relationship, or map");
  ExpectSemanticError(
      "MATCH (n) RETURN n SKIP 1.5",
      "invalid argument type: SKIP requires an integer expression");
  ExpectSemanticError(
      "MATCH (n) DELETE 1 + 1",
      "invalid argument type: DELETE requires a node, relationship, or path");
}

TEST(SemanticValidatorTest, RejectsKnownInvalidFunctionArguments) {
  ExpectSemanticError(
      "MATCH p = (n) RETURN labels(p)",
      "invalid argument type: labels() argument has invalid type");
  ExpectSemanticError(
      "MATCH (n) RETURN type(n)",
      "invalid argument type: type() argument has invalid type");
  ExpectSemanticError(
      "RETURN properties(1)",
      "invalid argument type: properties() argument has invalid type");
  ExpectSemanticError(
      "MATCH p = (n)-[:R]->(m) RETURN size(p)",
      "invalid argument type: size() argument has invalid type");
  ExpectSemanticError("RETURN abs('x')",
                      "invalid argument type: abs() argument has invalid type");
  ExpectSemanticError(
      "RETURN tail('x')",
      "invalid argument type: tail() argument has invalid type");
  ExpectSemanticError(
      "RETURN substring('abc', '1')",
      "invalid argument type: substring() argument has invalid type");
}

TEST(SemanticValidatorTest, AllowsWithWhereToUseIncomingVariables) {
  auto statement = ast::ParseCypherAndRewrite(
      "MATCH (a)-[r]->(b) OPTIONAL MATCH (a)-[r2]->(c) "
      "WITH c WHERE r IS NULL RETURN c");
  ASSERT_TRUE(statement);
}

TEST(SemanticValidatorTest, RejectsQuantifierPredicateTypeMismatch) {
  ExpectSemanticError(
      "RETURN all(x IN ['one'] WHERE x % 2 = 0)",
      "invalid argument type: modulo requires numeric operands");
}

TEST(SemanticValidatorTest, RejectsInvalidUpdatingRelationshipPatterns) {
  ExpectSemanticError("CREATE ()-->()",
                      "CREATE relationships require exactly one type");
  ExpectSemanticError("CREATE ()-[:A|:B]->()",
                      "CREATE relationships require exactly one type");
  ExpectSemanticError("CREATE ()-[:A]-()",
                      "CREATE relationships must have one direction");
  ExpectSemanticError("MERGE ()-->()",
                      "MERGE relationships require exactly one type");
  EXPECT_NO_THROW(ast::ParseCypher("MERGE ()-[:A]-()"));
}

TEST(SemanticValidatorTest, RejectsParameterPropertiesInMergePatterns) {
  ExpectSemanticError("MERGE (n $properties) RETURN n",
                      "MERGE node properties cannot be a parameter");
  ExpectSemanticError("MERGE (a)-[r:R $properties]->(b) RETURN r",
                      "MERGE relationship properties cannot be a parameter");
}

TEST(SemanticValidatorTest, RejectsRepeatedRelationshipInOnePattern) {
  ExpectSemanticError("MATCH (a)-[r]->()-[r]->(a) RETURN r",
                      "relationship variable is reused in one pattern: r");
}

TEST(SemanticValidatorTest, RejectsInvalidPaginationExpressions) {
  ExpectSemanticError("MATCH (n) RETURN n SKIP n.count",
                      "SKIP expression must be constant");
  ExpectSemanticError("MATCH (n) RETURN n LIMIT n.count",
                      "LIMIT expression must be constant");
  ExpectSemanticError("RETURN 1 SKIP -1",
                      "SKIP expression must be non-negative");
  ExpectSemanticError("RETURN 1 LIMIT -1",
                      "LIMIT expression must be non-negative");
}

TEST(SemanticValidatorTest, RejectsInvalidProjectionComposition) {
  ExpectSemanticError("RETURN 1 AS value, 2 AS value",
                      "duplicate projection column: value");
  ExpectSemanticError("WITH 1 AS value, 2 AS value RETURN value",
                      "duplicate projection column: value");
  ExpectSemanticError("MATCH (n) WITH n, count(*) RETURN n",
                      "WITH expressions must be aliased");
  ExpectSemanticError("MATCH (n) RETURN (n)-->()",
                      "pattern expressions are not allowed in projections");
}

TEST(SemanticValidatorTest, RejectsMixedUnionComposition) {
  ExpectSemanticError(
      "RETURN 1 AS value UNION RETURN 2 AS value UNION ALL RETURN 3 AS value",
      "cannot mix UNION and UNION ALL");
}

TEST(SemanticValidatorTest, RejectsNewVariablesInPatternPredicates) {
  ExpectSemanticError("MATCH (n) WHERE (n)-[r]->() RETURN n",
                      "undefined variable: r");
  ExpectSemanticError("MATCH (n) WHERE (n)-->(m) RETURN n",
                      "undefined variable: m");
}

TEST(SemanticValidatorTest, RejectsUpdatesInExistentialSubquery) {
  ExpectSemanticError(
      "MATCH (n) WHERE exists { MATCH (n)-->(m) SET m.value = 1 } RETURN n",
      "existential subquery cannot contain updates");
}
