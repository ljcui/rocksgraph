#ifndef AST_WALKER_CLASS
#error "AST_WALKER_CLASS must be defined before including ast_walker_visit_impl.inc"
#endif

#ifndef AST_WALKER_PARAM
#error "AST_WALKER_PARAM must be defined before including ast_walker_visit_impl.inc"
#endif

void AST_WALKER_CLASS::WalkExpression(
    AST_WALKER_PARAM(std::unique_ptr<Expression>) expr) {
  if (expr) {
    AST_WALKER_PARAM(ASTNode) node = *expr;
    node.Accept(*this);
  }
}

void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Statement) node) { (void)node; }
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Query) node) { (void)node; }
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(RegularQuery) node) {
  WalkMaybe(node.single_query);
  WalkList(node.unions);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(StandaloneCall) node) {
  WalkList(node.arguments);
  WalkMaybe(node.yield_where);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(SingleQuery) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(SinglePartQuery) node) {
  WalkList(node.reading_clauses);
  WalkList(node.updating_clauses);
  WalkMaybe(node.return_clause);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(MultiPartQuery) node) {
  for (auto &part : node.parts) {
    WalkList(part.reading_clauses);
    WalkList(part.updating_clauses);
    WalkMaybe(part.with_clause);
  }
  WalkMaybe(node.final_single_part_query);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(UnionPart) node) {
  WalkMaybe(node.query);
}

void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Expression) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(BinaryExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(OrExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(XorExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(AndExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ComparisonExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(
    AST_WALKER_PARAM(ComparisonChainExpression) node) {
  WalkMaybe(node.left);
  for (auto &entry : node.rights) {
    WalkMaybe(entry.second);
  }
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(AddExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(SubtractExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(MultiplyExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(DivideExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ModuloExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(PowerExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(UnaryExpression) node) {
  WalkMaybe(node.operand);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(NotExpression) node) {
  WalkMaybe(node.operand);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(UnaryPlusExpression) node) {
  WalkMaybe(node.operand);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(UnaryMinusExpression) node) {
  WalkMaybe(node.operand);
}
void AST_WALKER_CLASS::Visit(
    AST_WALKER_PARAM(StringPredicateExpression) node) {
  WalkMaybe(node.left);
  WalkMaybe(node.right);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ListPredicateExpression) node) {
  WalkMaybe(node.element);
  WalkMaybe(node.list);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(LabelPredicateExpression) node) {
  WalkMaybe(node.expr);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(NullPredicateExpression) node) {
  WalkMaybe(node.operand);
}

void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Literal) node) { (void)node; }
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(BooleanLiteral) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(IntegerLiteral) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(DoubleLiteral) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(StringLiteral) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(NullLiteral) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ListLiteral) node) {
  WalkList(node.elements);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(MapLiteral) node) {
  for (auto &entry : node.entries) {
    WalkMaybe(entry.second);
  }
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Properties) node) {
  WalkMaybe(node.map);
  WalkMaybe(node.parameter);
}

void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Variable) node) { (void)node; }
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Parameter) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(PropertyExpression) node) {
  WalkMaybe(node.object);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ListIndexExpression) node) {
  WalkMaybe(node.list);
  WalkMaybe(node.index);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ListSliceExpression) node) {
  WalkMaybe(node.list);
  WalkMaybe(node.start_index);
  WalkMaybe(node.end_index);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(FunctionInvocation) node) {
  WalkList(node.arguments);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(CountStarExpression) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(CaseExpression) node) {
  WalkMaybe(node.test);
  for (auto &alt : node.alternatives) {
    WalkMaybe(alt.first);
    WalkMaybe(alt.second);
  }
  WalkMaybe(node.else_expr);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ParenthesizedExpression) node) {
  WalkMaybe(node.expr);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ListComprehension) node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.where_expr);
  WalkMaybe(node.eval_expr);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(PatternComprehension) node) {
  WalkMaybe(node.relationships_pattern);
  WalkMaybe(node.where_expr);
  WalkMaybe(node.eval_expr);
}
void AST_WALKER_CLASS::Visit(
    AST_WALKER_PARAM(PatternPredicateExpression) node) {
  WalkMaybe(node.relationships_pattern);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Quantifier) node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(AllQuantifier) node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(AnyQuantifier) node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(NoneQuantifier) node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(SingleQuantifier) node) {
  WalkMaybe(node.list_expr);
  WalkMaybe(node.predicate);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ExistentialSubquery) node) {
  WalkMaybe(node.query);
  WalkMaybe(node.pattern);
  WalkMaybe(node.where_expr);
}

void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Pattern) node) {
  WalkList(node.parts);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(PatternPart) node) {
  WalkMaybe(node.element);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(PatternElement) node) {
  WalkMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    WalkMaybe(link.first);
    WalkMaybe(link.second);
  }
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(RelationshipsPattern) node) {
  WalkMaybe(node.node_pattern);
  for (auto &link : node.chain) {
    WalkMaybe(link.first);
    WalkMaybe(link.second);
  }
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(NodePattern) node) {
  WalkMaybe(node.properties);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(RelationshipPattern) node) {
  WalkMaybe(node.detail);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(RelationshipDetail) node) {
  WalkMaybe(node.properties);
}

void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Clause) node) { (void)node; }
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ReadingClause) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Match) node) {
  WalkMaybe(node.pattern);
  WalkMaybe(node.where);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Unwind) node) {
  WalkMaybe(node.expression);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(InQueryCall) node) {
  WalkList(node.arguments);
  WalkMaybe(node.yield_where);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(UpdatingClause) node) {
  (void)node;
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Create) node) {
  WalkMaybe(node.pattern);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Merge) node) {
  WalkMaybe(node.pattern_part);
  for (auto &action : node.actions) {
    WalkMaybe(action.second);
  }
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Delete) node) {
  WalkList(node.expressions);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Set) node) {
  WalkList(node.items);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(SetItem) node) {
  WalkMaybe(node.target);
  WalkMaybe(node.value);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Remove) node) {
  WalkList(node.items);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(RemoveItem) node) {
  WalkMaybe(node.target);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ProjectionClause) node) {
  WalkMaybe(node.body);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ProjectionBody) node) {
  WalkList(node.items);
  WalkList(node.order_by);
  WalkMaybe(node.skip);
  WalkMaybe(node.limit);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(ProjectionItem) node) {
  WalkMaybe(node.expression);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(SortItem) node) {
  WalkMaybe(node.expression);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(With) node) {
  WalkMaybe(node.body);
  WalkMaybe(node.where);
}
void AST_WALKER_CLASS::Visit(AST_WALKER_PARAM(Return) node) {
  WalkMaybe(node.body);
}
