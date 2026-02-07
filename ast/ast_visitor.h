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
  virtual void Visit(Statement& node) = 0;
  virtual void Visit(Query& node) = 0;
  virtual void Visit(RegularQuery& node) = 0;
  virtual void Visit(StandaloneCall& node) = 0;
  virtual void Visit(SingleQuery& node) = 0;
  virtual void Visit(SinglePartQuery& node) = 0;
  virtual void Visit(MultiPartQuery& node) = 0;
  virtual void Visit(UnionPart& node) = 0;

  // Expressions
  virtual void Visit(Expression& node) = 0;
  virtual void Visit(BinaryExpression& node) = 0;
  virtual void Visit(OrExpression& node) = 0;
  virtual void Visit(XorExpression& node) = 0;
  virtual void Visit(AndExpression& node) = 0;
  virtual void Visit(ComparisonExpression& node) = 0;
  virtual void Visit(ComparisonChainExpression& node) = 0;
  virtual void Visit(AddExpression& node) = 0;
  virtual void Visit(SubtractExpression& node) = 0;
  virtual void Visit(MultiplyExpression& node) = 0;
  virtual void Visit(DivideExpression& node) = 0;
  virtual void Visit(ModuloExpression& node) = 0;
  virtual void Visit(PowerExpression& node) = 0;
  virtual void Visit(UnaryExpression& node) = 0;
  virtual void Visit(NotExpression& node) = 0;
  virtual void Visit(UnaryPlusExpression& node) = 0;
  virtual void Visit(UnaryMinusExpression& node) = 0;
  virtual void Visit(StringPredicateExpression& node) = 0;
  virtual void Visit(ListPredicateExpression& node) = 0;
  virtual void Visit(LabelPredicateExpression& node) = 0;
  virtual void Visit(NullPredicateExpression& node) = 0;

  // Literals
  virtual void Visit(Literal& node) = 0;
  virtual void Visit(BooleanLiteral& node) = 0;
  virtual void Visit(IntegerLiteral& node) = 0;
  virtual void Visit(DoubleLiteral& node) = 0;
  virtual void Visit(StringLiteral& node) = 0;
  virtual void Visit(NullLiteral& node) = 0;
  virtual void Visit(ListLiteral& node) = 0;
  virtual void Visit(MapLiteral& node) = 0;
  virtual void Visit(Properties& node) = 0;

  // Other expressions
  virtual void Visit(Variable& node) = 0;
  virtual void Visit(Parameter& node) = 0;
  virtual void Visit(PropertyExpression& node) = 0;
  virtual void Visit(ListIndexExpression& node) = 0;
  virtual void Visit(ListSliceExpression& node) = 0;
  virtual void Visit(FunctionInvocation& node) = 0;
  virtual void Visit(CountStarExpression& node) = 0;
  virtual void Visit(CaseExpression& node) = 0;
  virtual void Visit(ParenthesizedExpression& node) = 0;
  virtual void Visit(ListComprehension& node) = 0;
  virtual void Visit(PatternComprehension& node) = 0;
  virtual void Visit(PatternPredicateExpression& node) = 0;
  virtual void Visit(Quantifier& node) = 0;
  virtual void Visit(AllQuantifier& node) = 0;
  virtual void Visit(AnyQuantifier& node) = 0;
  virtual void Visit(NoneQuantifier& node) = 0;
  virtual void Visit(SingleQuantifier& node) = 0;
  virtual void Visit(ExistentialSubquery& node) = 0;

  // Pattern
  virtual void Visit(Pattern& node) = 0;
  virtual void Visit(PatternPart& node) = 0;
  virtual void Visit(PatternElement& node) = 0;
  virtual void Visit(RelationshipsPattern& node) = 0;
  virtual void Visit(NodePattern& node) = 0;
  virtual void Visit(RelationshipPattern& node) = 0;
  virtual void Visit(RelationshipDetail& node) = 0;

  // Clauses
  virtual void Visit(Clause& node) = 0;
  virtual void Visit(ReadingClause& node) = 0;
  virtual void Visit(Match& node) = 0;
  virtual void Visit(Unwind& node) = 0;
  virtual void Visit(InQueryCall& node) = 0;
  virtual void Visit(UpdatingClause& node) = 0;
  virtual void Visit(Create& node) = 0;
  virtual void Visit(Merge& node) = 0;
  virtual void Visit(Delete& node) = 0;
  virtual void Visit(Set& node) = 0;
  virtual void Visit(SetItem& node) = 0;
  virtual void Visit(Remove& node) = 0;
  virtual void Visit(RemoveItem& node) = 0;
  virtual void Visit(ProjectionClause& node) = 0;
  virtual void Visit(ProjectionBody& node) = 0;
  virtual void Visit(ProjectionItem& node) = 0;
  virtual void Visit(SortItem& node) = 0;
  virtual void Visit(With& node) = 0;
  virtual void Visit(Return& node) = 0;
};

}  // namespace ast
