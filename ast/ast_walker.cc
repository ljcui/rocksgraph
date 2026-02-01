#include "ast_walker.h"

namespace ast {

void ASTWalker::walk(ASTNode &node) { node.accept(*this); }

void ASTWalker::walkExpression(std::unique_ptr<Expression> &expr) {
  if (expr) {
    expr->accept(*this);
  }
}

void ASTWalker::visit(Statement &node) { (void)node; }
void ASTWalker::visit(Query &node) { (void)node; }
void ASTWalker::visit(RegularQuery &node) {
  walkMaybe(node.single_query);
  walkList(node.unions);
}
void ASTWalker::visit(StandaloneCall &node) {
  walkList(node.arguments);
  walkMaybe(node.yield_where);
}
void ASTWalker::visit(SingleQuery &node) { (void)node; }
void ASTWalker::visit(SinglePartQuery &node) {
  walkList(node.reading_clauses);
  walkList(node.updating_clauses);
  walkMaybe(node.return_clause);
}
void ASTWalker::visit(MultiPartQuery &node) {
  for (auto &part : node.parts) {
    walkList(part.reading_clauses);
    walkList(part.updating_clauses);
    walkMaybe(part.with_clause);
  }
  walkMaybe(node.final_single_part_query);
}
void ASTWalker::visit(UnionPart &node) { walkMaybe(node.query); }

void ASTWalker::visit(Expression &node) { (void)node; }
void ASTWalker::visit(BinaryExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(OrExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(XorExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(AndExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(ComparisonExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(ComparisonChainExpression &node) {
  walkMaybe(node.left);
  for (auto &entry : node.rights) {
    walkMaybe(entry.second);
  }
}
void ASTWalker::visit(AddExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(SubtractExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(MultiplyExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(DivideExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(ModuloExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(PowerExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(UnaryExpression &node) { walkMaybe(node.operand); }
void ASTWalker::visit(NotExpression &node) { walkMaybe(node.operand); }
void ASTWalker::visit(UnaryPlusExpression &node) { walkMaybe(node.operand); }
void ASTWalker::visit(UnaryMinusExpression &node) { walkMaybe(node.operand); }
void ASTWalker::visit(StringPredicateExpression &node) {
  walkMaybe(node.left);
  walkMaybe(node.right);
}
void ASTWalker::visit(ListPredicateExpression &node) {
  walkMaybe(node.element);
  walkMaybe(node.list);
}
void ASTWalker::visit(LabelPredicateExpression &node) {
  walkMaybe(node.expr);
}
void ASTWalker::visit(NullPredicateExpression &node) {
  walkMaybe(node.operand);
}

void ASTWalker::visit(Literal &node) { (void)node; }
void ASTWalker::visit(BooleanLiteral &node) { (void)node; }
void ASTWalker::visit(IntegerLiteral &node) { (void)node; }
void ASTWalker::visit(DoubleLiteral &node) { (void)node; }
void ASTWalker::visit(StringLiteral &node) { (void)node; }
void ASTWalker::visit(NullLiteral &node) { (void)node; }
void ASTWalker::visit(ListLiteral &node) { walkList(node.elements); }
void ASTWalker::visit(MapLiteral &node) {
  for (auto &entry : node.entries) {
    walkMaybe(entry.second);
  }
}
void ASTWalker::visit(Properties &node) {
  walkMaybe(node.map);
  walkMaybe(node.parameter);
}

void ASTWalker::visit(Variable &node) { (void)node; }
void ASTWalker::visit(Parameter &node) { (void)node; }
void ASTWalker::visit(PropertyExpression &node) { walkMaybe(node.object); }
void ASTWalker::visit(ListIndexExpression &node) {
  walkMaybe(node.list);
  walkMaybe(node.index);
}
void ASTWalker::visit(ListSliceExpression &node) {
  walkMaybe(node.list);
  walkMaybe(node.start_index);
  walkMaybe(node.end_index);
}
void ASTWalker::visit(FunctionInvocation &node) { walkList(node.arguments); }
void ASTWalker::visit(CountStarExpression &node) { (void)node; }
void ASTWalker::visit(CaseExpression &node) {
  walkMaybe(node.test);
  for (auto &alt : node.alternatives) {
    walkMaybe(alt.first);
    walkMaybe(alt.second);
  }
  walkMaybe(node.else_expr);
}
void ASTWalker::visit(ParenthesizedExpression &node) {
  walkMaybe(node.expr);
}
void ASTWalker::visit(ListComprehension &node) {
  walkMaybe(node.list_expr);
  walkMaybe(node.where_expr);
  walkMaybe(node.eval_expr);
}
void ASTWalker::visit(PatternComprehension &node) {
  walkMaybe(node.relationships_pattern);
  walkMaybe(node.where_expr);
  walkMaybe(node.eval_expr);
}
void ASTWalker::visit(PatternPredicateExpression &node) {
  walkMaybe(node.relationships_pattern);
}
void ASTWalker::visit(Quantifier &node) {
  walkMaybe(node.list_expr);
  walkMaybe(node.predicate);
}
void ASTWalker::visit(AllQuantifier &node) {
  walkMaybe(node.list_expr);
  walkMaybe(node.predicate);
}
void ASTWalker::visit(AnyQuantifier &node) {
  walkMaybe(node.list_expr);
  walkMaybe(node.predicate);
}
void ASTWalker::visit(NoneQuantifier &node) {
  walkMaybe(node.list_expr);
  walkMaybe(node.predicate);
}
void ASTWalker::visit(SingleQuantifier &node) {
  walkMaybe(node.list_expr);
  walkMaybe(node.predicate);
}
void ASTWalker::visit(ExistentialSubquery &node) {
  walkMaybe(node.query);
  walkMaybe(node.pattern);
  walkMaybe(node.where_expr);
}

void ASTWalker::visit(Pattern &node) { walkList(node.parts); }
void ASTWalker::visit(PatternPart &node) { walkMaybe(node.element); }
void ASTWalker::visit(PatternElement &node) {
  walkMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    walkMaybe(link.first);
    walkMaybe(link.second);
  }
}
void ASTWalker::visit(RelationshipsPattern &node) {
  walkMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    walkMaybe(link.first);
    walkMaybe(link.second);
  }
}
void ASTWalker::visit(NodePattern &node) { walkMaybe(node.properties); }
void ASTWalker::visit(RelationshipPattern &node) {
  walkMaybe(node.detail);
}
void ASTWalker::visit(RelationshipDetail &node) { walkMaybe(node.properties); }

void ASTWalker::visit(Clause &node) { (void)node; }
void ASTWalker::visit(ReadingClause &node) { (void)node; }
void ASTWalker::visit(Match &node) {
  walkMaybe(node.pattern);
  walkMaybe(node.where);
}
void ASTWalker::visit(Unwind &node) { walkMaybe(node.expression); }
void ASTWalker::visit(InQueryCall &node) {
  walkList(node.arguments);
  walkMaybe(node.yield_where);
}
void ASTWalker::visit(UpdatingClause &node) { (void)node; }
void ASTWalker::visit(Create &node) { walkMaybe(node.pattern); }
void ASTWalker::visit(Merge &node) {
  walkMaybe(node.pattern_part);
  for (auto &action : node.actions) {
    walkMaybe(action.second);
  }
}
void ASTWalker::visit(Delete &node) { walkList(node.expressions); }
void ASTWalker::visit(Set &node) { walkList(node.items); }
void ASTWalker::visit(SetItem &node) {
  walkMaybe(node.target);
  walkMaybe(node.value);
}
void ASTWalker::visit(Remove &node) { walkList(node.items); }
void ASTWalker::visit(RemoveItem &node) { walkMaybe(node.target); }
void ASTWalker::visit(ProjectionClause &node) { walkMaybe(node.body); }
void ASTWalker::visit(ProjectionBody &node) {
  walkList(node.items);
  walkList(node.order_by);
  walkMaybe(node.skip);
  walkMaybe(node.limit);
}
void ASTWalker::visit(ProjectionItem &node) { walkMaybe(node.expression); }
void ASTWalker::visit(SortItem &node) { walkMaybe(node.expression); }
void ASTWalker::visit(With &node) {
  walkMaybe(node.body);
  walkMaybe(node.where);
}
void ASTWalker::visit(Return &node) { walkMaybe(node.body); }

}  // namespace ast
