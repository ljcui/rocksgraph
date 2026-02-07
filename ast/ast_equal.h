#pragma once

#include <memory>

#include "ast_node.h"

namespace ast {

class ASTEqual {
 public:
  static bool Equal(const ASTNode *left, const ASTNode *right);
  static bool Equal(const ASTNode &left, const ASTNode &right);

 private:
  template <typename T>
  static bool EqualPtr(const std::unique_ptr<T> &left,
                       const std::unique_ptr<T> &right) {
    if (!left || !right) {
      return left == right;
    }
    return Equal(left.get(), right.get());
  }

  template <typename T>
  static bool EqualList(const std::vector<std::unique_ptr<T>> &left,
                        const std::vector<std::unique_ptr<T>> &right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
      if (!EqualPtr(left[i], right[i])) {
        return false;
      }
    }
    return true;
  }

  static bool EqualStatement(const Statement &left, const Statement &right);
  static bool EqualQuery(const Query &left, const Query &right);
  static bool EqualRegularQuery(const RegularQuery &left,
                                const RegularQuery &right);
  static bool EqualStandaloneCall(const StandaloneCall &left,
                                  const StandaloneCall &right);
  static bool EqualSingleQuery(const SingleQuery &left,
                               const SingleQuery &right);
  static bool EqualSinglePartQuery(const SinglePartQuery &left,
                                   const SinglePartQuery &right);
  static bool EqualMultiPartQuery(const MultiPartQuery &left,
                                  const MultiPartQuery &right);
  static bool EqualUnionPart(const UnionPart &left, const UnionPart &right);

  static bool EqualExpression(const Expression &left, const Expression &right);
  static bool EqualBinaryExpression(const BinaryExpression &left,
                                    const BinaryExpression &right);
  static bool EqualOrExpression(const OrExpression &left,
                                const OrExpression &right);
  static bool EqualXorExpression(const XorExpression &left,
                                 const XorExpression &right);
  static bool EqualAndExpression(const AndExpression &left,
                                 const AndExpression &right);
  static bool EqualComparisonExpression(const ComparisonExpression &left,
                                        const ComparisonExpression &right);
  static bool EqualComparisonChainExpression(
      const ComparisonChainExpression &left,
      const ComparisonChainExpression &right);
  static bool EqualAddExpression(const AddExpression &left,
                                 const AddExpression &right);
  static bool EqualSubtractExpression(const SubtractExpression &left,
                                      const SubtractExpression &right);
  static bool EqualMultiplyExpression(const MultiplyExpression &left,
                                      const MultiplyExpression &right);
  static bool EqualDivideExpression(const DivideExpression &left,
                                    const DivideExpression &right);
  static bool EqualModuloExpression(const ModuloExpression &left,
                                    const ModuloExpression &right);
  static bool EqualPowerExpression(const PowerExpression &left,
                                   const PowerExpression &right);
  static bool EqualUnaryExpression(const UnaryExpression &left,
                                   const UnaryExpression &right);
  static bool EqualNotExpression(const NotExpression &left,
                                 const NotExpression &right);
  static bool EqualUnaryPlusExpression(const UnaryPlusExpression &left,
                                       const UnaryPlusExpression &right);
  static bool EqualUnaryMinusExpression(const UnaryMinusExpression &left,
                                        const UnaryMinusExpression &right);
  static bool EqualStringPredicateExpression(
      const StringPredicateExpression &left,
      const StringPredicateExpression &right);
  static bool EqualListPredicateExpression(
      const ListPredicateExpression &left,
      const ListPredicateExpression &right);
  static bool EqualLabelPredicateExpression(
      const LabelPredicateExpression &left,
      const LabelPredicateExpression &right);
  static bool EqualNullPredicateExpression(
      const NullPredicateExpression &left,
      const NullPredicateExpression &right);

  static bool EqualLiteral(const Literal &left, const Literal &right);
  static bool EqualBooleanLiteral(const BooleanLiteral &left,
                                  const BooleanLiteral &right);
  static bool EqualIntegerLiteral(const IntegerLiteral &left,
                                  const IntegerLiteral &right);
  static bool EqualDoubleLiteral(const DoubleLiteral &left,
                                 const DoubleLiteral &right);
  static bool EqualStringLiteral(const StringLiteral &left,
                                 const StringLiteral &right);
  static bool EqualNullLiteral(const NullLiteral &left,
                               const NullLiteral &right);
  static bool EqualListLiteral(const ListLiteral &left,
                               const ListLiteral &right);
  static bool EqualMapLiteral(const MapLiteral &left, const MapLiteral &right);
  static bool EqualProperties(const Properties &left, const Properties &right);

  static bool EqualVariable(const Variable &left, const Variable &right);
  static bool EqualParameter(const Parameter &left, const Parameter &right);
  static bool EqualPropertyExpression(const PropertyExpression &left,
                                      const PropertyExpression &right);
  static bool EqualListIndexExpression(const ListIndexExpression &left,
                                       const ListIndexExpression &right);
  static bool EqualListSliceExpression(const ListSliceExpression &left,
                                       const ListSliceExpression &right);
  static bool EqualFunctionInvocation(const FunctionInvocation &left,
                                      const FunctionInvocation &right);
  static bool EqualCountStarExpression(const CountStarExpression &left,
                                       const CountStarExpression &right);
  static bool EqualCaseExpression(const CaseExpression &left,
                                  const CaseExpression &right);
  static bool EqualParenthesizedExpression(
      const ParenthesizedExpression &left,
      const ParenthesizedExpression &right);
  static bool EqualListComprehension(const ListComprehension &left,
                                     const ListComprehension &right);
  static bool EqualPatternComprehension(const PatternComprehension &left,
                                        const PatternComprehension &right);
  static bool EqualPatternPredicateExpression(
      const PatternPredicateExpression &left,
      const PatternPredicateExpression &right);
  static bool EqualQuantifier(const Quantifier &left, const Quantifier &right);
  static bool EqualAllQuantifier(const AllQuantifier &left,
                                 const AllQuantifier &right);
  static bool EqualAnyQuantifier(const AnyQuantifier &left,
                                 const AnyQuantifier &right);
  static bool EqualNoneQuantifier(const NoneQuantifier &left,
                                  const NoneQuantifier &right);
  static bool EqualSingleQuantifier(const SingleQuantifier &left,
                                    const SingleQuantifier &right);
  static bool EqualExistentialSubquery(const ExistentialSubquery &left,
                                       const ExistentialSubquery &right);

  static bool EqualPattern(const Pattern &left, const Pattern &right);
  static bool EqualPatternPart(const PatternPart &left,
                               const PatternPart &right);
  static bool EqualPatternElement(const PatternElement &left,
                                  const PatternElement &right);
  static bool EqualRelationshipsPattern(const RelationshipsPattern &left,
                                        const RelationshipsPattern &right);
  static bool EqualNodePattern(const NodePattern &left,
                               const NodePattern &right);
  static bool EqualRelationshipPattern(const RelationshipPattern &left,
                                       const RelationshipPattern &right);
  static bool EqualRelationshipDetail(const RelationshipDetail &left,
                                      const RelationshipDetail &right);

  static bool EqualClause(const Clause &left, const Clause &right);
  static bool EqualReadingClause(const ReadingClause &left,
                                 const ReadingClause &right);
  static bool EqualMatch(const Match &left, const Match &right);
  static bool EqualUnwind(const Unwind &left, const Unwind &right);
  static bool EqualInQueryCall(const InQueryCall &left,
                               const InQueryCall &right);
  static bool EqualUpdatingClause(const UpdatingClause &left,
                                  const UpdatingClause &right);
  static bool EqualCreate(const Create &left, const Create &right);
  static bool EqualMerge(const Merge &left, const Merge &right);
  static bool EqualDelete(const Delete &left, const Delete &right);
  static bool EqualSet(const Set &left, const Set &right);
  static bool EqualSetItem(const SetItem &left, const SetItem &right);
  static bool EqualRemove(const Remove &left, const Remove &right);
  static bool EqualRemoveItem(const RemoveItem &left, const RemoveItem &right);
  static bool EqualProjectionClause(const ProjectionClause &left,
                                    const ProjectionClause &right);
  static bool EqualProjectionBody(const ProjectionBody &left,
                                  const ProjectionBody &right);
  static bool EqualProjectionItem(const ProjectionItem &left,
                                  const ProjectionItem &right);
  static bool EqualSortItem(const SortItem &left, const SortItem &right);
  static bool EqualWith(const With &left, const With &right);
  static bool EqualReturn(const Return &left, const Return &right);
};

}  // namespace ast
