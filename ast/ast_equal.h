#pragma once

#include <memory>

#include "ast.h"

namespace ast {

class ASTEqual {
 public:
  static bool equal(const ASTNode *left, const ASTNode *right);
  static bool equal(const ASTNode &left, const ASTNode &right);

 private:
  template <typename T>
  static bool equalPtr(const std::unique_ptr<T> &left,
                       const std::unique_ptr<T> &right) {
    if (!left || !right) {
      return left == right;
    }
    return equal(left.get(), right.get());
  }

  template <typename T>
  static bool equalList(const std::vector<std::unique_ptr<T>> &left,
                        const std::vector<std::unique_ptr<T>> &right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
      if (!equalPtr(left[i], right[i])) {
        return false;
      }
    }
    return true;
  }

  static bool equalStatement(const Statement &left, const Statement &right);
  static bool equalQuery(const Query &left, const Query &right);
  static bool equalRegularQuery(const RegularQuery &left,
                                const RegularQuery &right);
  static bool equalStandaloneCall(const StandaloneCall &left,
                                  const StandaloneCall &right);
  static bool equalSingleQuery(const SingleQuery &left,
                               const SingleQuery &right);
  static bool equalSinglePartQuery(const SinglePartQuery &left,
                                   const SinglePartQuery &right);
  static bool equalMultiPartQuery(const MultiPartQuery &left,
                                  const MultiPartQuery &right);
  static bool equalUnionPart(const UnionPart &left, const UnionPart &right);

  static bool equalExpression(const Expression &left, const Expression &right);
  static bool equalBinaryExpression(const BinaryExpression &left,
                                    const BinaryExpression &right);
  static bool equalOrExpression(const OrExpression &left,
                                const OrExpression &right);
  static bool equalXorExpression(const XorExpression &left,
                                 const XorExpression &right);
  static bool equalAndExpression(const AndExpression &left,
                                 const AndExpression &right);
  static bool equalComparisonExpression(const ComparisonExpression &left,
                                        const ComparisonExpression &right);
  static bool equalComparisonChainExpression(
      const ComparisonChainExpression &left,
      const ComparisonChainExpression &right);
  static bool equalAddExpression(const AddExpression &left,
                                 const AddExpression &right);
  static bool equalSubtractExpression(const SubtractExpression &left,
                                      const SubtractExpression &right);
  static bool equalMultiplyExpression(const MultiplyExpression &left,
                                      const MultiplyExpression &right);
  static bool equalDivideExpression(const DivideExpression &left,
                                    const DivideExpression &right);
  static bool equalModuloExpression(const ModuloExpression &left,
                                    const ModuloExpression &right);
  static bool equalPowerExpression(const PowerExpression &left,
                                   const PowerExpression &right);
  static bool equalUnaryExpression(const UnaryExpression &left,
                                   const UnaryExpression &right);
  static bool equalNotExpression(const NotExpression &left,
                                 const NotExpression &right);
  static bool equalUnaryPlusExpression(const UnaryPlusExpression &left,
                                       const UnaryPlusExpression &right);
  static bool equalUnaryMinusExpression(const UnaryMinusExpression &left,
                                        const UnaryMinusExpression &right);
  static bool equalStringPredicateExpression(
      const StringPredicateExpression &left,
      const StringPredicateExpression &right);
  static bool equalListPredicateExpression(
      const ListPredicateExpression &left,
      const ListPredicateExpression &right);
  static bool equalLabelPredicateExpression(
      const LabelPredicateExpression &left,
      const LabelPredicateExpression &right);
  static bool equalNullPredicateExpression(
      const NullPredicateExpression &left,
      const NullPredicateExpression &right);

  static bool equalLiteral(const Literal &left, const Literal &right);
  static bool equalBooleanLiteral(const BooleanLiteral &left,
                                  const BooleanLiteral &right);
  static bool equalIntegerLiteral(const IntegerLiteral &left,
                                  const IntegerLiteral &right);
  static bool equalDoubleLiteral(const DoubleLiteral &left,
                                 const DoubleLiteral &right);
  static bool equalStringLiteral(const StringLiteral &left,
                                 const StringLiteral &right);
  static bool equalNullLiteral(const NullLiteral &left,
                               const NullLiteral &right);
  static bool equalListLiteral(const ListLiteral &left,
                               const ListLiteral &right);
  static bool equalMapLiteral(const MapLiteral &left, const MapLiteral &right);
  static bool equalProperties(const Properties &left, const Properties &right);

  static bool equalVariable(const Variable &left, const Variable &right);
  static bool equalParameter(const Parameter &left, const Parameter &right);
  static bool equalPropertyExpression(const PropertyExpression &left,
                                      const PropertyExpression &right);
  static bool equalListIndexExpression(const ListIndexExpression &left,
                                       const ListIndexExpression &right);
  static bool equalListSliceExpression(const ListSliceExpression &left,
                                       const ListSliceExpression &right);
  static bool equalFunctionInvocation(const FunctionInvocation &left,
                                      const FunctionInvocation &right);
  static bool equalCountStarExpression(const CountStarExpression &left,
                                       const CountStarExpression &right);
  static bool equalCaseExpression(const CaseExpression &left,
                                  const CaseExpression &right);
  static bool equalParenthesizedExpression(
      const ParenthesizedExpression &left,
      const ParenthesizedExpression &right);
  static bool equalListComprehension(const ListComprehension &left,
                                     const ListComprehension &right);
  static bool equalPatternComprehension(const PatternComprehension &left,
                                        const PatternComprehension &right);
  static bool equalPatternPredicateExpression(
      const PatternPredicateExpression &left,
      const PatternPredicateExpression &right);
  static bool equalQuantifier(const Quantifier &left, const Quantifier &right);
  static bool equalAllQuantifier(const AllQuantifier &left,
                                 const AllQuantifier &right);
  static bool equalAnyQuantifier(const AnyQuantifier &left,
                                 const AnyQuantifier &right);
  static bool equalNoneQuantifier(const NoneQuantifier &left,
                                  const NoneQuantifier &right);
  static bool equalSingleQuantifier(const SingleQuantifier &left,
                                    const SingleQuantifier &right);
  static bool equalExistentialSubquery(const ExistentialSubquery &left,
                                       const ExistentialSubquery &right);

  static bool equalPattern(const Pattern &left, const Pattern &right);
  static bool equalPatternPart(const PatternPart &left,
                               const PatternPart &right);
  static bool equalPatternElement(const PatternElement &left,
                                  const PatternElement &right);
  static bool equalRelationshipsPattern(const RelationshipsPattern &left,
                                        const RelationshipsPattern &right);
  static bool equalNodePattern(const NodePattern &left,
                               const NodePattern &right);
  static bool equalRelationshipPattern(const RelationshipPattern &left,
                                       const RelationshipPattern &right);
  static bool equalRelationshipDetail(const RelationshipDetail &left,
                                      const RelationshipDetail &right);

  static bool equalClause(const Clause &left, const Clause &right);
  static bool equalReadingClause(const ReadingClause &left,
                                 const ReadingClause &right);
  static bool equalMatch(const Match &left, const Match &right);
  static bool equalUnwind(const Unwind &left, const Unwind &right);
  static bool equalInQueryCall(const InQueryCall &left,
                               const InQueryCall &right);
  static bool equalUpdatingClause(const UpdatingClause &left,
                                  const UpdatingClause &right);
  static bool equalCreate(const Create &left, const Create &right);
  static bool equalMerge(const Merge &left, const Merge &right);
  static bool equalDelete(const Delete &left, const Delete &right);
  static bool equalSet(const Set &left, const Set &right);
  static bool equalSetItem(const SetItem &left, const SetItem &right);
  static bool equalRemove(const Remove &left, const Remove &right);
  static bool equalRemoveItem(const RemoveItem &left, const RemoveItem &right);
  static bool equalProjectionClause(const ProjectionClause &left,
                                    const ProjectionClause &right);
  static bool equalProjectionBody(const ProjectionBody &left,
                                  const ProjectionBody &right);
  static bool equalProjectionItem(const ProjectionItem &left,
                                  const ProjectionItem &right);
  static bool equalSortItem(const SortItem &left, const SortItem &right);
  static bool equalWith(const With &left, const With &right);
  static bool equalReturn(const Return &left, const Return &right);
};

}  // namespace ast
