#include "semantic_validator.h"

#include <ranges>
#include <unordered_set>
#include <utility>

#include "ast_exception.h"
#include "ast_walker.h"

namespace ast {
namespace {

class SemanticValidator : public ASTWalker {
 public:
  explicit SemanticValidator(std::vector<std::string> &errors)
      : errors_(errors) {}

  void Validate(ASTNode &node) {
    scope_stack_.clear();
    scope_stack_.emplace_back();
    reported_.clear();
    Walk(node);
    scope_stack_.clear();
  }

 protected:
  void Visit(RegularQuery &node) override {
    const Scope base = CurrentScope();
    if (node.single_query) {
      PushScope(base);
      WalkMaybe(node.single_query);
      PopScope();
    }
    for (auto &part : node.unions) {
      if (!part || !part->query) {
        continue;
      }
      PushScope(base);
      WalkMaybe(part->query);
      PopScope();
    }
  }

  void Visit(StandaloneCall &node) override {
    WalkList(node.arguments);

    Scope yield_scope = CurrentScope();
    if (!node.yield_star) {
      for (const auto &item : node.yield_items) {
        yield_scope.Add(item.variable);
      }
    }

    if (node.yield_where) {
      PushScope(yield_scope);
      if (node.yield_star) {
        allow_any_depth_++;
      }
      WalkMaybe(node.yield_where);
      if (node.yield_star) {
        allow_any_depth_--;
      }
      PopScope();
    }

    ReplaceCurrentScope(yield_scope);
  }

  void Visit(Match &node) override {
    if (node.pattern) {
      CollectFromPattern(*node.pattern, CurrentScope());
    }
    ASTWalker::Visit(node);
  }

  void Visit(Unwind &node) override {
    ASTWalker::Visit(node);
    Define(node.variable);
  }

  void Visit(InQueryCall &node) override {
    WalkList(node.arguments);

    Scope yield_scope = CurrentScope();
    for (const auto &item : node.yield_items) {
      yield_scope.Add(item.variable);
    }

    if (node.yield_where) {
      PushScope(yield_scope);
      WalkMaybe(node.yield_where);
      PopScope();
    }

    ReplaceCurrentScope(yield_scope);
  }

  void Visit(Create &node) override {
    if (node.pattern) {
      CollectFromPattern(*node.pattern, CurrentScope());
    }
    ASTWalker::Visit(node);
  }

  void Visit(Merge &node) override {
    if (node.pattern_part) {
      CollectFromPatternPart(*node.pattern_part, CurrentScope());
    }
    ASTWalker::Visit(node);
  }

  void Visit(ProjectionBody &node) override {
    const Scope pre = CurrentScope();
    for (auto &item : node.items) {
      WalkMaybe(item);
    }

    const Scope projected = ScopeFromProjection(node, pre);
    const Scope order_scope = MergeScopes(pre, projected);

    PushScope(order_scope);
    WalkList(node.order_by);
    WalkMaybe(node.skip);
    WalkMaybe(node.limit);
    PopScope();
  }

  void Visit(With &node) override {
    if (!node.body) {
      return;
    }
    const Scope pre = CurrentScope();
    node.body->Accept(*this);

    const Scope projected = ScopeFromProjection(*node.body, pre);
    ReplaceCurrentScope(projected);
    WalkMaybe(node.where);
  }

  void Visit(ListComprehension &node) override {
    WalkMaybe(node.list_expr);

    PushScope(CurrentScope());
    Define(node.variable);
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(PatternComprehension &node) override {
    PushScope(CurrentScope());
    Define(node.variable);
    if (node.relationships_pattern) {
      CollectFromRelationshipsPattern(*node.relationships_pattern,
                                      CurrentScope());
    }
    ASTWalker::Visit(node);
    PopScope();
  }

  void Visit(PatternPredicateExpression &node) override {
    PushScope(CurrentScope());
    if (node.relationships_pattern) {
      CollectFromRelationshipsPattern(*node.relationships_pattern,
                                      CurrentScope());
    }
    ASTWalker::Visit(node);
    PopScope();
  }

  void Visit(AllQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(AnyQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(NoneQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(SingleQuantifier &node) override { ValidateQuantifier(node); }

  void Visit(ExistentialSubquery &node) override {
    if (node.query) {
      PushScope(CurrentScope());
      WalkMaybe(node.query);
      PopScope();
      return;
    }
    if (node.pattern) {
      PushScope(CurrentScope());
      CollectFromPattern(*node.pattern, CurrentScope());
      WalkMaybe(node.pattern);
      WalkMaybe(node.where_expr);
      PopScope();
    }
  }

  void Visit(Variable &node) override {
    if (allow_any_depth_ > 0) {
      return;
    }
    if (node.name.empty()) {
      return;
    }
    if (!IsDefined(node.name)) {
      ReportUndefined(node.name);
    }
  }

 private:
  struct Scope {
    std::unordered_set<std::string> names;

    void Add(const std::string &name) {
      if (!name.empty()) {
        names.insert(name);
      }
    }

    [[nodiscard]] bool Contains(const std::string &name) const {
      return names.find(name) != names.end();
    }
  };

  Scope &CurrentScope() { return scope_stack_.back(); }
  [[nodiscard]] const Scope &CurrentScope() const {
    return scope_stack_.back();
  }

  void PushScope(const Scope &scope) { scope_stack_.push_back(scope); }
  void PushEmptyScope() { scope_stack_.emplace_back(); }
  void PopScope() { scope_stack_.pop_back(); }

  void Define(const std::string &name) { CurrentScope().Add(name); }

  void ReplaceCurrentScope(const Scope &scope) { scope_stack_.back() = scope; }

  [[nodiscard]] bool IsDefined(const std::string &name) const {
    for (const auto &it : std::ranges::reverse_view(scope_stack_)) {
      if (it.Contains(name)) {
        return true;
      }
    }
    return false;
  }

  static Scope MergeScopes(const Scope &lhs, const Scope &rhs) {
    Scope out = lhs;
    out.names.insert(rhs.names.begin(), rhs.names.end());
    return out;
  }

  void ReportUndefined(const std::string &name) {
    if (reported_.insert(name).second) {
      errors_.push_back("undefined variable: " + name);
    }
  }

  void CollectFromPattern(const Pattern &pattern, Scope &scope) const {
    for (const auto &part : pattern.parts) {
      if (part) {
        CollectFromPatternPart(*part, scope);
      }
    }
  }

  void CollectFromPatternPart(const PatternPart &part, Scope &scope) const {
    scope.Add(part.variable);
    if (part.element) {
      CollectFromPatternElement(*part.element, scope);
    }
  }

  void CollectFromPatternElement(const PatternElement &element,
                                 Scope &scope) const {
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

  void CollectFromRelationshipsPattern(const RelationshipsPattern &pattern,
                                       Scope &scope) const {
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

  static void CollectFromNodePattern(const NodePattern &node, Scope &scope) {
    scope.Add(node.variable);
  }

  static void CollectFromRelationshipDetail(const RelationshipDetail &detail,
                                            Scope &scope) {
    scope.Add(detail.variable);
  }

  static void CollectFromProjectionItem(const ProjectionItem &item,
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

  [[nodiscard]] Scope ScopeFromProjection(const ProjectionBody &body,
                                          const Scope &fallback) const {
    Scope scope = body.star ? fallback : Scope{};
    for (const auto &item : body.items) {
      if (item) {
        CollectFromProjectionItem(*item, scope);
      }
    }
    return scope;
  }

  void ValidateQuantifier(Quantifier &node) {
    WalkMaybe(node.list_expr);
    PushScope(CurrentScope());
    Define(node.variable);
    WalkMaybe(node.predicate);
    PopScope();
  }

  std::vector<Scope> scope_stack_;
  std::unordered_set<std::string> reported_;
  std::vector<std::string> &errors_;
  int allow_any_depth_ = 0;
};

}  // namespace

void ValidateStatement(ASTNode &node) {
  std::vector<std::string> errors;
  SemanticValidator validator(errors);
  validator.Validate(node);
  if (!errors.empty()) {
    THROW(SemanticError, std::move(errors));
  }
}

}  // namespace ast
