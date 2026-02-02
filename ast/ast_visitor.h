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
  virtual void visit(Statement& node) = 0;
  virtual void visit(Query& node) = 0;
  virtual void visit(RegularQuery& node) = 0;
  virtual void visit(StandaloneCall& node) = 0;
  virtual void visit(SingleQuery& node) = 0;
  virtual void visit(SinglePartQuery& node) = 0;
  virtual void visit(MultiPartQuery& node) = 0;
  virtual void visit(UnionPart& node) = 0;

  // Expressions
  virtual void visit(Expression& node) = 0;
  virtual void visit(BinaryExpression& node) = 0;
  virtual void visit(OrExpression& node) = 0;
  virtual void visit(XorExpression& node) = 0;
  virtual void visit(AndExpression& node) = 0;
  virtual void visit(ComparisonExpression& node) = 0;
  virtual void visit(ComparisonChainExpression& node) = 0;
  virtual void visit(AddExpression& node) = 0;
  virtual void visit(SubtractExpression& node) = 0;
  virtual void visit(MultiplyExpression& node) = 0;
  virtual void visit(DivideExpression& node) = 0;
  virtual void visit(ModuloExpression& node) = 0;
  virtual void visit(PowerExpression& node) = 0;
  virtual void visit(UnaryExpression& node) = 0;
  virtual void visit(NotExpression& node) = 0;
  virtual void visit(UnaryPlusExpression& node) = 0;
  virtual void visit(UnaryMinusExpression& node) = 0;
  virtual void visit(StringPredicateExpression& node) = 0;
  virtual void visit(ListPredicateExpression& node) = 0;
  virtual void visit(LabelPredicateExpression& node) = 0;
  virtual void visit(NullPredicateExpression& node) = 0;

  // Literals
  virtual void visit(Literal& node) = 0;
  virtual void visit(BooleanLiteral& node) = 0;
  virtual void visit(IntegerLiteral& node) = 0;
  virtual void visit(DoubleLiteral& node) = 0;
  virtual void visit(StringLiteral& node) = 0;
  virtual void visit(NullLiteral& node) = 0;
  virtual void visit(ListLiteral& node) = 0;
  virtual void visit(MapLiteral& node) = 0;
  virtual void visit(Properties& node) = 0;

  // Other expressions
  virtual void visit(Variable& node) = 0;
  virtual void visit(Parameter& node) = 0;
  virtual void visit(PropertyExpression& node) = 0;
  virtual void visit(ListIndexExpression& node) = 0;
  virtual void visit(ListSliceExpression& node) = 0;
  virtual void visit(FunctionInvocation& node) = 0;
  virtual void visit(CountStarExpression& node) = 0;
  virtual void visit(CaseExpression& node) = 0;
  virtual void visit(ParenthesizedExpression& node) = 0;
  virtual void visit(ListComprehension& node) = 0;
  virtual void visit(PatternComprehension& node) = 0;
  virtual void visit(PatternPredicateExpression& node) = 0;
  virtual void visit(Quantifier& node) = 0;
  virtual void visit(AllQuantifier& node) = 0;
  virtual void visit(AnyQuantifier& node) = 0;
  virtual void visit(NoneQuantifier& node) = 0;
  virtual void visit(SingleQuantifier& node) = 0;
  virtual void visit(ExistentialSubquery& node) = 0;

  // Pattern
  virtual void visit(Pattern& node) = 0;
  virtual void visit(PatternPart& node) = 0;
  virtual void visit(PatternElement& node) = 0;
  virtual void visit(RelationshipsPattern& node) = 0;
  virtual void visit(NodePattern& node) = 0;
  virtual void visit(RelationshipPattern& node) = 0;
  virtual void visit(RelationshipDetail& node) = 0;

  // Clauses
  virtual void visit(Clause& node) = 0;
  virtual void visit(ReadingClause& node) = 0;
  virtual void visit(Match& node) = 0;
  virtual void visit(Unwind& node) = 0;
  virtual void visit(InQueryCall& node) = 0;
  virtual void visit(UpdatingClause& node) = 0;
  virtual void visit(Create& node) = 0;
  virtual void visit(Merge& node) = 0;
  virtual void visit(Delete& node) = 0;
  virtual void visit(Set& node) = 0;
  virtual void visit(SetItem& node) = 0;
  virtual void visit(Remove& node) = 0;
  virtual void visit(RemoveItem& node) = 0;
  virtual void visit(ProjectionClause& node) = 0;
  virtual void visit(ProjectionBody& node) = 0;
  virtual void visit(ProjectionItem& node) = 0;
  virtual void visit(SortItem& node) = 0;
  virtual void visit(With& node) = 0;
  virtual void visit(Return& node) = 0;
};

}  // namespace ast
