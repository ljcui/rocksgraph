#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../ast_rewriter.h"

namespace ast {

// Isolates aggregate subexpressions in a preceding WITH so the original
// projection only combines already computed grouping and aggregate values.
class AggregationExpressionRewriter : public ASTRewriter {
 public:
  void Visit(RegularQuery &node) override;
  void Visit(UnionPart &node) override;

 private:
  struct NamedExpression {
    std::unique_ptr<Expression> expression;
    std::string alias;
  };

  class ExpressionAliasRewriter;

  void Prepare(ASTNode &node);
  void RewriteSingleQuery(std::unique_ptr<SingleQuery> &query);
  [[nodiscard]] std::unique_ptr<SingleQuery> RewriteSinglePart(
      std::unique_ptr<SinglePartQuery> query);
  void RewriteMultiPart(MultiPartQuery &node);
  [[nodiscard]] bool RewriteProjection(ProjectionBody &body,
                                       std::unique_ptr<With> &prefix);
  void AddExpression(const Expression &expression,
                     const std::string &preferred_alias,
                     std::vector<NamedExpression> &out);
  void AddAggregateExpressions(const Expression &expression,
                               std::vector<NamedExpression> &out);
  [[nodiscard]] std::unique_ptr<With> BuildPrefix(
      const std::vector<NamedExpression> &expressions) const;
  [[nodiscard]] std::string NextAlias();
  [[nodiscard]] bool ContainsAggregation(const Expression *expression) const;
  [[nodiscard]] bool IsTopLevelAggregation(const Expression *expression) const;
  [[nodiscard]] bool IsNotConstantExpression(
      const Expression *expression) const;

  std::unordered_set<std::string> used_names_;
  std::size_t next_alias_id_ = 0;
  bool prepared_ = false;
};

}  // namespace ast
