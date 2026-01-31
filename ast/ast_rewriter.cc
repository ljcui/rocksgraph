#include "ast_rewriter.h"

#include <utility>

namespace ast {

void ASTRewriter::rewrite(ASTNode &node) { node.accept(*this); }

void ASTRewriter::visit(Statement &node) { (void)node; }
void ASTRewriter::visit(Query &node) { (void)node; }
void ASTRewriter::visit(RegularQuery &node) {
  rewriteMaybe(node.single_query);
  rewriteList(node.unions);
}
void ASTRewriter::visit(StandaloneCall &node) {
  rewriteList(node.arguments);
  rewriteMaybe(node.yield_where);
}
void ASTRewriter::visit(SingleQuery &node) { (void)node; }
void ASTRewriter::visit(SinglePartQuery &node) {
  rewriteList(node.reading_clauses);
  rewriteList(node.updating_clauses);
  rewriteMaybe(node.return_clause);
}
void ASTRewriter::visit(MultiPartQuery &node) {
  for (auto &part : node.parts) {
    rewriteList(part.reading_clauses);
    rewriteList(part.updating_clauses);
    rewriteMaybe(part.with_clause);
  }
  rewriteMaybe(node.final_single_part_query);
}
void ASTRewriter::visit(UnionPart &node) { rewriteMaybe(node.query); }

void ASTRewriter::visit(Expression &node) { (void)node; }
void ASTRewriter::visit(BinaryExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(OrExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(XorExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(AndExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ComparisonExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ComparisonChainExpression &node) {
  rewriteMaybe(node.left);
  for (auto &entry : node.rights) {
    rewriteMaybe(entry.second);
  }
}
void ASTRewriter::visit(AddExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(SubtractExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(MultiplyExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(DivideExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ModuloExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(PowerExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(UnaryExpression &node) { rewriteMaybe(node.operand); }
void ASTRewriter::visit(NotExpression &node) { rewriteMaybe(node.operand); }
void ASTRewriter::visit(UnaryPlusExpression &node) {
  rewriteMaybe(node.operand);
}
void ASTRewriter::visit(UnaryMinusExpression &node) {
  rewriteMaybe(node.operand);
}
void ASTRewriter::visit(StringPredicateExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ListPredicateExpression &node) {
  rewriteMaybe(node.element);
  rewriteMaybe(node.list);
}
void ASTRewriter::visit(LabelPredicateExpression &node) {
  rewriteMaybe(node.expr);
}
void ASTRewriter::visit(NullPredicateExpression &node) {
  rewriteMaybe(node.operand);
}

void ASTRewriter::visit(Literal &node) { (void)node; }
void ASTRewriter::visit(BooleanLiteral &node) { (void)node; }
void ASTRewriter::visit(IntegerLiteral &node) { (void)node; }
void ASTRewriter::visit(DoubleLiteral &node) { (void)node; }
void ASTRewriter::visit(StringLiteral &node) { (void)node; }
void ASTRewriter::visit(NullLiteral &node) { (void)node; }
void ASTRewriter::visit(ListLiteral &node) { rewriteList(node.elements); }
void ASTRewriter::visit(MapLiteral &node) {
  for (auto &entry : node.entries) {
    rewriteMaybe(entry.second);
  }
}
void ASTRewriter::visit(Properties &node) {
  rewriteMaybe(node.map);
  rewriteMaybe(node.parameter);
}

void ASTRewriter::visit(Variable &node) { (void)node; }
void ASTRewriter::visit(Parameter &node) { (void)node; }
void ASTRewriter::visit(PropertyExpression &node) {
  rewriteMaybe(node.object);
}
void ASTRewriter::visit(ListIndexExpression &node) {
  rewriteMaybe(node.list);
  rewriteMaybe(node.index);
}
void ASTRewriter::visit(ListSliceExpression &node) {
  rewriteMaybe(node.list);
  rewriteMaybe(node.start_index);
  rewriteMaybe(node.end_index);
}
void ASTRewriter::visit(FunctionInvocation &node) {
  rewriteList(node.arguments);
}
void ASTRewriter::visit(CountStarExpression &node) { (void)node; }
void ASTRewriter::visit(CaseExpression &node) {
  rewriteMaybe(node.test);
  for (auto &alt : node.alternatives) {
    rewriteMaybe(alt.first);
    rewriteMaybe(alt.second);
  }
  rewriteMaybe(node.else_expr);
}
void ASTRewriter::visit(ParenthesizedExpression &node) {
  rewriteMaybe(node.expr);
}
void ASTRewriter::visit(ListComprehension &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.where_expr);
  rewriteMaybe(node.eval_expr);
}
void ASTRewriter::visit(PatternComprehension &node) {
  rewriteMaybe(node.relationships_pattern);
  rewriteMaybe(node.where_expr);
  rewriteMaybe(node.eval_expr);
}
void ASTRewriter::visit(PatternPredicateExpression &node) {
  rewriteMaybe(node.relationships_pattern);
}
void ASTRewriter::visit(Quantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(AllQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(AnyQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(NoneQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(SingleQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(ExistentialSubquery &node) {
  rewriteMaybe(node.query);
  rewriteMaybe(node.pattern);
  rewriteMaybe(node.where_expr);
}

void ASTRewriter::visit(Pattern &node) { rewriteList(node.parts); }
void ASTRewriter::visit(PatternPart &node) { rewriteMaybe(node.element); }
void ASTRewriter::visit(PatternElement &node) {
  rewriteMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    rewriteMaybe(link.first);
    rewriteMaybe(link.second);
  }
}
void ASTRewriter::visit(RelationshipsPattern &node) {
  rewriteMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    rewriteMaybe(link.first);
    rewriteMaybe(link.second);
  }
}
void ASTRewriter::visit(NodePattern &node) { rewriteMaybe(node.properties); }
void ASTRewriter::visit(RelationshipPattern &node) {
  rewriteMaybe(node.detail);
}
void ASTRewriter::visit(RelationshipDetail &node) {
  rewriteMaybe(node.properties);
}

void ASTRewriter::visit(Clause &node) { (void)node; }
void ASTRewriter::visit(ReadingClause &node) { (void)node; }
void ASTRewriter::visit(Match &node) {
  rewriteMaybe(node.pattern);
  rewriteMaybe(node.where);
}
void ASTRewriter::visit(Unwind &node) { rewriteMaybe(node.expression); }
void ASTRewriter::visit(InQueryCall &node) {
  rewriteList(node.arguments);
  rewriteMaybe(node.yield_where);
}
void ASTRewriter::visit(UpdatingClause &node) { (void)node; }
void ASTRewriter::visit(Create &node) { rewriteMaybe(node.pattern); }
void ASTRewriter::visit(Merge &node) {
  rewriteMaybe(node.pattern_part);
  for (auto &action : node.actions) {
    rewriteMaybe(action.second);
  }
}
void ASTRewriter::visit(Delete &node) { rewriteList(node.expressions); }
void ASTRewriter::visit(Set &node) { rewriteList(node.items); }
void ASTRewriter::visit(SetItem &node) {
  rewriteMaybe(node.target);
  rewriteMaybe(node.value);
}
void ASTRewriter::visit(Remove &node) { rewriteList(node.items); }
void ASTRewriter::visit(RemoveItem &node) { rewriteMaybe(node.target); }
void ASTRewriter::visit(ProjectionClause &node) { rewriteMaybe(node.body); }
void ASTRewriter::visit(ProjectionBody &node) {
  rewriteList(node.items);
  rewriteList(node.order_by);
  rewriteMaybe(node.skip);
  rewriteMaybe(node.limit);
}
void ASTRewriter::visit(ProjectionItem &node) { rewriteMaybe(node.expression); }
void ASTRewriter::visit(SortItem &node) { rewriteMaybe(node.expression); }
void ASTRewriter::visit(With &node) {
  rewriteMaybe(node.body);
  rewriteMaybe(node.where);
}
void ASTRewriter::visit(Return &node) { rewriteMaybe(node.body); }

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
  static const Scope empty;
  if (scope_stack_.empty()) {
    return empty;
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

void ReturnStarRewriter::collectFromPatternElement(const PatternElement &element,
                                                   Scope &scope) const {
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
                                                Scope &scope) const {
  scope.add(node.variable);
}

void ReturnStarRewriter::collectFromRelationshipDetail(
    const RelationshipDetail &detail, Scope &scope) const {
  scope.add(detail.variable);
}

void ReturnStarRewriter::collectFromProjectionItem(const ProjectionItem &item,
                                                   Scope &scope) const {
  if (!item.alias.empty()) {
    scope.add(item.alias);
    return;
  }
  if (const auto *var = dynamic_cast<const Variable *>(item.expression.get())) {
    scope.add(var->name);
  }
}

}  // namespace ast
