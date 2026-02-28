#include "ast_equal.h"

namespace ast {

bool ASTEqual::Equal(const ASTNode &left, const ASTNode &right) {
  return Equal(&left, &right);
}

bool ASTEqual::Equal(const ASTNode *left, const ASTNode *right) {
  if (left == right) {
    return true;
  }
  if ((left == nullptr) || (right == nullptr)) {
    return false;
  }
  if (left->node_type != right->node_type) {
    return false;
  }

#define AST_EQUAL_CASE(NodeType, Type, Func)      \
  case ASTNodeType::NodeType:                     \
    return Func(static_cast<const Type &>(*left), \
                static_cast<const Type &>(*right))

  switch (left->node_type) {
    AST_EQUAL_CASE(kRegularQuery, RegularQuery, EqualRegularQuery);
    AST_EQUAL_CASE(kStandaloneCall, StandaloneCall, EqualStandaloneCall);
    AST_EQUAL_CASE(kSinglePartQuery, SinglePartQuery, EqualSinglePartQuery);
    AST_EQUAL_CASE(kMultiPartQuery, MultiPartQuery, EqualMultiPartQuery);
    AST_EQUAL_CASE(kUnionPart, UnionPart, EqualUnionPart);

    AST_EQUAL_CASE(kOrExpression, OrExpression, EqualOrExpression);
    AST_EQUAL_CASE(kXorExpression, XorExpression, EqualXorExpression);
    AST_EQUAL_CASE(kAndExpression, AndExpression, EqualAndExpression);
    AST_EQUAL_CASE(kComparisonExpression, ComparisonExpression,
                   EqualComparisonExpression);
    AST_EQUAL_CASE(kComparisonChainExpression, ComparisonChainExpression,
                   EqualComparisonChainExpression);
    AST_EQUAL_CASE(kAddExpression, AddExpression, EqualAddExpression);
    AST_EQUAL_CASE(kSubtractExpression, SubtractExpression,
                   EqualSubtractExpression);
    AST_EQUAL_CASE(kMultiplyExpression, MultiplyExpression,
                   EqualMultiplyExpression);
    AST_EQUAL_CASE(kDivideExpression, DivideExpression, EqualDivideExpression);
    AST_EQUAL_CASE(kModuloExpression, ModuloExpression, EqualModuloExpression);
    AST_EQUAL_CASE(kPowerExpression, PowerExpression, EqualPowerExpression);
    AST_EQUAL_CASE(kNotExpression, NotExpression, EqualNotExpression);
    AST_EQUAL_CASE(kUnaryPlusExpression, UnaryPlusExpression,
                   EqualUnaryPlusExpression);
    AST_EQUAL_CASE(kUnaryMinusExpression, UnaryMinusExpression,
                   EqualUnaryMinusExpression);
    AST_EQUAL_CASE(kStringPredicateExpression, StringPredicateExpression,
                   EqualStringPredicateExpression);
    AST_EQUAL_CASE(kListPredicateExpression, ListPredicateExpression,
                   EqualListPredicateExpression);
    AST_EQUAL_CASE(kLabelPredicateExpression, LabelPredicateExpression,
                   EqualLabelPredicateExpression);
    AST_EQUAL_CASE(kNullPredicateExpression, NullPredicateExpression,
                   EqualNullPredicateExpression);

    AST_EQUAL_CASE(kBooleanLiteral, BooleanLiteral, EqualBooleanLiteral);
    AST_EQUAL_CASE(kIntegerLiteral, IntegerLiteral, EqualIntegerLiteral);
    AST_EQUAL_CASE(kDoubleLiteral, DoubleLiteral, EqualDoubleLiteral);
    AST_EQUAL_CASE(kStringLiteral, StringLiteral, EqualStringLiteral);
    AST_EQUAL_CASE(kNullLiteral, NullLiteral, EqualNullLiteral);
    AST_EQUAL_CASE(kListLiteral, ListLiteral, EqualListLiteral);
    AST_EQUAL_CASE(kMapLiteral, MapLiteral, EqualMapLiteral);
    AST_EQUAL_CASE(kProperties, Properties, EqualProperties);

    AST_EQUAL_CASE(kVariable, Variable, EqualVariable);
    AST_EQUAL_CASE(kParameter, Parameter, EqualParameter);
    AST_EQUAL_CASE(kPropertyExpression, PropertyExpression,
                   EqualPropertyExpression);
    AST_EQUAL_CASE(kListIndexExpression, ListIndexExpression,
                   EqualListIndexExpression);
    AST_EQUAL_CASE(kListSliceExpression, ListSliceExpression,
                   EqualListSliceExpression);
    AST_EQUAL_CASE(kFunctionInvocation, FunctionInvocation,
                   EqualFunctionInvocation);
    AST_EQUAL_CASE(kCountStarExpression, CountStarExpression,
                   EqualCountStarExpression);
    AST_EQUAL_CASE(kCaseExpression, CaseExpression, EqualCaseExpression);
    AST_EQUAL_CASE(kParenthesizedExpression, ParenthesizedExpression,
                   EqualParenthesizedExpression);
    AST_EQUAL_CASE(kListComprehension, ListComprehension,
                   EqualListComprehension);
    AST_EQUAL_CASE(kPatternComprehension, PatternComprehension,
                   EqualPatternComprehension);
    AST_EQUAL_CASE(kPatternPredicateExpression, PatternPredicateExpression,
                   EqualPatternPredicateExpression);
    AST_EQUAL_CASE(kAllQuantifier, AllQuantifier, EqualAllQuantifier);
    AST_EQUAL_CASE(kAnyQuantifier, AnyQuantifier, EqualAnyQuantifier);
    AST_EQUAL_CASE(kNoneQuantifier, NoneQuantifier, EqualNoneQuantifier);
    AST_EQUAL_CASE(kSingleQuantifier, SingleQuantifier, EqualSingleQuantifier);
    AST_EQUAL_CASE(kExistentialSubquery, ExistentialSubquery,
                   EqualExistentialSubquery);

    AST_EQUAL_CASE(kPattern, Pattern, EqualPattern);
    AST_EQUAL_CASE(kPatternPart, PatternPart, EqualPatternPart);
    AST_EQUAL_CASE(kPatternElement, PatternElement, EqualPatternElement);
    AST_EQUAL_CASE(kRelationshipsPattern, RelationshipsPattern,
                   EqualRelationshipsPattern);
    AST_EQUAL_CASE(kNodePattern, NodePattern, EqualNodePattern);
    AST_EQUAL_CASE(kRelationshipPattern, RelationshipPattern,
                   EqualRelationshipPattern);
    AST_EQUAL_CASE(kRelationshipDetail, RelationshipDetail,
                   EqualRelationshipDetail);

    AST_EQUAL_CASE(kMatch, Match, EqualMatch);
    AST_EQUAL_CASE(kUnwind, Unwind, EqualUnwind);
    AST_EQUAL_CASE(kInQueryCall, InQueryCall, EqualInQueryCall);
    AST_EQUAL_CASE(kCreate, Create, EqualCreate);
    AST_EQUAL_CASE(kMerge, Merge, EqualMerge);
    AST_EQUAL_CASE(kDelete, Delete, EqualDelete);
    AST_EQUAL_CASE(kSet, Set, EqualSet);
    AST_EQUAL_CASE(kSetItem, SetItem, EqualSetItem);
    AST_EQUAL_CASE(kRemove, Remove, EqualRemove);
    AST_EQUAL_CASE(kRemoveItem, RemoveItem, EqualRemoveItem);
    AST_EQUAL_CASE(kProjectionBody, ProjectionBody, EqualProjectionBody);
    AST_EQUAL_CASE(kProjectionItem, ProjectionItem, EqualProjectionItem);
    AST_EQUAL_CASE(kSortItem, SortItem, EqualSortItem);
    AST_EQUAL_CASE(kWith, With, EqualWith);
    AST_EQUAL_CASE(kReturn, Return, EqualReturn);

    default:
      return false;
  }

#undef AST_EQUAL_CASE
}

bool ASTEqual::EqualStatement(const Statement & /*unused*/,
                              const Statement & /*unused*/) {
  return true;
}

bool ASTEqual::EqualQuery(const Query & /*unused*/, const Query & /*unused*/) {
  return true;
}

bool ASTEqual::EqualRegularQuery(const RegularQuery &left,
                                 const RegularQuery &right) {
  return EqualPtr(left.single_query, right.single_query) &&
         EqualList(left.unions, right.unions);
}

bool ASTEqual::EqualStandaloneCall(const StandaloneCall &left,
                                   const StandaloneCall &right) {
  if (left.procedure_name != right.procedure_name) {
    return false;
  }
  if (left.yield_star != right.yield_star) {
    return false;
  }
  if (!EqualList(left.arguments, right.arguments)) {
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
  return EqualPtr(left.yield_where, right.yield_where);
}

bool ASTEqual::EqualSingleQuery(const SingleQuery & /*unused*/,
                                const SingleQuery & /*unused*/) {
  return true;
}

bool ASTEqual::EqualSinglePartQuery(const SinglePartQuery &left,
                                    const SinglePartQuery &right) {
  return EqualList(left.reading_clauses, right.reading_clauses) &&
         EqualList(left.updating_clauses, right.updating_clauses) &&
         EqualPtr(left.return_clause, right.return_clause);
}

bool ASTEqual::EqualMultiPartQuery(const MultiPartQuery &left,
                                   const MultiPartQuery &right) {
  if (left.parts.size() != right.parts.size()) {
    return false;
  }
  for (size_t i = 0; i < left.parts.size(); ++i) {
    const auto &lp = left.parts[i];
    const auto &rp = right.parts[i];
    if (!EqualList(lp.reading_clauses, rp.reading_clauses) ||
        !EqualList(lp.updating_clauses, rp.updating_clauses) ||
        !EqualPtr(lp.with_clause, rp.with_clause)) {
      return false;
    }
  }
  return EqualPtr(left.final_single_part_query, right.final_single_part_query);
}

bool ASTEqual::EqualUnionPart(const UnionPart &left, const UnionPart &right) {
  return left.all == right.all && EqualPtr(left.query, right.query);
}

bool ASTEqual::EqualExpression(const Expression & /*unused*/,
                               const Expression & /*unused*/) {
  return true;
}

bool ASTEqual::EqualBinaryExpression(const BinaryExpression &left,
                                     const BinaryExpression &right) {
  return EqualPtr(left.left, right.left) && EqualPtr(left.right, right.right);
}

bool ASTEqual::EqualOrExpression(const OrExpression &left,
                                 const OrExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualXorExpression(const XorExpression &left,
                                  const XorExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualAndExpression(const AndExpression &left,
                                  const AndExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualComparisonExpression(const ComparisonExpression &left,
                                         const ComparisonExpression &right) {
  return left.op == right.op && EqualPtr(left.left, right.left) &&
         EqualPtr(left.right, right.right);
}

bool ASTEqual::EqualComparisonChainExpression(
    const ComparisonChainExpression &left,
    const ComparisonChainExpression &right) {
  if (!EqualPtr(left.left, right.left)) {
    return false;
  }
  if (left.rights.size() != right.rights.size()) {
    return false;
  }
  for (size_t i = 0; i < left.rights.size(); ++i) {
    if (left.rights[i].first != right.rights[i].first) {
      return false;
    }
    if (!EqualPtr(left.rights[i].second, right.rights[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::EqualAddExpression(const AddExpression &left,
                                  const AddExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualSubtractExpression(const SubtractExpression &left,
                                       const SubtractExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualMultiplyExpression(const MultiplyExpression &left,
                                       const MultiplyExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualDivideExpression(const DivideExpression &left,
                                     const DivideExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualModuloExpression(const ModuloExpression &left,
                                     const ModuloExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualPowerExpression(const PowerExpression &left,
                                    const PowerExpression &right) {
  return EqualBinaryExpression(left, right);
}

bool ASTEqual::EqualUnaryExpression(const UnaryExpression &left,
                                    const UnaryExpression &right) {
  return EqualPtr(left.operand, right.operand);
}

bool ASTEqual::EqualNotExpression(const NotExpression &left,
                                  const NotExpression &right) {
  return EqualUnaryExpression(left, right);
}

bool ASTEqual::EqualUnaryPlusExpression(const UnaryPlusExpression &left,
                                        const UnaryPlusExpression &right) {
  return EqualUnaryExpression(left, right);
}

bool ASTEqual::EqualUnaryMinusExpression(const UnaryMinusExpression &left,
                                         const UnaryMinusExpression &right) {
  return EqualUnaryExpression(left, right);
}

bool ASTEqual::EqualStringPredicateExpression(
    const StringPredicateExpression &left,
    const StringPredicateExpression &right) {
  return left.op == right.op && EqualPtr(left.left, right.left) &&
         EqualPtr(left.right, right.right);
}

bool ASTEqual::EqualListPredicateExpression(
    const ListPredicateExpression &left, const ListPredicateExpression &right) {
  return EqualPtr(left.element, right.element) &&
         EqualPtr(left.list, right.list);
}

bool ASTEqual::EqualLabelPredicateExpression(
    const LabelPredicateExpression &left,
    const LabelPredicateExpression &right) {
  return left.labels == right.labels && EqualPtr(left.expr, right.expr);
}

bool ASTEqual::EqualNullPredicateExpression(
    const NullPredicateExpression &left, const NullPredicateExpression &right) {
  return left.is_null == right.is_null && EqualPtr(left.operand, right.operand);
}

bool ASTEqual::EqualLiteral(const Literal & /*unused*/,
                            const Literal & /*unused*/) {
  return true;
}

bool ASTEqual::EqualBooleanLiteral(const BooleanLiteral &left,
                                   const BooleanLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::EqualIntegerLiteral(const IntegerLiteral &left,
                                   const IntegerLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::EqualDoubleLiteral(const DoubleLiteral &left,
                                  const DoubleLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::EqualStringLiteral(const StringLiteral &left,
                                  const StringLiteral &right) {
  return left.value == right.value;
}

bool ASTEqual::EqualNullLiteral(const NullLiteral & /*unused*/,
                                const NullLiteral & /*unused*/) {
  return true;
}

bool ASTEqual::EqualListLiteral(const ListLiteral &left,
                                const ListLiteral &right) {
  return EqualList(left.elements, right.elements);
}

bool ASTEqual::EqualMapLiteral(const MapLiteral &left,
                               const MapLiteral &right) {
  if (left.entries.size() != right.entries.size()) {
    return false;
  }
  for (size_t i = 0; i < left.entries.size(); ++i) {
    if (left.entries[i].first != right.entries[i].first) {
      return false;
    }
    if (!EqualPtr(left.entries[i].second, right.entries[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::EqualProperties(const Properties &left,
                               const Properties &right) {
  return EqualPtr(left.map, right.map) &&
         EqualPtr(left.parameter, right.parameter);
}

bool ASTEqual::EqualVariable(const Variable &left, const Variable &right) {
  return left.name == right.name;
}

bool ASTEqual::EqualParameter(const Parameter &left, const Parameter &right) {
  return left.name == right.name;
}

bool ASTEqual::EqualPropertyExpression(const PropertyExpression &left,
                                       const PropertyExpression &right) {
  return left.property_key == right.property_key &&
         EqualPtr(left.object, right.object);
}

bool ASTEqual::EqualListIndexExpression(const ListIndexExpression &left,
                                        const ListIndexExpression &right) {
  return EqualPtr(left.list, right.list) && EqualPtr(left.index, right.index);
}

bool ASTEqual::EqualListSliceExpression(const ListSliceExpression &left,
                                        const ListSliceExpression &right) {
  return EqualPtr(left.list, right.list) &&
         EqualPtr(left.start_index, right.start_index) &&
         EqualPtr(left.end_index, right.end_index);
}

bool ASTEqual::EqualFunctionInvocation(const FunctionInvocation &left,
                                       const FunctionInvocation &right) {
  return left.function_name == right.function_name &&
         left.distinct == right.distinct &&
         EqualList(left.arguments, right.arguments);
}

bool ASTEqual::EqualCountStarExpression(
    const CountStarExpression & /*unused*/,
    const CountStarExpression & /*unused*/) {
  return true;
}

bool ASTEqual::EqualCaseExpression(const CaseExpression &left,
                                   const CaseExpression &right) {
  if (!EqualPtr(left.test, right.test)) {
    return false;
  }
  if (left.alternatives.size() != right.alternatives.size()) {
    return false;
  }
  for (size_t i = 0; i < left.alternatives.size(); ++i) {
    if (!EqualPtr(left.alternatives[i].first, right.alternatives[i].first) ||
        !EqualPtr(left.alternatives[i].second, right.alternatives[i].second)) {
      return false;
    }
  }
  return EqualPtr(left.else_expr, right.else_expr);
}

bool ASTEqual::EqualParenthesizedExpression(
    const ParenthesizedExpression &left, const ParenthesizedExpression &right) {
  return EqualPtr(left.expr, right.expr);
}

bool ASTEqual::EqualListComprehension(const ListComprehension &left,
                                      const ListComprehension &right) {
  return left.variable == right.variable &&
         EqualPtr(left.list_expr, right.list_expr) &&
         EqualPtr(left.where_expr, right.where_expr) &&
         EqualPtr(left.eval_expr, right.eval_expr);
}

bool ASTEqual::EqualPatternComprehension(const PatternComprehension &left,
                                         const PatternComprehension &right) {
  return left.variable == right.variable &&
         EqualPtr(left.relationships_pattern, right.relationships_pattern) &&
         EqualPtr(left.where_expr, right.where_expr) &&
         EqualPtr(left.eval_expr, right.eval_expr);
}

bool ASTEqual::EqualPatternPredicateExpression(
    const PatternPredicateExpression &left,
    const PatternPredicateExpression &right) {
  return EqualPtr(left.relationships_pattern, right.relationships_pattern);
}

bool ASTEqual::EqualQuantifier(const Quantifier &left,
                               const Quantifier &right) {
  return left.variable == right.variable &&
         EqualPtr(left.list_expr, right.list_expr) &&
         EqualPtr(left.predicate, right.predicate);
}

bool ASTEqual::EqualAllQuantifier(const AllQuantifier &left,
                                  const AllQuantifier &right) {
  return EqualQuantifier(left, right);
}

bool ASTEqual::EqualAnyQuantifier(const AnyQuantifier &left,
                                  const AnyQuantifier &right) {
  return EqualQuantifier(left, right);
}

bool ASTEqual::EqualNoneQuantifier(const NoneQuantifier &left,
                                   const NoneQuantifier &right) {
  return EqualQuantifier(left, right);
}

bool ASTEqual::EqualSingleQuantifier(const SingleQuantifier &left,
                                     const SingleQuantifier &right) {
  return EqualQuantifier(left, right);
}

bool ASTEqual::EqualExistentialSubquery(const ExistentialSubquery &left,
                                        const ExistentialSubquery &right) {
  return EqualPtr(left.query, right.query) &&
         EqualPtr(left.pattern, right.pattern) &&
         EqualPtr(left.where_expr, right.where_expr);
}

bool ASTEqual::EqualPattern(const Pattern &left, const Pattern &right) {
  return EqualList(left.parts, right.parts);
}

bool ASTEqual::EqualPatternPart(const PatternPart &left,
                                const PatternPart &right) {
  return left.variable == right.variable &&
         EqualPtr(left.element, right.element);
}

bool ASTEqual::EqualPatternElement(const PatternElement &left,
                                   const PatternElement &right) {
  if (!EqualPtr(left.node_pattern, right.node_pattern)) {
    return false;
  }
  if (left.chain.size() != right.chain.size()) {
    return false;
  }
  for (size_t i = 0; i < left.chain.size(); ++i) {
    if (!EqualPtr(left.chain[i].first, right.chain[i].first) ||
        !EqualPtr(left.chain[i].second, right.chain[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::EqualRelationshipsPattern(const RelationshipsPattern &left,
                                         const RelationshipsPattern &right) {
  if (!EqualPtr(left.node_pattern, right.node_pattern)) {
    return false;
  }
  if (left.chain.size() != right.chain.size()) {
    return false;
  }
  for (size_t i = 0; i < left.chain.size(); ++i) {
    if (!EqualPtr(left.chain[i].first, right.chain[i].first) ||
        !EqualPtr(left.chain[i].second, right.chain[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::EqualNodePattern(const NodePattern &left,
                                const NodePattern &right) {
  return left.variable == right.variable && left.labels == right.labels &&
         EqualPtr(left.properties, right.properties);
}

bool ASTEqual::EqualRelationshipPattern(const RelationshipPattern &left,
                                        const RelationshipPattern &right) {
  return left.left_arrow == right.left_arrow &&
         left.right_arrow == right.right_arrow &&
         EqualPtr(left.detail, right.detail);
}

bool ASTEqual::EqualRelationshipDetail(const RelationshipDetail &left,
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
  return EqualPtr(left.properties, right.properties);
}

bool ASTEqual::EqualClause(const Clause & /*unused*/,
                           const Clause & /*unused*/) {
  return true;
}

bool ASTEqual::EqualReadingClause(const ReadingClause & /*unused*/,
                                  const ReadingClause & /*unused*/) {
  return true;
}

bool ASTEqual::EqualMatch(const Match &left, const Match &right) {
  return left.optional_match == right.optional_match &&
         EqualPtr(left.pattern, right.pattern) &&
         EqualPtr(left.where, right.where);
}

bool ASTEqual::EqualUnwind(const Unwind &left, const Unwind &right) {
  return left.variable == right.variable &&
         EqualPtr(left.expression, right.expression);
}

bool ASTEqual::EqualInQueryCall(const InQueryCall &left,
                                const InQueryCall &right) {
  if (left.procedure_name != right.procedure_name) {
    return false;
  }
  if (!EqualList(left.arguments, right.arguments)) {
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
  return EqualPtr(left.yield_where, right.yield_where);
}

bool ASTEqual::EqualUpdatingClause(const UpdatingClause & /*unused*/,
                                   const UpdatingClause & /*unused*/) {
  return true;
}

bool ASTEqual::EqualCreate(const Create &left, const Create &right) {
  return EqualPtr(left.pattern, right.pattern);
}

bool ASTEqual::EqualMerge(const Merge &left, const Merge &right) {
  if (!EqualPtr(left.pattern_part, right.pattern_part)) {
    return false;
  }
  if (left.actions.size() != right.actions.size()) {
    return false;
  }
  for (size_t i = 0; i < left.actions.size(); ++i) {
    if (left.actions[i].first != right.actions[i].first) {
      return false;
    }
    if (!EqualPtr(left.actions[i].second, right.actions[i].second)) {
      return false;
    }
  }
  return true;
}

bool ASTEqual::EqualDelete(const Delete &left, const Delete &right) {
  return left.detach == right.detach &&
         EqualList(left.expressions, right.expressions);
}

bool ASTEqual::EqualSet(const Set &left, const Set &right) {
  return EqualList(left.items, right.items);
}

bool ASTEqual::EqualSetItem(const SetItem &left, const SetItem &right) {
  return left.type == right.type && left.plus_equal == right.plus_equal &&
         left.labels == right.labels && EqualPtr(left.target, right.target) &&
         EqualPtr(left.value, right.value);
}

bool ASTEqual::EqualRemove(const Remove &left, const Remove &right) {
  return EqualList(left.items, right.items);
}

bool ASTEqual::EqualRemoveItem(const RemoveItem &left,
                               const RemoveItem &right) {
  return left.type == right.type && left.labels == right.labels &&
         EqualPtr(left.target, right.target);
}

bool ASTEqual::EqualProjectionClause(const ProjectionClause &left,
                                     const ProjectionClause &right) {
  return EqualPtr(left.body, right.body);
}

bool ASTEqual::EqualProjectionBody(const ProjectionBody &left,
                                   const ProjectionBody &right) {
  return left.distinct == right.distinct && left.star == right.star &&
         EqualList(left.items, right.items) &&
         EqualList(left.order_by, right.order_by) &&
         EqualPtr(left.skip, right.skip) && EqualPtr(left.limit, right.limit);
}

bool ASTEqual::EqualProjectionItem(const ProjectionItem &left,
                                   const ProjectionItem &right) {
  return left.alias == right.alias &&
         EqualPtr(left.expression, right.expression);
}

bool ASTEqual::EqualSortItem(const SortItem &left, const SortItem &right) {
  return left.ascending == right.ascending &&
         EqualPtr(left.expression, right.expression);
}

bool ASTEqual::EqualWith(const With &left, const With &right) {
  return EqualPtr(left.body, right.body) && EqualPtr(left.where, right.where);
}

bool ASTEqual::EqualReturn(const Return &left, const Return &right) {
  return EqualPtr(left.body, right.body);
}

}  // namespace ast
