#include "ast/semantic_table.h"

#include <gtest/gtest.h>

#include <memory>
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
  EXPECT_EQ(table.VariableType("nodes"), ast::SemanticVariableType::kScalar);
}
