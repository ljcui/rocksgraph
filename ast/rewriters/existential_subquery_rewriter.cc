#include "existential_subquery_rewriter.h"

#include <utility>

namespace ast {

void ExistentialSubqueryRewriter::rewriteExpression(
    std::unique_ptr<Expression> &expr) {
  if (!expr) {
    return;
  }
  ASTRewriter::rewriteExpression(expr);
  auto *subquery = dynamic_cast<ExistentialSubquery *>(expr.get());
  if (!subquery || !subquery->pattern || subquery->query) {
    return;
  }

  auto match = std::make_unique<Match>();
  match->pattern = std::move(subquery->pattern);
  match->where = std::move(subquery->where_expr);

  auto single_part = std::make_unique<SinglePartQuery>();
  single_part->reading_clauses.push_back(std::move(match));

  auto return_clause = std::make_unique<Return>();
  auto body = std::make_unique<ProjectionBody>();
  auto item = std::make_unique<ProjectionItem>();
  auto literal = std::make_unique<IntegerLiteral>();
  literal->value = 1;
  item->expression = std::move(literal);
  body->items.push_back(std::move(item));
  return_clause->body = std::move(body);
  single_part->return_clause = std::move(return_clause);

  auto query = std::make_unique<RegularQuery>();
  query->single_query = std::move(single_part);
  subquery->query = std::move(query);
}

}  // namespace ast
