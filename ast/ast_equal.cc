#include "ast_equal.h"

#include <typeinfo>

namespace ast {

bool ASTEqual::equal(const ASTNode &left, const ASTNode &right) {
  return equal(&left, &right);
}

bool ASTEqual::equal(const ASTNode *left, const ASTNode *right) {
  if (left == right) {
    return true;
  }
  if ((left == nullptr) || (right == nullptr)) {
    return false;
  }
  if (typeid(*left) != typeid(*right)) {
    return false;
  }

#define AST_EQUAL_TYPE(Type, Func)                  \
  if (typeid(*left) == typeid(Type)) {              \
    return Func(static_cast<const Type &>(*left),   \
                static_cast<const Type &>(*right)); \
  }

  AST_EQUAL_TYPE(RegularQuery, equalRegularQuery)
  AST_EQUAL_TYPE(StandaloneCall, equalStandaloneCall)
  AST_EQUAL_TYPE(SinglePartQuery, equalSinglePartQuery)
  AST_EQUAL_TYPE(MultiPartQuery, equalMultiPartQuery)
  AST_EQUAL_TYPE(UnionPart, equalUnionPart)

  AST_EQUAL_TYPE(OrExpression, equalOrExpression)
  AST_EQUAL_TYPE(XorExpression, equalXorExpression)
  AST_EQUAL_TYPE(AndExpression, equalAndExpression)
  AST_EQUAL_TYPE(ComparisonExpression, equalComparisonExpression)
  AST_EQUAL_TYPE(ComparisonChainExpression, equalComparisonChainExpression)
  AST_EQUAL_TYPE(AddExpression, equalAddExpression)
  AST_EQUAL_TYPE(SubtractExpression, equalSubtractExpression)
  AST_EQUAL_TYPE(MultiplyExpression, equalMultiplyExpression)
  AST_EQUAL_TYPE(DivideExpression, equalDivideExpression)
  AST_EQUAL_TYPE(ModuloExpression, equalModuloExpression)
  AST_EQUAL_TYPE(PowerExpression, equalPowerExpression)
  AST_EQUAL_TYPE(NotExpression, equalNotExpression)
  AST_EQUAL_TYPE(UnaryPlusExpression, equalUnaryPlusExpression)
  AST_EQUAL_TYPE(UnaryMinusExpression, equalUnaryMinusExpression)
  AST_EQUAL_TYPE(StringPredicateExpression, equalStringPredicateExpression)
  AST_EQUAL_TYPE(ListPredicateExpression, equalListPredicateExpression)
  AST_EQUAL_TYPE(LabelPredicateExpression, equalLabelPredicateExpression)
  AST_EQUAL_TYPE(NullPredicateExpression, equalNullPredicateExpression)

  AST_EQUAL_TYPE(BooleanLiteral, equalBooleanLiteral)
  AST_EQUAL_TYPE(IntegerLiteral, equalIntegerLiteral)
  AST_EQUAL_TYPE(DoubleLiteral, equalDoubleLiteral)
  AST_EQUAL_TYPE(StringLiteral, equalStringLiteral)
  AST_EQUAL_TYPE(NullLiteral, equalNullLiteral)
  AST_EQUAL_TYPE(ListLiteral, equalListLiteral)
  AST_EQUAL_TYPE(MapLiteral, equalMapLiteral)
  AST_EQUAL_TYPE(Properties, equalProperties)

  AST_EQUAL_TYPE(Variable, equalVariable)
  AST_EQUAL_TYPE(Parameter, equalParameter)
  AST_EQUAL_TYPE(PropertyExpression, equalPropertyExpression)
  AST_EQUAL_TYPE(ListIndexExpression, equalListIndexExpression)
  AST_EQUAL_TYPE(ListSliceExpression, equalListSliceExpression)
  AST_EQUAL_TYPE(FunctionInvocation, equalFunctionInvocation)
  AST_EQUAL_TYPE(CountStarExpression, equalCountStarExpression)
  AST_EQUAL_TYPE(CaseExpression, equalCaseExpression)
  AST_EQUAL_TYPE(ParenthesizedExpression, equalParenthesizedExpression)
  AST_EQUAL_TYPE(ListComprehension, equalListComprehension)
  AST_EQUAL_TYPE(PatternComprehension, equalPatternComprehension)
  AST_EQUAL_TYPE(PatternPredicateExpression, equalPatternPredicateExpression)
  AST_EQUAL_TYPE(AllQuantifier, equalAllQuantifier)
  AST_EQUAL_TYPE(AnyQuantifier, equalAnyQuantifier)
  AST_EQUAL_TYPE(NoneQuantifier, equalNoneQuantifier)
  AST_EQUAL_TYPE(SingleQuantifier, equalSingleQuantifier)
  AST_EQUAL_TYPE(ExistentialSubquery, equalExistentialSubquery)

  AST_EQUAL_TYPE(Pattern, equalPattern)
  AST_EQUAL_TYPE(PatternPart, equalPatternPart)
  AST_EQUAL_TYPE(PatternElement, equalPatternElement)
  AST_EQUAL_TYPE(RelationshipsPattern, equalRelationshipsPattern)
  AST_EQUAL_TYPE(NodePattern, equalNodePattern)
  AST_EQUAL_TYPE(RelationshipPattern, equalRelationshipPattern)
  AST_EQUAL_TYPE(RelationshipDetail, equalRelationshipDetail)

  AST_EQUAL_TYPE(Match, equalMatch)
  AST_EQUAL_TYPE(Unwind, equalUnwind)
  AST_EQUAL_TYPE(InQueryCall, equalInQueryCall)
  AST_EQUAL_TYPE(Create, equalCreate)
  AST_EQUAL_TYPE(Merge, equalMerge)
  AST_EQUAL_TYPE(Delete, equalDelete)
  AST_EQUAL_TYPE(Set, equalSet)
  AST_EQUAL_TYPE(SetItem, equalSetItem)
  AST_EQUAL_TYPE(Remove, equalRemove)
  AST_EQUAL_TYPE(RemoveItem, equalRemoveItem)
  AST_EQUAL_TYPE(ProjectionBody, equalProjectionBody)
  AST_EQUAL_TYPE(ProjectionItem, equalProjectionItem)
  AST_EQUAL_TYPE(SortItem, equalSortItem)
  AST_EQUAL_TYPE(With, equalWith)
  AST_EQUAL_TYPE(Return, equalReturn)

  AST_EQUAL_TYPE(Statement, equalStatement)
  AST_EQUAL_TYPE(Query, equalQuery)
  AST_EQUAL_TYPE(SingleQuery, equalSingleQuery)
  AST_EQUAL_TYPE(Expression, equalExpression)
  AST_EQUAL_TYPE(BinaryExpression, equalBinaryExpression)
  AST_EQUAL_TYPE(UnaryExpression, equalUnaryExpression)
  AST_EQUAL_TYPE(Literal, equalLiteral)
  AST_EQUAL_TYPE(Quantifier, equalQuantifier)
  AST_EQUAL_TYPE(Clause, equalClause)
  AST_EQUAL_TYPE(ReadingClause, equalReadingClause)
  AST_EQUAL_TYPE(UpdatingClause, equalUpdatingClause)
  AST_EQUAL_TYPE(ProjectionClause, equalProjectionClause)

#undef AST_EQUAL_TYPE

  return false;
}

bool ASTEqual::equalStatement(const Statement & /*unused*/,
                              const Statement & /*unused*/) {
  return true;
}

bool ASTEqual::equalQuery(const Query & /*unused*/, const Query & /*unused*/) {
  return true;
}

bool ASTEqual::equalRegularQuery(const RegularQuery &left,
                                 const RegularQuery &right) {
  return equalPtr(left.single_query, right.single_query) &&
         equalList(left.unions, right.unions);
}

bool ASTEqual::equalStandaloneCall(const StandaloneCall &left,
                                   const StandaloneCall &right) {
  if (left.procedure_name != right.procedure_name) {
    return false;
  }
  if (left.yield_star != right.yield_star) {
    return false;
  }
  if (!equalList(left.arguments, right.arguments)) {
    return false;
  }
  if (left.yield_items.size() != right.yield_items.size()) {
    return false;
  }
  for (size_t i = 0; i < left.yield_items.size(); ++i) {
    const auto &l = left.yield_items[i];
    const auto &r = right.yield_items[i];
    if (l.result_field != r.result_field || l.variable != r.variable) {
      return false;
    }
  }
  return equalPtr(left.yield_where, right.yield_where);
}

bool ASTEqual::equalSingleQuery(const SingleQuery & /*unused*/,
                                const SingleQuery & /*unused*/) {
  return true;
}

bool ASTEqual::equalSinglePartQuery(const SinglePartQuery &left,
                                    const SinglePartQuery &right) {
  return equalList(left.reading_clauses, right.reading_clauses) &&
         equalList(left.updating_clauses, right.updating_clauses) &&
         equalPtr(left.return_clause, right.return_clause);
}

bool ASTEqual::equalMultiPartQuery(const MultiPartQuery &left,
                                   const MultiPartQuery &right) {
  if (left.parts.size() != right.parts.size()) {
    return false;
  }
  for (size_t i = 0; i < left.parts.size(); ++i) {
    const auto &lp = left.parts[i];
    const auto &rp = right.parts[i];
    if (!equalList(lp.reading_clauses, rp.reading_clauses) ||
        !equalList(lp.updating_clauses, rp.updating_clauses) ||
        !equalPtr(lp.with_clause, rp.with_clause)) {
      return false;
    }
  }
  return equalPtr(left.final_single_part_query, right.final_single_part_query);
}

bool ASTEqual::equalUnionPart(const UnionPart &left, const UnionPart &right) {
  return left.all == right.all && equalPtr(left.query, right.query);
}

bool ASTEqual::equalExpression(const Expression & /*unused*/,
                               const Expression & /*unused*/) {
  return true;
}

bool ASTEqual::equalBinaryExpression(const BinaryExpression &left,
                                     const BinaryExpression &right) {
  return equalPtr(left.left, right.left) && equalPtr(left.right, right.right);
}

bool ASTEqual::equalOrExpression(const OrExpression &left,
                                 const OrExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalXorExpression(const XorExpression &left,
                                  const XorExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalAndExpression(const AndExpression &left,
                                  const AndExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalComparisonExpression(const ComparisonExpression &left,
                                         const ComparisonExpression &right) {
  return left.op == right.op && equalPtr(left.left, right.left) &&
         equalPtr(left.right, right.right);
}

bool ASTEqual::equalComparisonChainExpression(
    const ComparisonChainExpression &left,
    const ComparisonChainExpression &right) {
  if (!equalPtr(left.left, right.left)) {
    return false;
  }
  if (left.rights.size() != right.rights.size()) {
    return false;
  }
  for (size_t i = 0; i < left.rights.size(); ++i) {
    if (left.rights[i].first != right.rights[i].first) {
      return false;
    }
    if (!equalPtr(left.rights[i].second, right.rights[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::equalAddExpression(const AddExpression &left,
                                  const AddExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalSubtractExpression(const SubtractExpression &left,
                                       const SubtractExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalMultiplyExpression(const MultiplyExpression &left,
                                       const MultiplyExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalDivideExpression(const DivideExpression &left,
                                     const DivideExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalModuloExpression(const ModuloExpression &left,
                                     const ModuloExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalPowerExpression(const PowerExpression &left,
                                    const PowerExpression &right) {
  return equalBinaryExpression(left, right);
}

bool ASTEqual::equalUnaryExpression(const UnaryExpression &left,
                                    const UnaryExpression &right) {
  return equalPtr(left.operand, right.operand);
}

bool ASTEqual::equalNotExpression(const NotExpression &left,
                                  const NotExpression &right) {
  return equalUnaryExpression(left, right);
}

bool ASTEqual::equalUnaryPlusExpression(const UnaryPlusExpression &left,
                                        const UnaryPlusExpression &right) {
  return equalUnaryExpression(left, right);
}

bool ASTEqual::equalUnaryMinusExpression(const UnaryMinusExpression &left,
                                         const UnaryMinusExpression &right) {
  return equalUnaryExpression(left, right);
}

bool ASTEqual::equalStringPredicateExpression(
    const StringPredicateExpression &left,
    const StringPredicateExpression &right) {
  return left.op == right.op && equalPtr(left.left, right.left) &&
         equalPtr(left.right, right.right);
}

bool ASTEqual::equalListPredicateExpression(
    const ListPredicateExpression &left, const ListPredicateExpression &right) {
  return equalPtr(left.element, right.element) &&
         equalPtr(left.list, right.list);
}

bool ASTEqual::equalLabelPredicateExpression(
    const LabelPredicateExpression &left,
    const LabelPredicateExpression &right) {
  return left.labels == right.labels && equalPtr(left.expr, right.expr);
}

bool ASTEqual::equalNullPredicateExpression(
    const NullPredicateExpression &left, const NullPredicateExpression &right) {
  return left.is_null == right.is_null && equalPtr(left.operand, right.operand);
}

bool ASTEqual::equalLiteral(const Literal & /*unused*/,
                            const Literal & /*unused*/) {
  return true;
}

bool ASTEqual::equalBooleanLiteral(const BooleanLiteral &left,
                                   const BooleanLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::equalIntegerLiteral(const IntegerLiteral &left,
                                   const IntegerLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::equalDoubleLiteral(const DoubleLiteral &left,
                                  const DoubleLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::equalStringLiteral(const StringLiteral &left,
                                  const StringLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::equalNullLiteral(const NullLiteral & /*unused*/,
                                const NullLiteral & /*unused*/) {
  return true;
}

bool ASTEqual::equalListLiteral(const ListLiteral &left,
                                const ListLiteral &right) {
  return equalList(left.elements, right.elements);
}

bool ASTEqual::equalMapLiteral(const MapLiteral &left,
                               const MapLiteral &right) {
  if (left.entries.size() != right.entries.size()) {
    return false;
  }
  for (size_t i = 0; i < left.entries.size(); ++i) {
    if (left.entries[i].first != right.entries[i].first) {
      return false;
    }
    if (!equalPtr(left.entries[i].second, right.entries[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::equalProperties(const Properties &left,
                               const Properties &right) {
  return equalPtr(left.map, right.map) &&
         equalPtr(left.parameter, right.parameter);
}

bool ASTEqual::equalVariable(const Variable &left, const Variable &right) {
  return left.name == right.name;
}

bool ASTEqual::equalParameter(const Parameter &left, const Parameter &right) {
  return left.name == right.name;
}

bool ASTEqual::equalPropertyExpression(const PropertyExpression &left,
                                       const PropertyExpression &right) {
  return left.property_key == right.property_key &&
         equalPtr(left.object, right.object);
}

bool ASTEqual::equalListIndexExpression(const ListIndexExpression &left,
                                        const ListIndexExpression &right) {
  return equalPtr(left.list, right.list) && equalPtr(left.index, right.index);
}

bool ASTEqual::equalListSliceExpression(const ListSliceExpression &left,
                                        const ListSliceExpression &right) {
  return equalPtr(left.list, right.list) &&
         equalPtr(left.start_index, right.start_index) &&
         equalPtr(left.end_index, right.end_index);
}

bool ASTEqual::equalFunctionInvocation(const FunctionInvocation &left,
                                       const FunctionInvocation &right) {
  return left.function_name == right.function_name &&
         left.distinct == right.distinct &&
         equalList(left.arguments, right.arguments);
}

bool ASTEqual::equalCountStarExpression(
    const CountStarExpression & /*unused*/,
    const CountStarExpression & /*unused*/) {
  return true;
}

bool ASTEqual::equalCaseExpression(const CaseExpression &left,
                                   const CaseExpression &right) {
  if (!equalPtr(left.test, right.test)) {
    return false;
  }
  if (left.alternatives.size() != right.alternatives.size()) {
    return false;
  }
  for (size_t i = 0; i < left.alternatives.size(); ++i) {
    if (!equalPtr(left.alternatives[i].first, right.alternatives[i].first) ||
        !equalPtr(left.alternatives[i].second, right.alternatives[i].second)) {
      return false;
    }
  }
  return equalPtr(left.else_expr, right.else_expr);
}

bool ASTEqual::equalParenthesizedExpression(
    const ParenthesizedExpression &left, const ParenthesizedExpression &right) {
  return equalPtr(left.expr, right.expr);
}

bool ASTEqual::equalListComprehension(const ListComprehension &left,
                                      const ListComprehension &right) {
  return left.variable == right.variable &&
         equalPtr(left.list_expr, right.list_expr) &&
         equalPtr(left.where_expr, right.where_expr) &&
         equalPtr(left.eval_expr, right.eval_expr);
}

bool ASTEqual::equalPatternComprehension(const PatternComprehension &left,
                                         const PatternComprehension &right) {
  return left.variable == right.variable &&
         equalPtr(left.relationships_pattern, right.relationships_pattern) &&
         equalPtr(left.where_expr, right.where_expr) &&
         equalPtr(left.eval_expr, right.eval_expr);
}

bool ASTEqual::equalPatternPredicateExpression(
    const PatternPredicateExpression &left,
    const PatternPredicateExpression &right) {
  return equalPtr(left.relationships_pattern, right.relationships_pattern);
}

bool ASTEqual::equalQuantifier(const Quantifier &left,
                               const Quantifier &right) {
  return left.variable == right.variable &&
         equalPtr(left.list_expr, right.list_expr) &&
         equalPtr(left.predicate, right.predicate);
}

bool ASTEqual::equalAllQuantifier(const AllQuantifier &left,
                                  const AllQuantifier &right) {
  return equalQuantifier(left, right);
}

bool ASTEqual::equalAnyQuantifier(const AnyQuantifier &left,
                                  const AnyQuantifier &right) {
  return equalQuantifier(left, right);
}

bool ASTEqual::equalNoneQuantifier(const NoneQuantifier &left,
                                   const NoneQuantifier &right) {
  return equalQuantifier(left, right);
}

bool ASTEqual::equalSingleQuantifier(const SingleQuantifier &left,
                                     const SingleQuantifier &right) {
  return equalQuantifier(left, right);
}

bool ASTEqual::equalExistentialSubquery(const ExistentialSubquery &left,
                                        const ExistentialSubquery &right) {
  return equalPtr(left.query, right.query) &&
         equalPtr(left.pattern, right.pattern) &&
         equalPtr(left.where_expr, right.where_expr);
}

bool ASTEqual::equalPattern(const Pattern &left, const Pattern &right) {
  return equalList(left.parts, right.parts);
}

bool ASTEqual::equalPatternPart(const PatternPart &left,
                                const PatternPart &right) {
  return left.variable == right.variable &&
         equalPtr(left.element, right.element);
}

bool ASTEqual::equalPatternElement(const PatternElement &left,
                                   const PatternElement &right) {
  if (!equalPtr(left.node_pattern, right.node_pattern)) {
    return false;
  }
  if (left.chain.size() != right.chain.size()) {
    return false;
  }
  for (size_t i = 0; i < left.chain.size(); ++i) {
    if (!equalPtr(left.chain[i].first, right.chain[i].first) ||
        !equalPtr(left.chain[i].second, right.chain[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::equalRelationshipsPattern(const RelationshipsPattern &left,
                                         const RelationshipsPattern &right) {
  if (!equalPtr(left.node_pattern, right.node_pattern)) {
    return false;
  }
  if (left.chain.size() != right.chain.size()) {
    return false;
  }
  for (size_t i = 0; i < left.chain.size(); ++i) {
    if (!equalPtr(left.chain[i].first, right.chain[i].first) ||
        !equalPtr(left.chain[i].second, right.chain[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::equalNodePattern(const NodePattern &left,
                                const NodePattern &right) {
  return left.variable == right.variable && left.labels == right.labels &&
         equalPtr(left.properties, right.properties);
}

bool ASTEqual::equalRelationshipPattern(const RelationshipPattern &left,
                                        const RelationshipPattern &right) {
  return left.left_arrow == right.left_arrow &&
         left.right_arrow == right.right_arrow &&
         equalPtr(left.detail, right.detail);
}

bool ASTEqual::equalRelationshipDetail(const RelationshipDetail &left,
                                       const RelationshipDetail &right) {
  if (left.variable != right.variable || left.types != right.types ||
      left.range.has_value() != right.range.has_value()) {
    return false;
  }
  if (left.range.has_value()) {
    const auto &lr = left.range.value();
    const auto &rr = right.range.value();
    if (lr.min != rr.min || lr.max != rr.max) {
      return false;
    }
  }
  return equalPtr(left.properties, right.properties);
}

bool ASTEqual::equalClause(const Clause & /*unused*/,
                           const Clause & /*unused*/) {
  return true;
}

bool ASTEqual::equalReadingClause(const ReadingClause & /*unused*/,
                                  const ReadingClause & /*unused*/) {
  return true;
}

bool ASTEqual::equalMatch(const Match &left, const Match &right) {
  return left.optional_match == right.optional_match &&
         equalPtr(left.pattern, right.pattern) &&
         equalPtr(left.where, right.where);
}

bool ASTEqual::equalUnwind(const Unwind &left, const Unwind &right) {
  return left.variable == right.variable &&
         equalPtr(left.expression, right.expression);
}

bool ASTEqual::equalInQueryCall(const InQueryCall &left,
                                const InQueryCall &right) {
  if (left.procedure_name != right.procedure_name) {
    return false;
  }
  if (!equalList(left.arguments, right.arguments)) {
    return false;
  }
  if (left.yield_items.size() != right.yield_items.size()) {
    return false;
  }
  for (size_t i = 0; i < left.yield_items.size(); ++i) {
    const auto &l = left.yield_items[i];
    const auto &r = right.yield_items[i];
    if (l.result_field != r.result_field || l.variable != r.variable) {
      return false;
    }
  }
  return equalPtr(left.yield_where, right.yield_where);
}

bool ASTEqual::equalUpdatingClause(const UpdatingClause & /*unused*/,
                                   const UpdatingClause & /*unused*/) {
  return true;
}

bool ASTEqual::equalCreate(const Create &left, const Create &right) {
  return equalPtr(left.pattern, right.pattern);
}

bool ASTEqual::equalMerge(const Merge &left, const Merge &right) {
  if (!equalPtr(left.pattern_part, right.pattern_part)) {
    return false;
  }
  if (left.actions.size() != right.actions.size()) {
    return false;
  }
  for (size_t i = 0; i < left.actions.size(); ++i) {
    if (left.actions[i].first != right.actions[i].first) {
      return false;
    }
    if (!equalPtr(left.actions[i].second, right.actions[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::equalDelete(const Delete &left, const Delete &right) {
  return left.detach == right.detach &&
         equalList(left.expressions, right.expressions);
}

bool ASTEqual::equalSet(const Set &left, const Set &right) {
  return equalList(left.items, right.items);
}

bool ASTEqual::equalSetItem(const SetItem &left, const SetItem &right) {
  return left.type == right.type && left.plus_equal == right.plus_equal &&
         left.labels == right.labels && equalPtr(left.target, right.target) &&
         equalPtr(left.value, right.value);
}

bool ASTEqual::equalRemove(const Remove &left, const Remove &right) {
  return equalList(left.items, right.items);
}

bool ASTEqual::equalRemoveItem(const RemoveItem &left,
                               const RemoveItem &right) {
  return left.type == right.type && left.labels == right.labels &&
         equalPtr(left.target, right.target);
}

bool ASTEqual::equalProjectionClause(const ProjectionClause &left,
                                     const ProjectionClause &right) {
  return equalPtr(left.body, right.body);
}

bool ASTEqual::equalProjectionBody(const ProjectionBody &left,
                                   const ProjectionBody &right) {
  return left.distinct == right.distinct && left.star == right.star &&
         equalList(left.items, right.items) &&
         equalList(left.order_by, right.order_by) &&
         equalPtr(left.skip, right.skip) && equalPtr(left.limit, right.limit);
}

bool ASTEqual::equalProjectionItem(const ProjectionItem &left,
                                   const ProjectionItem &right) {
  return left.alias == right.alias &&
         equalPtr(left.expression, right.expression);
}

bool ASTEqual::equalSortItem(const SortItem &left, const SortItem &right) {
  return left.ascending == right.ascending &&
         equalPtr(left.expression, right.expression);
}

bool ASTEqual::equalWith(const With &left, const With &right) {
  return equalPtr(left.body, right.body) && equalPtr(left.where, right.where);
}

bool ASTEqual::equalReturn(const Return &left, const Return &right) {
  return equalPtr(left.body, right.body);
}

}  // namespace ast
