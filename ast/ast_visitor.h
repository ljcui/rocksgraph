#pragma once

namespace ast {

class Statement;
class Query;
class RegularQuery;
class StandaloneCall;
class SingleQuery;
class SinglePartQuery;
class MultiPartQuery;
class UnionPart;
class Expression;
class BinaryExpression;
class OrExpression;
class XorExpression;
class AndExpression;
class ComparisonExpression;
class ComparisonChainExpression;
class AddExpression;
class SubtractExpression;
class MultiplyExpression;
class DivideExpression;
class ModuloExpression;
class PowerExpression;
class UnaryExpression;
class NotExpression;
class UnaryPlusExpression;
class UnaryMinusExpression;
class StringPredicateExpression;
class ListPredicateExpression;
class LabelPredicateExpression;
class NullPredicateExpression;
class Literal;
class BooleanLiteral;
class IntegerLiteral;
class DoubleLiteral;
class StringLiteral;
class NullLiteral;
class ListLiteral;
class MapLiteral;
class Properties;
class Variable;
class Parameter;
class PropertyExpression;
class ListIndexExpression;
class ListSliceExpression;
class FunctionInvocation;
class CountStarExpression;
class CaseExpression;
class ParenthesizedExpression;
class ListComprehension;
class PatternComprehension;
class PatternPredicateExpression;
class Quantifier;
class AllQuantifier;
class AnyQuantifier;
class NoneQuantifier;
class SingleQuantifier;
class ExistentialSubquery;
class Pattern;
class PatternPart;
class PatternElement;
class RelationshipsPattern;
class NodePattern;
class RelationshipPattern;
class RelationshipDetail;
class Clause;
class ReadingClause;
class Match;
class Unwind;
class InQueryCall;
class UpdatingClause;
class Create;
class Merge;
class Delete;
class Set;
class SetItem;
class Remove;
class RemoveItem;
class ProjectionClause;
class ProjectionBody;
class ProjectionItem;
class SortItem;
class With;
class Return;

class ASTVisitor {
 public:
  virtual ~ASTVisitor() = default;

  // Top-level statements and queries
  virtual void visit(Statement& node) {}
  virtual void visit(Query& node) {}
  virtual void visit(RegularQuery& node) {}
  virtual void visit(StandaloneCall& node) {}
  virtual void visit(SingleQuery& node) {}
  virtual void visit(SinglePartQuery& node) {}
  virtual void visit(MultiPartQuery& node) {}
  virtual void visit(UnionPart& node) {}

  // Expressions
  virtual void visit(Expression& node) {}
  virtual void visit(BinaryExpression& node) {}
  virtual void visit(OrExpression& node) {}
  virtual void visit(XorExpression& node) {}
  virtual void visit(AndExpression& node) {}
  virtual void visit(ComparisonExpression& node) {}
  virtual void visit(ComparisonChainExpression& node) {}
  virtual void visit(AddExpression& node) {}
  virtual void visit(SubtractExpression& node) {}
  virtual void visit(MultiplyExpression& node) {}
  virtual void visit(DivideExpression& node) {}
  virtual void visit(ModuloExpression& node) {}
  virtual void visit(PowerExpression& node) {}
  virtual void visit(UnaryExpression& node) {}
  virtual void visit(NotExpression& node) {}
  virtual void visit(UnaryPlusExpression& node) {}
  virtual void visit(UnaryMinusExpression& node) {}
  virtual void visit(StringPredicateExpression& node) {}
  virtual void visit(ListPredicateExpression& node) {}
  virtual void visit(LabelPredicateExpression& node) {}
  virtual void visit(NullPredicateExpression& node) {}

  // Literals
  virtual void visit(Literal& node) {}
  virtual void visit(BooleanLiteral& node) {}
  virtual void visit(IntegerLiteral& node) {}
  virtual void visit(DoubleLiteral& node) {}
  virtual void visit(StringLiteral& node) {}
  virtual void visit(NullLiteral& node) {}
  virtual void visit(ListLiteral& node) {}
  virtual void visit(MapLiteral& node) {}
  virtual void visit(Properties& node) {}

  // Other expressions
  virtual void visit(Variable& node) {}
  virtual void visit(Parameter& node) {}
  virtual void visit(PropertyExpression& node) {}
  virtual void visit(ListIndexExpression& node) {}
  virtual void visit(ListSliceExpression& node) {}
  virtual void visit(FunctionInvocation& node) {}
  virtual void visit(CountStarExpression& node) {}
  virtual void visit(CaseExpression& node) {}
  virtual void visit(ParenthesizedExpression& node) {}
  virtual void visit(ListComprehension& node) {}
  virtual void visit(PatternComprehension& node) {}
  virtual void visit(PatternPredicateExpression& node) {}
  virtual void visit(Quantifier& node) {}
  virtual void visit(AllQuantifier& node) {}
  virtual void visit(AnyQuantifier& node) {}
  virtual void visit(NoneQuantifier& node) {}
  virtual void visit(SingleQuantifier& node) {}
  virtual void visit(ExistentialSubquery& node) {}

  // Pattern
  virtual void visit(Pattern& node) {}
  virtual void visit(PatternPart& node) {}
  virtual void visit(PatternElement& node) {}
  virtual void visit(RelationshipsPattern& node) {}
  virtual void visit(NodePattern& node) {}
  virtual void visit(RelationshipPattern& node) {}
  virtual void visit(RelationshipDetail& node) {}

  // Clauses
  virtual void visit(Clause& node) {}
  virtual void visit(ReadingClause& node) {}
  virtual void visit(Match& node) {}
  virtual void visit(Unwind& node) {}
  virtual void visit(InQueryCall& node) {}
  virtual void visit(UpdatingClause& node) {}
  virtual void visit(Create& node) {}
  virtual void visit(Merge& node) {}
  virtual void visit(Delete& node) {}
  virtual void visit(Set& node) {}
  virtual void visit(SetItem& node) {}
  virtual void visit(Remove& node) {}
  virtual void visit(RemoveItem& node) {}
  virtual void visit(ProjectionClause& node) {}
  virtual void visit(ProjectionBody& node) {}
  virtual void visit(ProjectionItem& node) {}
  virtual void visit(SortItem& node) {}
  virtual void visit(With& node) {}
  virtual void visit(Return& node) {}
};

}  // namespace ast
