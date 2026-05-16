#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast_const_walker.h"
#include "common/exception.h"

namespace ast {

namespace detail {

class ExpressionDependencyCollector : public ASTConstWalker {
 public:
  explicit ExpressionDependencyCollector(
      std::unordered_set<std::string> outer_scope = {})
      : outer_scope_(std::move(outer_scope)) {}

  std::unordered_set<std::string> Collect(const Expression &expression) {
    dependencies_.clear();
    local_scopes_.clear();
    expression.Accept(*this);
    return dependencies_;
  }

 protected:
  void Visit(const Variable &node) override {
    CHECK(!node.name.empty(), common::InvalidArgumentError,
          "variable dependency name is empty");
    if (!IsLocal(node.name)) {
      dependencies_.insert(node.name);
    }
  }

  void Visit(const ListComprehension &node) override {
    WalkMaybe(node.list_expr);
    PushScope({node.variable});
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(const PatternComprehension &node) override {
    PushScope();
    AddPatternBinding(node.variable);
    if (node.relationships_pattern) {
      AddPatternBindings(*node.relationships_pattern);
      WalkMaybe(node.relationships_pattern);
    }
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(const PatternPredicateExpression &node) override {
    PushScope();
    if (node.relationships_pattern) {
      AddPatternBindings(*node.relationships_pattern);
      WalkMaybe(node.relationships_pattern);
    }
    PopScope();
  }

  void Visit(const AllQuantifier &node) override { VisitQuantifier(node); }
  void Visit(const AnyQuantifier &node) override { VisitQuantifier(node); }
  void Visit(const NoneQuantifier &node) override { VisitQuantifier(node); }
  void Visit(const SingleQuantifier &node) override { VisitQuantifier(node); }

  void Visit(const ExistentialSubquery &node) override {
    PushScope();
    if (node.query) {
      WalkMaybe(node.query);
    } else {
      if (node.pattern) {
        AddPatternBindings(*node.pattern);
        WalkMaybe(node.pattern);
      }
      WalkMaybe(node.where_expr);
    }
    PopScope();
  }

  void Visit(const Match &node) override {
    if (node.pattern) {
      AddPatternBindings(*node.pattern);
      WalkMaybe(node.pattern);
    }
    WalkMaybe(node.where);
  }

  void Visit(const Unwind &node) override {
    WalkMaybe(node.expression);
    AddLocal(node.variable);
  }

  void Visit(const InQueryCall &node) override {
    WalkList(node.arguments);
    for (const auto &item : node.yield_items) {
      AddLocal(item.variable);
    }
    WalkMaybe(node.yield_where);
  }

  void Visit(const ProjectionBody &node) override {
    for (const auto &item : node.items) {
      WalkMaybe(item);
    }
    for (const auto &item : node.items) {
      if (item) {
        AddLocal(item->alias);
      }
    }
    WalkList(node.order_by);
    WalkMaybe(node.skip);
    WalkMaybe(node.limit);
  }

  void Visit(const With &node) override {
    if (node.body) {
      node.body->Accept(*this);
    }
    WalkMaybe(node.where);
  }

 private:
  void VisitQuantifier(const Quantifier &node) {
    WalkMaybe(node.list_expr);
    PushScope({node.variable});
    WalkMaybe(node.predicate);
    PopScope();
  }

  void PushScope(std::initializer_list<std::string> names = {}) {
    local_scopes_.emplace_back();
    for (const std::string &name : names) {
      AddLocal(name);
    }
  }

  void PopScope() {
    CHECK(!local_scopes_.empty(), common::InternalError,
          "expression dependency scope stack is empty");
    local_scopes_.pop_back();
  }

  void AddLocal(const std::string &name) {
    if (name.empty()) {
      return;
    }
    CHECK(!local_scopes_.empty(), common::InternalError,
          "expression dependency local scope is missing");
    local_scopes_.back().insert(name);
  }

  [[nodiscard]] bool IsLocal(const std::string &name) const {
    for (auto it = local_scopes_.rbegin(); it != local_scopes_.rend(); ++it) {
      if (it->contains(name)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool IsOuter(const std::string &name) const {
    return outer_scope_.contains(name) && !IsLocal(name);
  }

  void AddPatternBinding(const std::string &name) {
    if (name.empty()) {
      return;
    }
    if (IsOuter(name)) {
      dependencies_.insert(name);
      return;
    }
    AddLocal(name);
  }

  void AddPatternBindings(const Pattern &pattern) {
    for (const auto &part : pattern.parts) {
      if (part) {
        AddPatternBindings(*part);
      }
    }
  }

  void AddPatternBindings(const PatternPart &part) {
    AddPatternBinding(part.variable);
    if (part.element) {
      AddPatternBindings(*part.element);
    }
  }

  void AddPatternBindings(const PatternElement &element) {
    if (element.node_pattern) {
      AddPatternBindings(*element.node_pattern);
    }
    for (const auto &link : element.chain) {
      if (link.first) {
        AddPatternBindings(*link.first);
      }
      if (link.second) {
        AddPatternBindings(*link.second);
      }
    }
  }

  void AddPatternBindings(const RelationshipsPattern &pattern) {
    if (pattern.node_pattern) {
      AddPatternBindings(*pattern.node_pattern);
    }
    for (const auto &link : pattern.chain) {
      if (link.first) {
        AddPatternBindings(*link.first);
      }
      if (link.second) {
        AddPatternBindings(*link.second);
      }
    }
  }

  void AddPatternBindings(const NodePattern &node) {
    AddPatternBinding(node.variable);
  }

  void AddPatternBindings(const RelationshipPattern &pattern) {
    if (pattern.detail) {
      AddPatternBindings(*pattern.detail);
    }
  }

  void AddPatternBindings(const RelationshipDetail &detail) {
    AddPatternBinding(detail.variable);
  }

  std::unordered_set<std::string> outer_scope_;
  std::vector<std::unordered_set<std::string>> local_scopes_;
  std::unordered_set<std::string> dependencies_;
};

}  // namespace detail

inline std::unordered_set<std::string> CollectExpressionDependencies(
    const Expression &expression,
    std::unordered_set<std::string> outer_scope = {}) {
  detail::ExpressionDependencyCollector collector(std::move(outer_scope));
  return collector.Collect(expression);
}

}  // namespace ast
