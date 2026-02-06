#include "return_star_rewriter.h"

namespace ast {

void ReturnStarRewriter::rewrite(ASTNode &node) {
  scope_stack_.clear();
  ASTRewriter::rewrite(node);
}

void ReturnStarRewriter::Scope::add(const std::string &name) {
  if (name.empty()) {
    return;
  }
  if (names.insert(name).second) {
    order.push_back(name);
  }
}

const ReturnStarRewriter::Scope &ReturnStarRewriter::currentScope() const {
  static const Scope kEmpty;
  if (scope_stack_.empty()) {
    return kEmpty;
  }
  return scope_stack_.back();
}

void ReturnStarRewriter::visit(SinglePartQuery &node) {
  Scope scope = currentScope();
  for (const auto &clause : node.reading_clauses) {
    collectFromReadingClause(*clause, scope);
  }
  for (const auto &clause : node.updating_clauses) {
    collectFromUpdatingClause(*clause, scope);
  }

  scope_stack_.push_back(scope);
  rewriteList(node.reading_clauses);
  rewriteList(node.updating_clauses);
  rewriteMaybe(node.return_clause);
  scope_stack_.pop_back();
}

void ReturnStarRewriter::visit(MultiPartQuery &node) {
  Scope scope = currentScope();
  for (auto &part : node.parts) {
    Scope part_scope = scope;
    for (const auto &clause : part.reading_clauses) {
      collectFromReadingClause(*clause, part_scope);
    }
    for (const auto &clause : part.updating_clauses) {
      collectFromUpdatingClause(*clause, part_scope);
    }

    scope_stack_.push_back(part_scope);
    rewriteList(part.reading_clauses);
    rewriteList(part.updating_clauses);
    rewriteMaybe(part.with_clause);
    scope_stack_.pop_back();

    if (part.with_clause && part.with_clause->body) {
      scope = scopeFromProjection(*part.with_clause->body, part_scope);
    } else {
      scope = part_scope;
    }
  }

  scope_stack_.push_back(scope);
  rewriteMaybe(node.final_single_part_query);
  scope_stack_.pop_back();
}

void ReturnStarRewriter::visit(ProjectionBody &node) {
  expandStar(node);
  ASTRewriter::visit(node);
}

ReturnStarRewriter::Scope ReturnStarRewriter::scopeFromProjection(
    const ProjectionBody &body, const Scope &fallback) const {
  Scope scope = body.star ? fallback : Scope{};
  for (const auto &item : body.items) {
    if (item) {
      collectFromProjectionItem(*item, scope);
    }
  }
  return scope;
}

void ReturnStarRewriter::expandStar(ProjectionBody &body) {
  if (!body.star) {
    return;
  }
  const Scope &scope = currentScope();
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
    expanded.push_back(std::move(item));
  }
  for (auto &item : body.items) {
    expanded.push_back(std::move(item));
  }
  body.items.swap(expanded);
  body.star = false;
}

void ReturnStarRewriter::collectFromReadingClause(const ReadingClause &clause,
                                                  Scope &scope) const {
  if (const auto *match = dynamic_cast<const Match *>(&clause)) {
    if (match->pattern) {
      collectFromPattern(*match->pattern, scope);
    }
    return;
  }
  if (const auto *unwind = dynamic_cast<const Unwind *>(&clause)) {
    scope.add(unwind->variable);
    return;
  }
  if (const auto *call = dynamic_cast<const InQueryCall *>(&clause)) {
    for (const auto &item : call->yield_items) {
      scope.add(item.variable);
    }
    return;
  }
}

void ReturnStarRewriter::collectFromUpdatingClause(const UpdatingClause &clause,
                                                   Scope &scope) const {
  if (const auto *create = dynamic_cast<const Create *>(&clause)) {
    if (create->pattern) {
      collectFromPattern(*create->pattern, scope);
    }
    return;
  }
  if (const auto *merge = dynamic_cast<const Merge *>(&clause)) {
    if (merge->pattern_part) {
      collectFromPatternPart(*merge->pattern_part, scope);
    }
    return;
  }
}

void ReturnStarRewriter::collectFromPattern(const Pattern &pattern,
                                            Scope &scope) const {
  for (const auto &part : pattern.parts) {
    if (part) {
      collectFromPatternPart(*part, scope);
    }
  }
}

void ReturnStarRewriter::collectFromPatternPart(const PatternPart &part,
                                                Scope &scope) const {
  scope.add(part.variable);
  if (part.element) {
    collectFromPatternElement(*part.element, scope);
  }
}

void ReturnStarRewriter::collectFromPatternElement(
    const PatternElement &element, Scope &scope) const {
  if (element.node_pattern) {
    collectFromNodePattern(*element.node_pattern, scope);
  }
  for (const auto &link : element.chain) {
    if (link.first && link.first->detail) {
      collectFromRelationshipDetail(*link.first->detail, scope);
    }
    if (link.second) {
      collectFromNodePattern(*link.second, scope);
    }
  }
}

void ReturnStarRewriter::collectFromRelationshipsPattern(
    const RelationshipsPattern &pattern, Scope &scope) const {
  if (pattern.node_pattern) {
    collectFromNodePattern(*pattern.node_pattern, scope);
  }
  for (const auto &link : pattern.chain) {
    if (link.first && link.first->detail) {
      collectFromRelationshipDetail(*link.first->detail, scope);
    }
    if (link.second) {
      collectFromNodePattern(*link.second, scope);
    }
  }
}

void ReturnStarRewriter::collectFromNodePattern(const NodePattern &node,
                                                Scope &scope) {
  scope.add(node.variable);
}

void ReturnStarRewriter::collectFromRelationshipDetail(
    const RelationshipDetail &detail, Scope &scope) {
  scope.add(detail.variable);
}

void ReturnStarRewriter::collectFromProjectionItem(const ProjectionItem &item,
                                                   Scope &scope) {
  if (!item.alias.empty()) {
    scope.add(item.alias);
    return;
  }
  if (const auto *var = dynamic_cast<const Variable *>(item.expression.get())) {
    scope.add(var->name);
  }
}

}  // namespace ast
