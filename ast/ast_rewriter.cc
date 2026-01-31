#include "ast_rewriter.h"

#include <utility>

namespace ast {

void ASTRewriter::rewrite(ASTNode &node) { node.accept(*this); }

void ASTRewriter::visit(Statement &node) { (void)node; }
void ASTRewriter::visit(Query &node) { (void)node; }
void ASTRewriter::visit(RegularQuery &node) {
  rewriteMaybe(node.single_query);
  rewriteList(node.unions);
}
void ASTRewriter::visit(StandaloneCall &node) {
  rewriteList(node.arguments);
  rewriteMaybe(node.yield_where);
}
void ASTRewriter::visit(SingleQuery &node) { (void)node; }
void ASTRewriter::visit(SinglePartQuery &node) {
  rewriteList(node.reading_clauses);
  rewriteList(node.updating_clauses);
  rewriteMaybe(node.return_clause);
}
void ASTRewriter::visit(MultiPartQuery &node) {
  for (auto &part : node.parts) {
    rewriteList(part.reading_clauses);
    rewriteList(part.updating_clauses);
    rewriteMaybe(part.with_clause);
  }
  rewriteMaybe(node.final_single_part_query);
}
void ASTRewriter::visit(UnionPart &node) { rewriteMaybe(node.query); }

void ASTRewriter::visit(Expression &node) { (void)node; }
void ASTRewriter::visit(BinaryExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(OrExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(XorExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(AndExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ComparisonExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ComparisonChainExpression &node) {
  rewriteMaybe(node.left);
  for (auto &entry : node.rights) {
    rewriteMaybe(entry.second);
  }
}
void ASTRewriter::visit(AddExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(SubtractExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(MultiplyExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(DivideExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ModuloExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(PowerExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(UnaryExpression &node) { rewriteMaybe(node.operand); }
void ASTRewriter::visit(NotExpression &node) { rewriteMaybe(node.operand); }
void ASTRewriter::visit(UnaryPlusExpression &node) {
  rewriteMaybe(node.operand);
}
void ASTRewriter::visit(UnaryMinusExpression &node) {
  rewriteMaybe(node.operand);
}
void ASTRewriter::visit(StringPredicateExpression &node) {
  rewriteMaybe(node.left);
  rewriteMaybe(node.right);
}
void ASTRewriter::visit(ListPredicateExpression &node) {
  rewriteMaybe(node.element);
  rewriteMaybe(node.list);
}
void ASTRewriter::visit(LabelPredicateExpression &node) {
  rewriteMaybe(node.expr);
}
void ASTRewriter::visit(NullPredicateExpression &node) {
  rewriteMaybe(node.operand);
}

void ASTRewriter::visit(Literal &node) { (void)node; }
void ASTRewriter::visit(BooleanLiteral &node) { (void)node; }
void ASTRewriter::visit(IntegerLiteral &node) { (void)node; }
void ASTRewriter::visit(DoubleLiteral &node) { (void)node; }
void ASTRewriter::visit(StringLiteral &node) { (void)node; }
void ASTRewriter::visit(NullLiteral &node) { (void)node; }
void ASTRewriter::visit(ListLiteral &node) { rewriteList(node.elements); }
void ASTRewriter::visit(MapLiteral &node) {
  for (auto &entry : node.entries) {
    rewriteMaybe(entry.second);
  }
}
void ASTRewriter::visit(Properties &node) {
  rewriteMaybe(node.map);
  rewriteMaybe(node.parameter);
}

void ASTRewriter::visit(Variable &node) { (void)node; }
void ASTRewriter::visit(Parameter &node) { (void)node; }
void ASTRewriter::visit(PropertyExpression &node) {
  rewriteMaybe(node.object);
}
void ASTRewriter::visit(ListIndexExpression &node) {
  rewriteMaybe(node.list);
  rewriteMaybe(node.index);
}
void ASTRewriter::visit(ListSliceExpression &node) {
  rewriteMaybe(node.list);
  rewriteMaybe(node.start_index);
  rewriteMaybe(node.end_index);
}
void ASTRewriter::visit(FunctionInvocation &node) {
  rewriteList(node.arguments);
}
void ASTRewriter::visit(CountStarExpression &node) { (void)node; }
void ASTRewriter::visit(CaseExpression &node) {
  rewriteMaybe(node.test);
  for (auto &alt : node.alternatives) {
    rewriteMaybe(alt.first);
    rewriteMaybe(alt.second);
  }
  rewriteMaybe(node.else_expr);
}
void ASTRewriter::visit(ParenthesizedExpression &node) {
  rewriteMaybe(node.expr);
}
void ASTRewriter::visit(ListComprehension &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.where_expr);
  rewriteMaybe(node.eval_expr);
}
void ASTRewriter::visit(PatternComprehension &node) {
  rewriteMaybe(node.relationships_pattern);
  rewriteMaybe(node.where_expr);
  rewriteMaybe(node.eval_expr);
}
void ASTRewriter::visit(PatternPredicateExpression &node) {
  rewriteMaybe(node.relationships_pattern);
}
void ASTRewriter::visit(Quantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(AllQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(AnyQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(NoneQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(SingleQuantifier &node) {
  rewriteMaybe(node.list_expr);
  rewriteMaybe(node.predicate);
}
void ASTRewriter::visit(ExistentialSubquery &node) {
  rewriteMaybe(node.query);
  rewriteMaybe(node.pattern);
  rewriteMaybe(node.where_expr);
}

void ASTRewriter::visit(Pattern &node) { rewriteList(node.parts); }
void ASTRewriter::visit(PatternPart &node) { rewriteMaybe(node.element); }
void ASTRewriter::visit(PatternElement &node) {
  rewriteMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    rewriteMaybe(link.first);
    rewriteMaybe(link.second);
  }
}
void ASTRewriter::visit(RelationshipsPattern &node) {
  rewriteMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    rewriteMaybe(link.first);
    rewriteMaybe(link.second);
  }
}
void ASTRewriter::visit(NodePattern &node) { rewriteMaybe(node.properties); }
void ASTRewriter::visit(RelationshipPattern &node) {
  rewriteMaybe(node.detail);
}
void ASTRewriter::visit(RelationshipDetail &node) {
  rewriteMaybe(node.properties);
}

void ASTRewriter::visit(Clause &node) { (void)node; }
void ASTRewriter::visit(ReadingClause &node) { (void)node; }
void ASTRewriter::visit(Match &node) {
  rewriteMaybe(node.pattern);
  rewriteMaybe(node.where);
}
void ASTRewriter::visit(Unwind &node) { rewriteMaybe(node.expression); }
void ASTRewriter::visit(InQueryCall &node) {
  rewriteList(node.arguments);
  rewriteMaybe(node.yield_where);
}
void ASTRewriter::visit(UpdatingClause &node) { (void)node; }
void ASTRewriter::visit(Create &node) { rewriteMaybe(node.pattern); }
void ASTRewriter::visit(Merge &node) {
  rewriteMaybe(node.pattern_part);
  for (auto &action : node.actions) {
    rewriteMaybe(action.second);
  }
}
void ASTRewriter::visit(Delete &node) { rewriteList(node.expressions); }
void ASTRewriter::visit(Set &node) { rewriteList(node.items); }
void ASTRewriter::visit(SetItem &node) {
  rewriteMaybe(node.target);
  rewriteMaybe(node.value);
}
void ASTRewriter::visit(Remove &node) { rewriteList(node.items); }
void ASTRewriter::visit(RemoveItem &node) { rewriteMaybe(node.target); }
void ASTRewriter::visit(ProjectionClause &node) { rewriteMaybe(node.body); }
void ASTRewriter::visit(ProjectionBody &node) {
  rewriteList(node.items);
  rewriteList(node.order_by);
  rewriteMaybe(node.skip);
  rewriteMaybe(node.limit);
}
void ASTRewriter::visit(ProjectionItem &node) { rewriteMaybe(node.expression); }
void ASTRewriter::visit(SortItem &node) { rewriteMaybe(node.expression); }
void ASTRewriter::visit(With &node) {
  rewriteMaybe(node.body);
  rewriteMaybe(node.where);
}
void ASTRewriter::visit(Return &node) { rewriteMaybe(node.body); }

}  // namespace ast
