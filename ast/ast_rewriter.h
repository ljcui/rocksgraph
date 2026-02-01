#pragma once

#include <vector>

#include "ast.h"

namespace ast {

class ASTRewriter : public ASTVisitor {
 public:
 void rewrite(ASTNode &node);

 protected:
  void rewriteMaybe(std::unique_ptr<Expression> &ptr) {
    if (ptr) {
      rewriteExpression(ptr);
    }
  }

  template <typename T>
  void rewriteMaybe(std::unique_ptr<T> &ptr) {
    if (ptr) {
      ptr->accept(*this);
    }
  }

  template <typename T>
  void rewriteList(std::vector<std::unique_ptr<T>> &list) {
    for (auto &item : list) {
      rewriteMaybe(item);
    }
  }

  virtual void rewriteExpression(std::unique_ptr<Expression> &expr);

  void visit(Statement &node) override;
  void visit(Query &node) override;
  void visit(RegularQuery &node) override;
  void visit(StandaloneCall &node) override;
  void visit(SingleQuery &node) override;
  void visit(SinglePartQuery &node) override;
  void visit(MultiPartQuery &node) override;
  void visit(UnionPart &node) override;

  void visit(Expression &node) override;
  void visit(BinaryExpression &node) override;
  void visit(OrExpression &node) override;
  void visit(XorExpression &node) override;
  void visit(AndExpression &node) override;
  void visit(ComparisonExpression &node) override;
  void visit(ComparisonChainExpression &node) override;
  void visit(AddExpression &node) override;
  void visit(SubtractExpression &node) override;
  void visit(MultiplyExpression &node) override;
  void visit(DivideExpression &node) override;
  void visit(ModuloExpression &node) override;
  void visit(PowerExpression &node) override;
  void visit(UnaryExpression &node) override;
  void visit(NotExpression &node) override;
  void visit(UnaryPlusExpression &node) override;
  void visit(UnaryMinusExpression &node) override;
  void visit(StringPredicateExpression &node) override;
  void visit(ListPredicateExpression &node) override;
  void visit(LabelPredicateExpression &node) override;
  void visit(NullPredicateExpression &node) override;

  void visit(Literal &node) override;
  void visit(BooleanLiteral &node) override;
  void visit(IntegerLiteral &node) override;
  void visit(DoubleLiteral &node) override;
  void visit(StringLiteral &node) override;
  void visit(NullLiteral &node) override;
  void visit(ListLiteral &node) override;
  void visit(MapLiteral &node) override;
  void visit(Properties &node) override;

  void visit(Variable &node) override;
  void visit(Parameter &node) override;
  void visit(PropertyExpression &node) override;
  void visit(ListIndexExpression &node) override;
  void visit(ListSliceExpression &node) override;
  void visit(FunctionInvocation &node) override;
  void visit(CountStarExpression &node) override;
  void visit(CaseExpression &node) override;
  void visit(ParenthesizedExpression &node) override;
  void visit(ListComprehension &node) override;
  void visit(PatternComprehension &node) override;
  void visit(PatternPredicateExpression &node) override;
  void visit(Quantifier &node) override;
  void visit(AllQuantifier &node) override;
  void visit(AnyQuantifier &node) override;
  void visit(NoneQuantifier &node) override;
  void visit(SingleQuantifier &node) override;
  void visit(ExistentialSubquery &node) override;

  void visit(Pattern &node) override;
  void visit(PatternPart &node) override;
  void visit(PatternElement &node) override;
  void visit(RelationshipsPattern &node) override;
  void visit(NodePattern &node) override;
  void visit(RelationshipPattern &node) override;
  void visit(RelationshipDetail &node) override;

  void visit(Clause &node) override;
  void visit(ReadingClause &node) override;
  void visit(Match &node) override;
  void visit(Unwind &node) override;
  void visit(InQueryCall &node) override;
  void visit(UpdatingClause &node) override;
  void visit(Create &node) override;
  void visit(Merge &node) override;
  void visit(Delete &node) override;
  void visit(Set &node) override;
  void visit(SetItem &node) override;
  void visit(Remove &node) override;
  void visit(RemoveItem &node) override;
  void visit(ProjectionClause &node) override;
  void visit(ProjectionBody &node) override;
  void visit(ProjectionItem &node) override;
  void visit(SortItem &node) override;
  void visit(With &node) override;
  void visit(Return &node) override;
};

}  // namespace ast
