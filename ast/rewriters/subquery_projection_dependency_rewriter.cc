#include "subquery_projection_dependency_rewriter.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace ast {
namespace {

void AddDependenciesToProjection(ProjectionBody *body,
                                 const std::vector<std::string> &dependencies) {
  if (body == nullptr) {
    return;
  }
  for (const std::string &dependency : dependencies) {
    const bool already_projected =
        std::ranges::any_of(body->items, [&](const auto &item) {
          return item != nullptr && item->alias == dependency;
        });
    if (already_projected) {
      continue;
    }
    auto variable = std::make_unique<Variable>();
    variable->name = dependency;
    auto item = std::make_unique<ProjectionItem>();
    item->expression = std::move(variable);
    item->alias = dependency;
    body->items.push_back(std::move(item));
  }
}

struct SplitReturn {
  std::unique_ptr<With> with_clause;
  std::unique_ptr<Return> return_clause;
};

std::optional<SplitReturn> SplitOrderedReturn(SinglePartQuery &query) {
  if (query.return_clause == nullptr || query.return_clause->body == nullptr ||
      query.return_clause->body->order_by.empty()) {
    return std::nullopt;
  }

  auto with_clause = std::make_unique<With>();
  with_clause->body = std::move(query.return_clause->body);

  auto return_clause = std::make_unique<Return>();
  return_clause->body = std::make_unique<ProjectionBody>();
  for (const auto &item : with_clause->body->items) {
    if (item == nullptr || item->alias.empty()) {
      continue;
    }
    auto variable = std::make_unique<Variable>();
    variable->name = item->alias;
    auto projection = std::make_unique<ProjectionItem>();
    projection->expression = std::move(variable);
    projection->alias = item->alias;
    return_clause->body->items.push_back(std::move(projection));
  }
  query.return_clause.reset();
  return SplitReturn{.with_clause = std::move(with_clause),
                     .return_clause = std::move(return_clause)};
}

void SplitOrderedTerminalReturn(std::unique_ptr<SingleQuery> &query) {
  if (query == nullptr) {
    return;
  }
  if (query->Is(ASTNodeType::kSinglePartQuery)) {
    auto single = std::unique_ptr<SinglePartQuery>(
        CastAst<SinglePartQuery>(query.release()));
    std::optional<SplitReturn> split = SplitOrderedReturn(*single);
    if (!split.has_value()) {
      query = std::move(single);
      return;
    }

    auto multi = std::make_unique<MultiPartQuery>();
    MultiPartQuery::WithPart part;
    part.reading_clauses = std::move(single->reading_clauses);
    part.updating_clauses = std::move(single->updating_clauses);
    part.with_clause = std::move(split->with_clause);
    multi->parts.push_back(std::move(part));
    multi->final_single_part_query = std::make_unique<SinglePartQuery>();
    multi->final_single_part_query->return_clause =
        std::move(split->return_clause);
    query = std::move(multi);
    return;
  }
  if (!query->Is(ASTNodeType::kMultiPartQuery)) {
    return;
  }

  auto &multi = CastAst<MultiPartQuery>(*query);
  if (multi.final_single_part_query == nullptr) {
    return;
  }
  std::optional<SplitReturn> split =
      SplitOrderedReturn(*multi.final_single_part_query);
  if (!split.has_value()) {
    return;
  }

  MultiPartQuery::WithPart part;
  part.reading_clauses =
      std::move(multi.final_single_part_query->reading_clauses);
  part.updating_clauses =
      std::move(multi.final_single_part_query->updating_clauses);
  part.with_clause = std::move(split->with_clause);
  multi.parts.push_back(std::move(part));
  multi.final_single_part_query = std::make_unique<SinglePartQuery>();
  multi.final_single_part_query->return_clause =
      std::move(split->return_clause);
}

void AddDependenciesToSingleQuery(
    std::unique_ptr<SingleQuery> &query,
    const std::vector<std::string> &dependencies) {
  SplitOrderedTerminalReturn(query);
  if (query == nullptr || query->Is(ASTNodeType::kSinglePartQuery)) {
    return;
  }
  if (!query->Is(ASTNodeType::kMultiPartQuery)) {
    return;
  }
  auto *multi = CastAst<MultiPartQuery>(query.get());
  for (auto &part : multi->parts) {
    if (part.with_clause != nullptr) {
      AddDependenciesToProjection(part.with_clause->body.get(), dependencies);
    }
  }
}

}  // namespace

void SubqueryProjectionDependencyRewriter::Visit(RegularQuery &node) {
  Prepare(node);
  ASTRewriter::Visit(node);
}

void SubqueryProjectionDependencyRewriter::Visit(StandaloneCall &node) {
  Prepare(node);
  ASTRewriter::Visit(node);
}

void SubqueryProjectionDependencyRewriter::Visit(ExistentialSubquery &node) {
  ASTRewriter::Visit(node);
  if (!semantic_table_.has_value() || node.query == nullptr) {
    return;
  }
  AddDependenciesToQuery(*node.query,
                         semantic_table_->ExpressionDependencies(node));
}

void SubqueryProjectionDependencyRewriter::Prepare(const ASTNode &node) {
  if (!semantic_table_.has_value()) {
    semantic_table_.emplace(AnalyzeSemanticTable(node));
  }
}

void SubqueryProjectionDependencyRewriter::AddDependenciesToQuery(
    RegularQuery &query, const std::unordered_set<std::string> &dependencies) {
  std::vector<std::string> ordered(dependencies.begin(), dependencies.end());
  std::sort(ordered.begin(), ordered.end());
  AddDependenciesToSingleQuery(query.single_query, ordered);
  for (auto &part : query.unions) {
    if (part != nullptr) {
      AddDependenciesToSingleQuery(part->query, ordered);
    }
  }
}

}  // namespace ast
