#include "ast_const_walker.h"

namespace ast {

void ASTConstWalker::Walk(const ASTNode &node) { node.Accept(*this); }

void ASTConstWalker::WalkExpression(const std::unique_ptr<Expression> &expr) {
  if (expr) {
    const ASTNode &node = *expr;
    node.Accept(*this);
  }
}

void ASTConstWalker::Visit(const Statement &node) { (void)node; }
void ASTConstWalker::Visit(const Query &node) { (void)node; }
void ASTConstWalker::Visit(const RegularQuery &node) {
  WalkMaybe(node.single_query);
  WalkList(node.unions);
}
void ASTConstWalker::Visit(const StandaloneCall &node) {
  WalkList(node.arguments);
  WalkMaybe(node.yield_where);
}
void ASTConstWalker::Visit(const SingleQuery &node) { (void)node; }
void ASTConstWalker::Visit(const SinglePartQuery &node) {
  WalkList(node.reading_clauses);
  WalkList(node.updating_clauses);
  WalkMaybe(node.return_clause);
}
void ASTConstWalker::Visit(const MultiPartQuery &node) {
  for (const auto &part : node.parts) {
    WalkList(part.reading_clauses);
    WalkList(part.updating_clauses);
    WalkMaybe(part.with_clause);
  }
  WalkMaybe(node.final_single_part_query);
}
void ASTConstWalker::Visit(const UnionPart &node) { WalkMaybe(node.query); }

void ASTConstWalker::Visit(const Expression &node) { (void)node; }
void ASTConstWalker::Visit(const BinaryExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const OrExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const XorExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const AndExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const ComparisonExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const ComparisonChainExpression &node) {
  WalkMaybe(node.left);
  for (const auto &entry : node.rights) {
    WalkMaybe(entry.second);
  }
}
void ASTConstWalker::Visit(const AddExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const SubtractExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const MultiplyExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const DivideExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const ModuloExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const PowerExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const UnaryExpression &node) {
  WalkMaybe(node.operand);
}
void ASTConstWalker::Visit(const NotExpression &node) {
  WalkMaybe(node.operand);
}
void ASTConstWalker::Visit(const UnaryPlusExpression &node) {
  WalkMaybe(node.operand);
}
void ASTConstWalker::Visit(const UnaryMinusExpression &node) {
  WalkMaybe(node.operand);
}
void ASTConstWalker::Visit(const StringPredicateExpression &node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void ASTConstWalker::Visit(const ListPredicateExpression &node) {
  WalkMaybe(node.element);
  WalkMaybe(node.list);
}
void ASTConstWalker::Visit(const LabelPredicateExpression &node) {
  WalkMaybe(node.expr);
}
void ASTConstWalker::Visit(const NullPredicateExpression &node) {
  WalkMaybe(node.operand);
}

void ASTConstWalker::Visit(const Literal &node) { (void)node; }
void ASTConstWalker::Visit(const BooleanLiteral &node) { (void)node; }
void ASTConstWalker::Visit(const IntegerLiteral &node) { (void)node; }
void ASTConstWalker::Visit(const DoubleLiteral &node) { (void)node; }
void ASTConstWalker::Visit(const StringLiteral &node) { (void)node; }
void ASTConstWalker::Visit(const NullLiteral &node) { (void)node; }
void ASTConstWalker::Visit(const ListLiteral &node) { WalkList(node.elements); }
void ASTConstWalker::Visit(const MapLiteral &node) {
  for (const auto &entry : node.entries) {
    WalkMaybe(entry.second);
  }
}
void ASTConstWalker::Visit(const Properties &node) {
  WalkMaybe(node.map);
  WalkMaybe(node.parameter);
}

void ASTConstWalker::Visit(const Variable &node) { (void)node; }
void ASTConstWalker::Visit(const Parameter &node) { (void)node; }
void ASTConstWalker::Visit(const PropertyExpression &node) {
  WalkMaybe(node.object);
}
void ASTConstWalker::Visit(const ListIndexExpression &node) {
  WalkMaybe(node.list);
  WalkMaybe(node.index);
}
void ASTConstWalker::Visit(const ListSliceExpression &node) {
  WalkMaybe(node.list);
  WalkMaybe(node.start_index);
  WalkMaybe(node.end_index);
}
void ASTConstWalker::Visit(const FunctionInvocation &node) {
  WalkList(node.arguments);
}
void ASTConstWalker::Visit(const CountStarExpression &node) { (void)node; }
void ASTConstWalker::Visit(const CaseExpression &node) {
  WalkMaybe(node.test);
  for (const auto &alt : node.alternatives) {
    WalkMaybe(alt.first);
    WalkMaybe(alt.second);
  }
  WalkMaybe(node.else_expr);
}
void ASTConstWalker::Visit(const ParenthesizedExpression &node) {
  WalkMaybe(node.expr);
}
void ASTConstWalker::Visit(const ListComprehension &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.where_expr);
  WalkMaybe(node.eval_expr);
}
void ASTConstWalker::Visit(const PatternComprehension &node) {
  WalkMaybe(node.relationships_pattern);
  WalkMaybe(node.where_expr);
  WalkMaybe(node.eval_expr);
}
void ASTConstWalker::Visit(const PatternPredicateExpression &node) {
  WalkMaybe(node.relationships_pattern);
}
void ASTConstWalker::Visit(const Quantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTConstWalker::Visit(const AllQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTConstWalker::Visit(const AnyQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTConstWalker::Visit(const NoneQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTConstWalker::Visit(const SingleQuantifier &node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void ASTConstWalker::Visit(const ExistentialSubquery &node) {
  WalkMaybe(node.query);
  WalkMaybe(node.pattern);
  WalkMaybe(node.where_expr);
}

void ASTConstWalker::Visit(const Pattern &node) { WalkList(node.parts); }
void ASTConstWalker::Visit(const PatternPart &node) { WalkMaybe(node.element); }
void ASTConstWalker::Visit(const PatternElement &node) {
  WalkMaybe(node.node_pattern);
  for (const auto &link : node.chain) {
    WalkMaybe(link.first);
    WalkMaybe(link.second);
  }
}
void ASTConstWalker::Visit(const RelationshipsPattern &node) {
  WalkMaybe(node.node_pattern);
  for (const auto &link : node.chain) {
    WalkMaybe(link.first);
    WalkMaybe(link.second);
  }
}
void ASTConstWalker::Visit(const NodePattern &node) {
  WalkMaybe(node.properties);
}
void ASTConstWalker::Visit(const RelationshipPattern &node) {
  WalkMaybe(node.detail);
}
void ASTConstWalker::Visit(const RelationshipDetail &node) {
  WalkMaybe(node.properties);
}

void ASTConstWalker::Visit(const Clause &node) { (void)node; }
void ASTConstWalker::Visit(const ReadingClause &node) { (void)node; }
void ASTConstWalker::Visit(const Match &node) {
  WalkMaybe(node.pattern);
  WalkMaybe(node.where);
}
void ASTConstWalker::Visit(const Unwind &node) { WalkMaybe(node.expression); }
void ASTConstWalker::Visit(const InQueryCall &node) {
  WalkList(node.arguments);
  WalkMaybe(node.yield_where);
}
void ASTConstWalker::Visit(const UpdatingClause &node) { (void)node; }
void ASTConstWalker::Visit(const Create &node) { WalkMaybe(node.pattern); }
void ASTConstWalker::Visit(const Merge &node) {
  WalkMaybe(node.pattern_part);
  for (const auto &action : node.actions) {
    WalkMaybe(action.second);
  }
}
void ASTConstWalker::Visit(const Delete &node) { WalkList(node.expressions); }
void ASTConstWalker::Visit(const Set &node) { WalkList(node.items); }
void ASTConstWalker::Visit(const SetItem &node) {
  WalkMaybe(node.target);
  WalkMaybe(node.value);
}
void ASTConstWalker::Visit(const Remove &node) { WalkList(node.items); }
void ASTConstWalker::Visit(const RemoveItem &node) { WalkMaybe(node.target); }
void ASTConstWalker::Visit(const ProjectionClause &node) {
  WalkMaybe(node.body);
}
void ASTConstWalker::Visit(const ProjectionBody &node) {
  WalkList(node.items);
  WalkList(node.order_by);
  WalkMaybe(node.skip);
  WalkMaybe(node.limit);
}
void ASTConstWalker::Visit(const ProjectionItem &node) {
  WalkMaybe(node.expression);
}
void ASTConstWalker::Visit(const SortItem &node) { WalkMaybe(node.expression); }
void ASTConstWalker::Visit(const With &node) {
  WalkMaybe(node.body);
  WalkMaybe(node.where);
}
void ASTConstWalker::Visit(const Return &node) { WalkMaybe(node.body); }

}  // namespace ast
