#pragma once

#include <ostream>

#include "ast_node.h"

namespace ast {

class ASTPrinter : public ASTVisitor {
 public:
  explicit ASTPrinter(std::ostream &out);
  void Print(ASTNode &node);

  void Visit(Statement &node) override;
  void Visit(Query &node) override;
  void Visit(RegularQuery &node) override;
  void Visit(StandaloneCall &node) override;
  void Visit(SingleQuery &node) override;
  void Visit(SinglePartQuery &node) override;
  void Visit(MultiPartQuery &node) override;
  void Visit(UnionPart &node) override;

  void Visit(Expression &node) override;
  void Visit(BinaryExpression &node) override;
  void Visit(OrExpression &node) override;
  void Visit(XorExpression &node) override;
  void Visit(AndExpression &node) override;
  void Visit(ComparisonExpression &node) override;
  void Visit(ComparisonChainExpression &node) override;
  void Visit(AddExpression &node) override;
  void Visit(SubtractExpression &node) override;
  void Visit(MultiplyExpression &node) override;
  void Visit(DivideExpression &node) override;
  void Visit(ModuloExpression &node) override;
  void Visit(PowerExpression &node) override;
  void Visit(UnaryExpression &node) override;
  void Visit(NotExpression &node) override;
  void Visit(UnaryPlusExpression &node) override;
  void Visit(UnaryMinusExpression &node) override;
  void Visit(StringPredicateExpression &node) override;
  void Visit(ListPredicateExpression &node) override;
  void Visit(LabelPredicateExpression &node) override;
  void Visit(NullPredicateExpression &node) override;

  void Visit(Literal &node) override;
  void Visit(BooleanLiteral &node) override;
  void Visit(IntegerLiteral &node) override;
  void Visit(DoubleLiteral &node) override;
  void Visit(StringLiteral &node) override;
  void Visit(NullLiteral &node) override;
  void Visit(ListLiteral &node) override;
  void Visit(MapLiteral &node) override;
  void Visit(Properties &node) override;

  void Visit(Variable &node) override;
  void Visit(Parameter &node) override;
  void Visit(PropertyExpression &node) override;
  void Visit(ListIndexExpression &node) override;
  void Visit(ListSliceExpression &node) override;
  void Visit(FunctionInvocation &node) override;
  void Visit(CountStarExpression &node) override;
  void Visit(CaseExpression &node) override;
  void Visit(ParenthesizedExpression &node) override;
  void Visit(ListComprehension &node) override;
  void Visit(PatternComprehension &node) override;
  void Visit(PatternPredicateExpression &node) override;
  void Visit(Quantifier &node) override;
  void Visit(AllQuantifier &node) override;
  void Visit(AnyQuantifier &node) override;
  void Visit(NoneQuantifier &node) override;
  void Visit(SingleQuantifier &node) override;
  void Visit(ExistentialSubquery &node) override;

  void Visit(Pattern &node) override;
  void Visit(PatternPart &node) override;
  void Visit(PatternElement &node) override;
  void Visit(RelationshipsPattern &node) override;
  void Visit(NodePattern &node) override;
  void Visit(RelationshipPattern &node) override;
  void Visit(RelationshipDetail &node) override;

  void Visit(Clause &node) override;
  void Visit(ReadingClause &node) override;
  void Visit(Match &node) override;
  void Visit(Unwind &node) override;
  void Visit(InQueryCall &node) override;
  void Visit(UpdatingClause &node) override;
  void Visit(Create &node) override;
  void Visit(Merge &node) override;
  void Visit(Delete &node) override;
  void Visit(Set &node) override;
  void Visit(SetItem &node) override;
  void Visit(Remove &node) override;
  void Visit(RemoveItem &node) override;
  void Visit(ProjectionClause &node) override;
  void Visit(ProjectionBody &node) override;
  void Visit(ProjectionItem &node) override;
  void Visit(SortItem &node) override;
  void Visit(With &node) override;
  void Visit(Return &node) override;

 private:
  void Line(const std::string &text);
  void Indent();
  void Dedent();

  template <typename T>
  void VisitMaybe(const std::unique_ptr<T> &ptr) {
    if (ptr) {
      ptr->Accept(*this);
    }
  }

  std::ostream &out_;
  int indent_ = 0;
};

}  // namespace ast
