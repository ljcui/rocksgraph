#include "return_star_rewriter.h"

namespace ast {

void ReturnStarRewriter::Rewrite(ASTNode &node) {
  scope_stack_.clear();
  ASTRewriter::Rewrite(node);
}

void ReturnStarRewriter::Scope::Add(const std::string &name) {
  if (name.empty()) {
    return;
  }
  if (names.insert(name).second) {
    order.push_back(name);
  }
}

const ReturnStarRewriter::Scope &ReturnStarRewriter::CurrentScope() const {
  static const Scope kEmpty;
  if (scope_stack_.empty()) {
    return kEmpty;
  }
  return scope_stack_.back();
}

void ReturnStarRewriter::Visit(SinglePartQuery &node) {
  Scope scope = CurrentScope();
  for (const auto &clause : node.reading_clauses) {
    CollectFromReadingClause(*clause, scope);
  }
  for (const auto &clause : node.updating_clauses) {
    CollectFromUpdatingClause(*clause, scope);
  }

  scope_stack_.push_back(scope);
  RewriteList(node.reading_clauses);
  RewriteList(node.updating_clauses);
  RewriteMaybe(node.return_clause);
  scope_stack_.pop_back();
}

void ReturnStarRewriter::Visit(MultiPartQuery &node) {
  Scope scope = CurrentScope();
  for (auto &part : node.parts) {
    Scope part_scope = scope;
    for (const auto &clause : part.reading_clauses) {
      CollectFromReadingClause(*clause, part_scope);
    }
    for (const auto &clause : part.updating_clauses) {
      CollectFromUpdatingClause(*clause, part_scope);
    }

    scope_stack_.push_back(part_scope);
    RewriteList(part.reading_clauses);
    RewriteList(part.updating_clauses);
    RewriteMaybe(part.with_clause);
    scope_stack_.pop_back();

    if (part.with_clause && part.with_clause->body) {
      scope = ScopeFromProjection(*part.with_clause->body, part_scope);
    } else {
      scope = part_scope;
    }
  }

  scope_stack_.push_back(scope);
  RewriteMaybe(node.final_single_part_query);
  scope_stack_.pop_back();
}

void ReturnStarRewriter::Visit(With &node) {
  if (node.body && node.body->star && CurrentScope().order.empty()) {
    node.body->star = false;
    node.body->empty_star_expansion = true;
  }
  ASTRewriter::Visit(node);
}

void ReturnStarRewriter::Visit(ProjectionBody &node) {
  ExpandStar(node);
  ASTRewriter::Visit(node);
}

ReturnStarRewriter::Scope ReturnStarRewriter::ScopeFromProjection(
    const ProjectionBody &body, const Scope &fallback) const {
  Scope scope = body.star ? fallback : Scope{};
  for (const auto &item : body.items) {
    if (item) {
      CollectFromProjectionItem(*item, scope);
    }
  }
  return scope;
}

void ReturnStarRewriter::ExpandStar(ProjectionBody &body) {
  if (!body.star) {
    return;
  }
  const Scope &scope = CurrentScope();
  if (scope.order.empty()) {
    return;
  }

  std::vector<std::unique_ptr<ProjectionItem>> expanded;
  expanded.reserve(scope.order.size() + body.items.size());
  for (const auto &name : scope.order) {
    auto item = std::make_unique<ProjectionItem>();
    auto var = std::make_unique<Variable>();
    var->name = name;
    item->expression = std::move(var);
    item->alias = name;
    expanded.push_back(std::move(item));
  }
  for (auto &item : body.items) {
    expanded.push_back(std::move(item));
  }
  body.items.swap(expanded);
  body.star = false;
}

void ReturnStarRewriter::CollectFromReadingClause(const ReadingClause &clause,
                                                  Scope &scope) const {
  switch (clause.node_type) {
    case ASTNodeType::kMatch: {
      const auto &match = CastAst<Match>(clause);
      if (match.pattern) {
        CollectFromPattern(*match.pattern, scope);
      }
      return;
    }
    case ASTNodeType::kUnwind: {
      const auto &unwind = CastAst<Unwind>(clause);
      scope.Add(unwind.variable);
      return;
    }
    case ASTNodeType::kInQueryCall: {
      const auto &call = CastAst<InQueryCall>(clause);
      for (const auto &item : call.yield_items) {
        scope.Add(item.variable);
      }
      return;
    }
    default:
      return;
  }
}

void ReturnStarRewriter::CollectFromUpdatingClause(const UpdatingClause &clause,
                                                   Scope &scope) const {
  switch (clause.node_type) {
    case ASTNodeType::kCreate: {
      const auto &create = CastAst<Create>(clause);
      if (create.pattern) {
        CollectFromPattern(*create.pattern, scope);
      }
      return;
    }
    case ASTNodeType::kMerge: {
      const auto &merge = CastAst<Merge>(clause);
      if (merge.pattern_part) {
        CollectFromPatternPart(*merge.pattern_part, scope);
      }
      return;
    }
    default:
      return;
  }
}

void ReturnStarRewriter::CollectFromPattern(const Pattern &pattern,
                                            Scope &scope) const {
  for (const auto &part : pattern.parts) {
    if (part) {
      CollectFromPatternPart(*part, scope);
    }
  }
}

void ReturnStarRewriter::CollectFromPatternPart(const PatternPart &part,
                                                Scope &scope) const {
  scope.Add(part.variable);
  if (part.element) {
    CollectFromPatternElement(*part.element, scope);
  }
}

void ReturnStarRewriter::CollectFromPatternElement(
    const PatternElement &element, Scope &scope) const {
  if (element.node_pattern) {
    CollectFromNodePattern(*element.node_pattern, scope);
  }
  for (const auto &link : element.chain) {
    if (link.first && link.first->detail) {
      CollectFromRelationshipDetail(*link.first->detail, scope);
    }
    if (link.second) {
      CollectFromNodePattern(*link.second, scope);
    }
  }
}

void ReturnStarRewriter::CollectFromRelationshipsPattern(
    const RelationshipsPattern &pattern, Scope &scope) const {
  if (pattern.node_pattern) {
    CollectFromNodePattern(*pattern.node_pattern, scope);
  }
  for (const auto &link : pattern.chain) {
    if (link.first && link.first->detail) {
      CollectFromRelationshipDetail(*link.first->detail, scope);
    }
    if (link.second) {
      CollectFromNodePattern(*link.second, scope);
    }
  }
}

void ReturnStarRewriter::CollectFromNodePattern(const NodePattern &node,
                                                Scope &scope) {
  scope.Add(node.variable);
}

void ReturnStarRewriter::CollectFromRelationshipDetail(
    const RelationshipDetail &detail, Scope &scope) {
  scope.Add(detail.variable);
}

void ReturnStarRewriter::CollectFromProjectionItem(const ProjectionItem &item,
                                                   Scope &scope) {
  if (!item.alias.empty()) {
    scope.Add(item.alias);
    return;
  }
  if (item.expression && item.expression->Is(ASTNodeType::kVariable)) {
    const auto *var = CastAst<Variable>(item.expression.get());
    scope.Add(var->name);
  }
}

}  // namespace ast
