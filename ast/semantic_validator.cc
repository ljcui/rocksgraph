#include "semantic_validator.h"

#include <unordered_set>

#include "ast_walker.h"

namespace ast {
namespace {

class SemanticValidator : public ASTWalker {
 public:
  explicit SemanticValidator(std::vector<std::string> &errors)
      : errors_(errors) {}

  void validate(ASTNode &node) {
    scope_stack_.clear();
    scope_stack_.push_back(Scope{});
    reported_.clear();
    walk(node);
    scope_stack_.clear();
  }

 protected:
  void visit(RegularQuery &node) override {
    const Scope base = currentScope();
    if (node.single_query) {
      pushScope(base);
      walkMaybe(node.single_query);
      popScope();
    }
    for (auto &part : node.unions) {
      if (!part || !part->query) {
        continue;
      }
      pushScope(base);
      walkMaybe(part->query);
      popScope();
    }
  }

  void visit(StandaloneCall &node) override {
    walkList(node.arguments);

    Scope yield_scope = currentScope();
    if (!node.yield_star) {
      for (const auto &item : node.yield_items) {
        yield_scope.add(item.variable);
      }
    }

    if (node.yield_where) {
      pushScope(yield_scope);
      if (node.yield_star) {
        allow_any_depth_++;
      }
      walkMaybe(node.yield_where);
      if (node.yield_star) {
        allow_any_depth_--;
      }
      popScope();
    }

    replaceCurrentScope(yield_scope);
  }

  void visit(MultiPartQuery &node) override {
    for (auto &part : node.parts) {
      walkList(part.reading_clauses);
      walkList(part.updating_clauses);
      if (part.with_clause) {
        part.with_clause->accept(*this);
      }
    }
    walkMaybe(node.final_single_part_query);
  }

  void visit(Match &node) override {
    if (node.pattern) {
      collectFromPattern(*node.pattern, currentScope());
    }
    ASTWalker::visit(node);
  }

  void visit(Unwind &node) override {
    ASTWalker::visit(node);
    define(node.variable);
  }

  void visit(InQueryCall &node) override {
    walkList(node.arguments);

    Scope yield_scope = currentScope();
    for (const auto &item : node.yield_items) {
      yield_scope.add(item.variable);
    }

    if (node.yield_where) {
      pushScope(yield_scope);
      walkMaybe(node.yield_where);
      popScope();
    }

    replaceCurrentScope(yield_scope);
  }

  void visit(Create &node) override {
    if (node.pattern) {
      collectFromPattern(*node.pattern, currentScope());
    }
    ASTWalker::visit(node);
  }

  void visit(Merge &node) override {
    if (node.pattern_part) {
      collectFromPatternPart(*node.pattern_part, currentScope());
    }
    ASTWalker::visit(node);
  }

  void visit(ProjectionBody &node) override {
    const Scope pre = currentScope();
    for (auto &item : node.items) {
      walkMaybe(item);
    }

    const Scope projected = scopeFromProjection(node, pre);
    const Scope order_scope = mergeScopes(pre, projected);

    pushScope(order_scope);
    walkList(node.order_by);
    walkMaybe(node.skip);
    walkMaybe(node.limit);
    popScope();
  }

  void visit(With &node) override {
    if (!node.body) {
      return;
    }
    const Scope pre = currentScope();
    node.body->accept(*this);

    const Scope projected = scopeFromProjection(*node.body, pre);
    replaceCurrentScope(projected);
    walkMaybe(node.where);
  }

  void visit(ListComprehension &node) override {
    walkMaybe(node.list_expr);

    pushScope(currentScope());
    define(node.variable);
    walkMaybe(node.where_expr);
    walkMaybe(node.eval_expr);
    popScope();
  }

  void visit(PatternComprehension &node) override {
    pushScope(currentScope());
    define(node.variable);
    if (node.relationships_pattern) {
      collectFromRelationshipsPattern(*node.relationships_pattern,
                                      currentScope());
    }
    walkMaybe(node.relationships_pattern);
    walkMaybe(node.where_expr);
    walkMaybe(node.eval_expr);
    popScope();
  }

  void visit(PatternPredicateExpression &node) override {
    pushScope(currentScope());
    if (node.relationships_pattern) {
      collectFromRelationshipsPattern(*node.relationships_pattern,
                                      currentScope());
    }
    walkMaybe(node.relationships_pattern);
    popScope();
  }

  void visit(AllQuantifier &node) override { validateQuantifier(node); }
  void visit(AnyQuantifier &node) override { validateQuantifier(node); }
  void visit(NoneQuantifier &node) override { validateQuantifier(node); }
  void visit(SingleQuantifier &node) override { validateQuantifier(node); }

  void visit(ExistentialSubquery &node) override {
    if (node.query) {
      pushScope(currentScope());
      walkMaybe(node.query);
      popScope();
      return;
    }
    if (node.pattern) {
      pushScope(currentScope());
      collectFromPattern(*node.pattern, currentScope());
      walkMaybe(node.pattern);
      walkMaybe(node.where_expr);
      popScope();
    }
  }

  void visit(Variable &node) override {
    if (allow_any_depth_ > 0) {
      return;
    }
    if (node.name.empty()) {
      return;
    }
    if (!isDefined(node.name)) {
      reportUndefined(node.name);
    }
  }

 private:
  struct Scope {
    std::unordered_set<std::string> names;

    void add(const std::string &name) {
      if (!name.empty()) {
        names.insert(name);
      }
    }

    bool contains(const std::string &name) const {
      return names.find(name) != names.end();
    }
  };

  Scope &currentScope() { return scope_stack_.back(); }
  const Scope &currentScope() const { return scope_stack_.back(); }

  void pushScope(const Scope &scope) { scope_stack_.push_back(scope); }
  void pushEmptyScope() { scope_stack_.push_back(Scope{}); }
  void popScope() { scope_stack_.pop_back(); }

  void define(const std::string &name) { currentScope().add(name); }

  void replaceCurrentScope(const Scope &scope) { scope_stack_.back() = scope; }

  bool isDefined(const std::string &name) const {
    for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
      if (it->contains(name)) {
        return true;
      }
    }
    return false;
  }

  Scope mergeScopes(const Scope &lhs, const Scope &rhs) const {
    Scope out = lhs;
    out.names.insert(rhs.names.begin(), rhs.names.end());
    return out;
  }

  void reportUndefined(const std::string &name) {
    if (reported_.insert(name).second) {
      errors_.push_back("undefined variable: " + name);
    }
  }

  void collectFromPattern(const Pattern &pattern, Scope &scope) const {
    for (const auto &part : pattern.parts) {
      if (part) {
        collectFromPatternPart(*part, scope);
      }
    }
  }

  void collectFromPatternPart(const PatternPart &part, Scope &scope) const {
    scope.add(part.variable);
    if (part.element) {
      collectFromPatternElement(*part.element, scope);
    }
  }

  void collectFromPatternElement(const PatternElement &element,
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

  void collectFromRelationshipsPattern(const RelationshipsPattern &pattern,
                                       Scope &scope) const {
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

  void collectFromNodePattern(const NodePattern &node, Scope &scope) const {
    scope.add(node.variable);
  }

  void collectFromRelationshipDetail(const RelationshipDetail &detail,
                                     Scope &scope) const {
    scope.add(detail.variable);
  }

  void collectFromProjectionItem(const ProjectionItem &item,
                                 Scope &scope) const {
    if (!item.alias.empty()) {
      scope.add(item.alias);
      return;
    }
    if (const auto *var =
            dynamic_cast<const Variable *>(item.expression.get())) {
      scope.add(var->name);
    }
  }

  Scope scopeFromProjection(const ProjectionBody &body,
                            const Scope &fallback) const {
    Scope scope = body.star ? fallback : Scope{};
    for (const auto &item : body.items) {
      if (item) {
        collectFromProjectionItem(*item, scope);
      }
    }
    return scope;
  }

  void validateQuantifier(Quantifier &node) {
    walkMaybe(node.list_expr);
    pushScope(currentScope());
    define(node.variable);
    walkMaybe(node.predicate);
    popScope();
  }

  std::vector<Scope> scope_stack_;
  std::unordered_set<std::string> reported_;
  std::vector<std::string> &errors_;
  int allow_any_depth_ = 0;
};

}  // namespace

void validateStatement(ASTNode &node, std::vector<std::string> &errors) {
  SemanticValidator validator(errors);
  validator.validate(node);
}

}  // namespace ast
