#include "ast_walker.h"

namespace ast {

void ASTWalker::Walk(ASTNode &node) { node.Accept(*this); }

void ASTWalker::WalkExpression(std::unique_ptr<Expression> &expr) {
  if (expr) {
    expr->Accept(*this);
  }
}

void ASTWalker::Visit(Statement &node) { (void)node; }
void ASTWalker::Visit(Query &node) { (void)node; }
void ASTWalker::Visit(RegularQuery &node) {
  WalkMaybe(node.single_query);
  WalkList(node.unions);
}
void ASTWalker::Visit(StandaloneCall &node) {
  WalkList(node.arguments);
  WalkMaybe(node.yield_where);
}
void ASTWalker::Visit(SingleQuery &node) { (void)node; }
void ASTWalker::Visit(SinglePartQuery &node) {
  WalkList(node.reading_clauses);
  WalkList(node.updating_clauses);
  WalkMaybe(node.return_clause);
}
void ASTWalker::Visit(MultiPartQuery &node) {
  for (auto &part : node.parts) {
    WalkList(part.reading_clauses);
    WalkList(part.updating_clauses);
    WalkMaybe(part.with_clause);
  }
  WalkMaybe(node.final_single_part_query);
}
void ASTWalker::Visit(UnionPart &node) { WalkMaybe(node.query); }

void ASTWalker::Visit(Expression &node) { (void)node; }
void ASTWalker::Visit(BinaryExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(OrExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(XorExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(AndExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(ComparisonExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(ComparisonChainExpression &node) {
  WalkMaybe(node.left);
  for (auto &entry : node.rights) {
    WalkMaybe(entry.second);
  }
}
void ASTWalker::Visit(AddExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(SubtractExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(MultiplyExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(DivideExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(ModuloExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(PowerExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(UnaryExpression &node) { WalkMaybe(node.operand); }
void ASTWalker::Visit(NotExpression &node) { WalkMaybe(node.operand); }
void ASTWalker::Visit(UnaryPlusExpression &node) { WalkMaybe(node.operand); }
void ASTWalker::Visit(UnaryMinusExpression &node) { WalkMaybe(node.operand); }
void ASTWalker::Visit(StringPredicateExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTWalker::Visit(ListPredicateExpression &node) {
  WalkMaybe(node.element);
  WalkMaybe(node.list);
}
void ASTWalker::Visit(LabelPredicateExpression &node) { WalkMaybe(node.expr); }
void ASTWalker::Visit(NullPredicateExpression &node) {
  WalkMaybe(node.operand);
}

void ASTWalker::Visit(Literal &node) { (void)node; }
void ASTWalker::Visit(BooleanLiteral &node) { (void)node; }
void ASTWalker::Visit(IntegerLiteral &node) { (void)node; }
void ASTWalker::Visit(DoubleLiteral &node) { (void)node; }
void ASTWalker::Visit(StringLiteral &node) { (void)node; }
void ASTWalker::Visit(NullLiteral &node) { (void)node; }
void ASTWalker::Visit(ListLiteral &node) { WalkList(node.elements); }
void ASTWalker::Visit(MapLiteral &node) {
  for (auto &entry : node.entries) {
    WalkMaybe(entry.second);
  }
}
void ASTWalker::Visit(Properties &node) {
  WalkMaybe(node.map);
  WalkMaybe(node.parameter);
}

void ASTWalker::Visit(Variable &node) { (void)node; }
void ASTWalker::Visit(Parameter &node) { (void)node; }
void ASTWalker::Visit(PropertyExpression &node) { WalkMaybe(node.object); }
void ASTWalker::Visit(ListIndexExpression &node) {
  WalkMaybe(node.list);
  WalkMaybe(node.index);
}
void ASTWalker::Visit(ListSliceExpression &node) {
  WalkMaybe(node.list);
  WalkMaybe(node.start_index);
  WalkMaybe(node.end_index);
}
void ASTWalker::Visit(FunctionInvocation &node) { WalkList(node.arguments); }
void ASTWalker::Visit(CountStarExpression &node) { (void)node; }
void ASTWalker::Visit(CaseExpression &node) {
  WalkMaybe(node.test);
  for (auto &alt : node.alternatives) {
    WalkMaybe(alt.first);
    WalkMaybe(alt.second);
  }
  WalkMaybe(node.else_expr);
}
void ASTWalker::Visit(ParenthesizedExpression &node) { WalkMaybe(node.expr); }
void ASTWalker::Visit(ListComprehension &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.where_expr);
  WalkMaybe(node.eval_expr);
}
void ASTWalker::Visit(PatternComprehension &node) {
  WalkMaybe(node.relationships_pattern);
  WalkMaybe(node.where_expr);
  WalkMaybe(node.eval_expr);
}
void ASTWalker::Visit(PatternPredicateExpression &node) {
  WalkMaybe(node.relationships_pattern);
}
void ASTWalker::Visit(Quantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTWalker::Visit(AllQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTWalker::Visit(AnyQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTWalker::Visit(NoneQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTWalker::Visit(SingleQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTWalker::Visit(ExistentialSubquery &node) {
  WalkMaybe(node.query);
  WalkMaybe(node.pattern);
  WalkMaybe(node.where_expr);
}

void ASTWalker::Visit(Pattern &node) { WalkList(node.parts); }
void ASTWalker::Visit(PatternPart &node) { WalkMaybe(node.element); }
void ASTWalker::Visit(PatternElement &node) {
  WalkMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    WalkMaybe(link.first);
    WalkMaybe(link.second);
  }
}
void ASTWalker::Visit(RelationshipsPattern &node) {
  WalkMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    WalkMaybe(link.first);
    WalkMaybe(link.second);
  }
}
void ASTWalker::Visit(NodePattern &node) { WalkMaybe(node.properties); }
void ASTWalker::Visit(RelationshipPattern &node) { WalkMaybe(node.detail); }
void ASTWalker::Visit(RelationshipDetail &node) { WalkMaybe(node.properties); }

void ASTWalker::Visit(Clause &node) { (void)node; }
void ASTWalker::Visit(ReadingClause &node) { (void)node; }
void ASTWalker::Visit(Match &node) {
  WalkMaybe(node.pattern);
  WalkMaybe(node.where);
}
void ASTWalker::Visit(Unwind &node) { WalkMaybe(node.expression); }
void ASTWalker::Visit(InQueryCall &node) {
  WalkList(node.arguments);
  WalkMaybe(node.yield_where);
}
void ASTWalker::Visit(UpdatingClause &node) { (void)node; }
void ASTWalker::Visit(Create &node) { WalkMaybe(node.pattern); }
void ASTWalker::Visit(Merge &node) {
  WalkMaybe(node.pattern_part);
  for (auto &action : node.actions) {
    WalkMaybe(action.second);
  }
}
void ASTWalker::Visit(Delete &node) { WalkList(node.expressions); }
void ASTWalker::Visit(Set &node) { WalkList(node.items); }
void ASTWalker::Visit(SetItem &node) {
  WalkMaybe(node.target);
  WalkMaybe(node.value);
}
void ASTWalker::Visit(Remove &node) { WalkList(node.items); }
void ASTWalker::Visit(RemoveItem &node) { WalkMaybe(node.target); }
void ASTWalker::Visit(ProjectionClause &node) { WalkMaybe(node.body); }
void ASTWalker::Visit(ProjectionBody &node) {
  WalkList(node.items);
  WalkList(node.order_by);
  WalkMaybe(node.skip);
  WalkMaybe(node.limit);
}
void ASTWalker::Visit(ProjectionItem &node) { WalkMaybe(node.expression); }
void ASTWalker::Visit(SortItem &node) { WalkMaybe(node.expression); }
void ASTWalker::Visit(With &node) {
  WalkMaybe(node.body);
  WalkMaybe(node.where);
}
void ASTWalker::Visit(Return &node) { WalkMaybe(node.body); }

}  // namespace ast
