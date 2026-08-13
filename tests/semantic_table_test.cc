#include "ast/semantic_table.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"

namespace {

std::unique_ptr<ast::Statement> ParseOrFail(const std::string &query) {
  try {
    return ast::ParseCypherAndRewrite(query);
  } catch (const ast::ParseError &e) {
    ADD_FAILURE() << "parse errors for query: " << query
                  << " message: " << e.what();
  } catch (const ast::SemanticError &e) {
    ADD_FAILURE() << "semantic errors for query: " << query
                  << " message: " << e.what();
  }
  return {};
}

bool Contains(const std::unordered_set<std::string> &set,
              const std::string &value) {
  return set.find(value) != set.end();
}

}  // namespace

TEST(SemanticTableTest, RecordsPatternVariableTypesAndWhereDependencies) {
  auto statement = ParseOrFail(
      "MATCH (a)-[r]->(b) WHERE a.age > b.age RETURN a AS person, r AS rel, "
      "b.age AS age");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.VariableType("a"), ast::SemanticVariableType::kNode);
  EXPECT_EQ(table.VariableType("r"), ast::SemanticVariableType::kRelationship);
  EXPECT_EQ(table.VariableType("b"), ast::SemanticVariableType::kNode);
  EXPECT_EQ(table.VariableType("person"), ast::SemanticVariableType::kNode);
  EXPECT_EQ(table.VariableType("rel"),
            ast::SemanticVariableType::kRelationship);
  EXPECT_EQ(table.VariableType("age"), ast::SemanticVariableType::kScalar);

  const auto &regular = ast::CastAst<ast::RegularQuery>(*statement);
  ASSERT_TRUE(regular.single_query);
  const auto &single =
      ast::CastAst<ast::SinglePartQuery>(*regular.single_query);
  ASSERT_EQ(single.reading_clauses.size(), 1U);
  const auto &match = ast::CastAst<ast::Match>(*single.reading_clauses[0]);
  ASSERT_TRUE(match.where);

  const auto &dependencies = table.ExpressionDependencies(*match.where);
  EXPECT_TRUE(Contains(dependencies, "a"));
  EXPECT_TRUE(Contains(dependencies, "b"));
  EXPECT_FALSE(Contains(dependencies, "r"));

  ASSERT_TRUE(single.return_clause);
  ASSERT_TRUE(single.return_clause->body);
  EXPECT_EQ(table.ProjectionOutputs(*single.return_clause->body),
            std::vector<std::string>({"person", "rel", "age"}));
}

TEST(SemanticTableTest, TracksWithProjectionOutputsAndTypesAcrossScope) {
  auto statement = ParseOrFail(
      "MATCH (n)-[r]->(m) WITH n AS person, r, m.age AS age "
      "WHERE age > 30 RETURN person, r, age");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.VariableType("person"), ast::SemanticVariableType::kNode);
  EXPECT_EQ(table.VariableType("r"), ast::SemanticVariableType::kRelationship);
  EXPECT_EQ(table.VariableType("age"), ast::SemanticVariableType::kScalar);

  const auto &regular = ast::CastAst<ast::RegularQuery>(*statement);
  ASSERT_TRUE(regular.single_query);
  const auto &multi = ast::CastAst<ast::MultiPartQuery>(*regular.single_query);
  ASSERT_EQ(multi.parts.size(), 1U);
  ASSERT_TRUE(multi.parts[0].with_clause);
  ASSERT_TRUE(multi.parts[0].with_clause->body);
  ASSERT_TRUE(multi.parts[0].with_clause->where);

  EXPECT_EQ(table.ProjectionOutputs(*multi.parts[0].with_clause->body),
            std::vector<std::string>({"person", "r", "age"}));
  const auto &with_dependencies =
      table.ExpressionDependencies(*multi.parts[0].with_clause->where);
  EXPECT_TRUE(Contains(with_dependencies, "age"));
  EXPECT_FALSE(Contains(with_dependencies, "m"));

  ASSERT_TRUE(multi.final_single_part_query);
  ASSERT_TRUE(multi.final_single_part_query->return_clause);
  ASSERT_TRUE(multi.final_single_part_query->return_clause->body);
  EXPECT_EQ(table.ProjectionOutputs(
                *multi.final_single_part_query->return_clause->body),
            std::vector<std::string>({"person", "r", "age"}));
}

TEST(SemanticTableTest, MarksAggregationExpressions) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN count(*) AS c, collect(n) AS nodes");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  const auto &regular = ast::CastAst<ast::RegularQuery>(*statement);
  ASSERT_TRUE(regular.single_query);
  const auto &single =
      ast::CastAst<ast::SinglePartQuery>(*regular.single_query);
  ASSERT_TRUE(single.return_clause);
  ASSERT_TRUE(single.return_clause->body);
  ASSERT_EQ(single.return_clause->body->items.size(), 2U);

  const auto &count_item = *single.return_clause->body->items[0];
  const auto &collect_item = *single.return_clause->body->items[1];
  ASSERT_TRUE(count_item.expression);
  ASSERT_TRUE(collect_item.expression);

  EXPECT_TRUE(table.ContainsAggregation(*count_item.expression));
  EXPECT_TRUE(table.ContainsAggregation(*collect_item.expression));
  EXPECT_TRUE(table.ExpressionDependencies(*count_item.expression).empty());
  EXPECT_TRUE(
      Contains(table.ExpressionDependencies(*collect_item.expression), "n"));
  EXPECT_EQ(table.VariableType("c"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("nodes"), ast::SemanticVariableType::kList);
}

TEST(SemanticTableTest, InfersFunctionProjectionResultTypes) {
  auto statement = ParseOrFail(
      "MATCH (n) RETURN collect(n) AS nodes, count(*) AS rows, "
      "count(n) AS counted, sum(n.age) AS total, coalesce(n.name, 'n/a') AS "
      "name");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.VariableType("nodes"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("rows"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("counted"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("total"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("name"), ast::SemanticVariableType::kScalar);
}

TEST(SemanticTableTest, UsesFunctionSignatureTableForProjectionTypes) {
  auto statement = ParseOrFail(
      "MATCH (a)-[r]->(b) RETURN labels(a) AS labels, keys({name: 'Ada'}) AS "
      "keys, range(1, 3) AS nums, properties(a) AS props, startNode(r) AS "
      "start, endNode(r) AS finish, type(r) AS rel_type, toString(a.name) AS "
      "name");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.KnownFunctionResultType("labels"),
            ast::SemanticVariableType::kList);
  EXPECT_EQ(table.KnownFunctionResultType("properties"),
            ast::SemanticVariableType::kMap);
  EXPECT_FALSE(table.KnownFunctionResultType("unknownFunction").has_value());

  EXPECT_EQ(table.VariableType("labels"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("keys"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("nums"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("props"), ast::SemanticVariableType::kMap);
  EXPECT_EQ(table.VariableType("start"), ast::SemanticVariableType::kNode);
  EXPECT_EQ(table.VariableType("finish"), ast::SemanticVariableType::kNode);
  EXPECT_EQ(table.VariableType("rel_type"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("name"), ast::SemanticVariableType::kScalar);
}

TEST(SemanticTableTest, UsesBuiltinProcedureRegistryForYieldTypes) {
  auto statement =
      ParseOrFail("CALL db.labels() YIELD label AS labelName RETURN labelName");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.KnownProcedureYieldType("db.labels", "label"),
            ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.KnownProcedureYieldFields("db.labels"),
            std::vector<std::string>({"label"}));
  EXPECT_EQ(table.KnownProcedureReadOnly("db.labels"),
            std::optional<bool>(true));
  EXPECT_FALSE(
      table.KnownProcedureYieldType("db.labels", "missing").has_value());
  EXPECT_FALSE(table.KnownProcedureReadOnly("db.unknown").has_value());
  EXPECT_FALSE(
      table.KnownProcedureYieldType("db.unknown", "label").has_value());
  EXPECT_EQ(table.VariableType("labelName"),
            ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.KnownProcedureYieldFields("dbms.procedures"),
            (std::vector<std::string>{"name", "signature", "description",
                                      "mode", "worksOnSystem"}));
  EXPECT_EQ(table.KnownProcedureYieldType("DBMS.PROCEDURES", "worksOnSystem"),
            ast::SemanticVariableType::kScalar);
}

TEST(SemanticTableTest, InfersStructuredExpressionResultTypes) {
  auto statement = ParseOrFail(
      "RETURN [1, 2, 3][0] AS first, [{a: 1}][0] AS first_map, [[1]][0] AS "
      "first_list, [1, 2, 3][0..2] AS slice, CASE WHEN true THEN {a: 1} ELSE "
      "{b: 2} END AS case_map, CASE WHEN true THEN [1] ELSE [2] END AS "
      "case_list, true AND false AS flag, 1 < 2 AS cmp");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.VariableType("first"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("first_map"), ast::SemanticVariableType::kMap);
  EXPECT_EQ(table.VariableType("first_list"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("slice"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("case_map"), ast::SemanticVariableType::kMap);
  EXPECT_EQ(table.VariableType("case_list"), ast::SemanticVariableType::kList);
  EXPECT_EQ(table.VariableType("flag"), ast::SemanticVariableType::kScalar);
  EXPECT_EQ(table.VariableType("cmp"), ast::SemanticVariableType::kScalar);
}

TEST(SemanticTableTest, TracksVariableTypesAtExpressionScopes) {
  auto statement = ParseOrFail("MATCH (n) WHERE n:Person WITH 1 AS n RETURN n");
  ASSERT_TRUE(statement);

  ast::SemanticTable table = ast::AnalyzeSemanticTable(*statement);

  EXPECT_EQ(table.VariableType("n"), ast::SemanticVariableType::kUnknown);

  const auto &regular = ast::CastAst<ast::RegularQuery>(*statement);
  ASSERT_TRUE(regular.single_query);
  const auto &multi = ast::CastAst<ast::MultiPartQuery>(*regular.single_query);
  ASSERT_EQ(multi.parts.size(), 1U);
  ASSERT_EQ(multi.parts[0].reading_clauses.size(), 1U);
  const auto &match =
      ast::CastAst<ast::Match>(*multi.parts[0].reading_clauses[0]);
  ASSERT_TRUE(match.where);

  EXPECT_EQ(table.VariableTypeAt(*match.where, "n"),
            ast::SemanticVariableType::kNode);

  ASSERT_TRUE(multi.final_single_part_query);
  ASSERT_TRUE(multi.final_single_part_query->return_clause);
  ASSERT_TRUE(multi.final_single_part_query->return_clause->body);
  ASSERT_EQ(multi.final_single_part_query->return_clause->body->items.size(),
            1U);
  const auto &return_item =
      *multi.final_single_part_query->return_clause->body->items[0];
  ASSERT_TRUE(return_item.expression);

  EXPECT_EQ(table.VariableTypeAt(*return_item.expression, "n"),
            ast::SemanticVariableType::kScalar);
}
