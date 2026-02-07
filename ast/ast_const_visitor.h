#pragma once

#include "ast_visitor.h"

namespace ast {

#define AST_CONST_VISITOR_NODE_LIST(V) \
  V(Statement)                         \
  V(Query)                             \
  V(RegularQuery)                      \
  V(StandaloneCall)                    \
  V(SingleQuery)                       \
  V(SinglePartQuery)                   \
  V(MultiPartQuery)                    \
  V(UnionPart)                         \
  V(Expression)                        \
  V(BinaryExpression)                  \
  V(OrExpression)                      \
  V(XorExpression)                     \
  V(AndExpression)                     \
  V(ComparisonExpression)              \
  V(ComparisonChainExpression)         \
  V(AddExpression)                     \
  V(SubtractExpression)                \
  V(MultiplyExpression)                \
  V(DivideExpression)                  \
  V(ModuloExpression)                  \
  V(PowerExpression)                   \
  V(UnaryExpression)                   \
  V(NotExpression)                     \
  V(UnaryPlusExpression)               \
  V(UnaryMinusExpression)              \
  V(StringPredicateExpression)         \
  V(ListPredicateExpression)           \
  V(LabelPredicateExpression)          \
  V(NullPredicateExpression)           \
  V(Literal)                           \
  V(BooleanLiteral)                    \
  V(IntegerLiteral)                    \
  V(DoubleLiteral)                     \
  V(StringLiteral)                     \
  V(NullLiteral)                       \
  V(ListLiteral)                       \
  V(MapLiteral)                        \
  V(Properties)                        \
  V(Variable)                          \
  V(Parameter)                         \
  V(PropertyExpression)                \
  V(ListIndexExpression)               \
  V(ListSliceExpression)               \
  V(FunctionInvocation)                \
  V(CountStarExpression)               \
  V(CaseExpression)                    \
  V(ParenthesizedExpression)           \
  V(ListComprehension)                 \
  V(PatternComprehension)              \
  V(PatternPredicateExpression)        \
  V(Quantifier)                        \
  V(AllQuantifier)                     \
  V(AnyQuantifier)                     \
  V(NoneQuantifier)                    \
  V(SingleQuantifier)                  \
  V(ExistentialSubquery)               \
  V(Pattern)                           \
  V(PatternPart)                       \
  V(PatternElement)                    \
  V(RelationshipsPattern)              \
  V(NodePattern)                       \
  V(RelationshipPattern)               \
  V(RelationshipDetail)                \
  V(Clause)                            \
  V(ReadingClause)                     \
  V(Match)                             \
  V(Unwind)                            \
  V(InQueryCall)                       \
  V(UpdatingClause)                    \
  V(Create)                            \
  V(Merge)                             \
  V(Delete)                            \
  V(Set)                               \
  V(SetItem)                           \
  V(Remove)                            \
  V(RemoveItem)                        \
  V(ProjectionClause)                  \
  V(ProjectionBody)                    \
  V(ProjectionItem)                    \
  V(SortItem)                          \
  V(With)                              \
  V(Return)

class ASTConstVisitor : public ASTVisitor {
 public:
  ~ASTConstVisitor() override = default;

#define DECLARE_CONST_VISIT(node_type) \
  virtual void Visit(const node_type &node) = 0;
  AST_CONST_VISITOR_NODE_LIST(DECLARE_CONST_VISIT)
#undef DECLARE_CONST_VISIT

#define DECLARE_BRIDGE_VISIT(node_type)          \
  void Visit(node_type &node) final {            \
    Visit(static_cast<const node_type &>(node)); \
  }
  AST_CONST_VISITOR_NODE_LIST(DECLARE_BRIDGE_VISIT)
#undef DECLARE_BRIDGE_VISIT
};

#undef AST_CONST_VISITOR_NODE_LIST

}  // namespace ast
