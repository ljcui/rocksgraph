#pragma once

#include <vector>

#include "ast_node.h"

namespace ast {

// ASTConstWalker provides a pure traversal over the AST without modifying it.
// Subclasses override visit methods to observe or collect information.
class ASTConstWalker : public ASTConstVisitor {
 public:
  void Walk(const ASTNode &node);

 protected:
  void WalkMaybe(const std::unique_ptr<Expression> &ptr) {
    if (ptr) {
      WalkExpression(ptr);
    }
  }

  template <typename T>
  void WalkMaybe(const std::unique_ptr<T> &ptr) {
    if (ptr) {
      const ASTNode &node = *ptr;
      node.Accept(*this);
    }
  }

  template <typename T>
  void WalkList(const std::vector<std::unique_ptr<T>> &list) {
    for (const auto &item : list) {
      WalkMaybe(item);
    }
  }

  virtual void WalkExpression(const std::unique_ptr<Expression> &expr);

  void Visit(const Statement &node) override;
  void Visit(const Query &node) override;
  void Visit(const RegularQuery &node) override;
  void Visit(const StandaloneCall &node) override;
  void Visit(const SingleQuery &node) override;
  void Visit(const SinglePartQuery &node) override;
  void Visit(const MultiPartQuery &node) override;
  void Visit(const UnionPart &node) override;

  void Visit(const Expression &node) override;
  void Visit(const BinaryExpression &node) override;
  void Visit(const OrExpression &node) override;
  void Visit(const XorExpression &node) override;
  void Visit(const AndExpression &node) override;
  void Visit(const ComparisonExpression &node) override;
  void Visit(const ComparisonChainExpression &node) override;
  void Visit(const AddExpression &node) override;
  void Visit(const SubtractExpression &node) override;
  void Visit(const MultiplyExpression &node) override;
  void Visit(const DivideExpression &node) override;
  void Visit(const ModuloExpression &node) override;
  void Visit(const PowerExpression &node) override;
  void Visit(const UnaryExpression &node) override;
  void Visit(const NotExpression &node) override;
  void Visit(const UnaryPlusExpression &node) override;
  void Visit(const UnaryMinusExpression &node) override;
  void Visit(const StringPredicateExpression &node) override;
  void Visit(const ListPredicateExpression &node) override;
  void Visit(const LabelPredicateExpression &node) override;
  void Visit(const NullPredicateExpression &node) override;

  void Visit(const Literal &node) override;
  void Visit(const BooleanLiteral &node) override;
  void Visit(const IntegerLiteral &node) override;
  void Visit(const DoubleLiteral &node) override;
  void Visit(const StringLiteral &node) override;
  void Visit(const NullLiteral &node) override;
  void Visit(const ListLiteral &node) override;
  void Visit(const MapLiteral &node) override;
  void Visit(const Properties &node) override;

  void Visit(const Variable &node) override;
  void Visit(const Parameter &node) override;
  void Visit(const PropertyExpression &node) override;
  void Visit(const ListIndexExpression &node) override;
  void Visit(const ListSliceExpression &node) override;
  void Visit(const FunctionInvocation &node) override;
  void Visit(const CountStarExpression &node) override;
  void Visit(const CaseExpression &node) override;
  void Visit(const ParenthesizedExpression &node) override;
  void Visit(const ListComprehension &node) override;
  void Visit(const PatternComprehension &node) override;
  void Visit(const PatternPredicateExpression &node) override;
  void Visit(const Quantifier &node) override;
  void Visit(const AllQuantifier &node) override;
  void Visit(const AnyQuantifier &node) override;
  void Visit(const NoneQuantifier &node) override;
  void Visit(const SingleQuantifier &node) override;
  void Visit(const ExistentialSubquery &node) override;

  void Visit(const Pattern &node) override;
  void Visit(const PatternPart &node) override;
  void Visit(const PatternElement &node) override;
  void Visit(const RelationshipsPattern &node) override;
  void Visit(const NodePattern &node) override;
  void Visit(const RelationshipPattern &node) override;
  void Visit(const RelationshipDetail &node) override;

  void Visit(const Clause &node) override;
  void Visit(const ReadingClause &node) override;
  void Visit(const Match &node) override;
  void Visit(const Unwind &node) override;
  void Visit(const InQueryCall &node) override;
  void Visit(const UpdatingClause &node) override;
  void Visit(const Create &node) override;
  void Visit(const Merge &node) override;
  void Visit(const Delete &node) override;
  void Visit(const Set &node) override;
  void Visit(const SetItem &node) override;
  void Visit(const Remove &node) override;
  void Visit(const RemoveItem &node) override;
  void Visit(const ProjectionClause &node) override;
  void Visit(const ProjectionBody &node) override;
  void Visit(const ProjectionItem &node) override;
  void Visit(const SortItem &node) override;
  void Visit(const With &node) override;
  void Visit(const Return &node) override;
};

}  // namespace ast
