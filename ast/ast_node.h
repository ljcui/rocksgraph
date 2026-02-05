#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast_visitor.h"

namespace ast {

// Forward declarations
class ReadingClause;
class UpdatingClause;
class With;
class ProjectionBody;
class Return;
class Expression;
class RegularQuery;
class Pattern;
class PatternPart;
class PatternElement;
class NodePattern;
class RelationshipPattern;
class RelationshipDetail;
class Set;
class RelationshipsPattern;
class Properties;
class Parameter;

enum class ASTNodeType {
  Unknown,
  SinglePartQuery,
  MultiPartQuery,
  UnionPart,
  RegularQuery,
  StandaloneCall,
  OrExpression,
  XorExpression,
  AndExpression,
  ComparisonExpression,
  ComparisonChainExpression,
  AddExpression,
  SubtractExpression,
  MultiplyExpression,
  DivideExpression,
  ModuloExpression,
  PowerExpression,
  NotExpression,
  UnaryPlusExpression,
  UnaryMinusExpression,
  StringPredicateExpression,
  ListPredicateExpression,
  LabelPredicateExpression,
  NullPredicateExpression,
  BooleanLiteral,
  IntegerLiteral,
  DoubleLiteral,
  StringLiteral,
  NullLiteral,
  ListLiteral,
  MapLiteral,
  Properties,
  Variable,
  Parameter,
  PropertyExpression,
  ListIndexExpression,
  ListSliceExpression,
  FunctionInvocation,
  CountStarExpression,
  CaseExpression,
  ParenthesizedExpression,
  ListComprehension,
  PatternComprehension,
  PatternPredicateExpression,
  AllQuantifier,
  AnyQuantifier,
  NoneQuantifier,
  SingleQuantifier,
  ExistentialSubquery,
  Pattern,
  PatternPart,
  PatternElement,
  RelationshipsPattern,
  NodePattern,
  RelationshipPattern,
  RelationshipDetail,
  Match,
  Unwind,
  InQueryCall,
  Create,
  Merge,
  Delete,
  SetItem,
  Set,
  RemoveItem,
  Remove,
  SortItem,
  ProjectionItem,
  ProjectionBody,
  With,
  Return,
};

enum class ASTNodeCategory {
  Unknown,
  Expression,
  Clause,
  Pattern,
};

// ============================================
// Base node
// ============================================
class ASTNode {
 public:
  ASTNodeType node_type = ASTNodeType::Unknown;
  ASTNodeCategory category = ASTNodeCategory::Unknown;
  virtual ~ASTNode() = default;
  virtual void accept(ASTVisitor& visitor) = 0;
};

// ============================================
// Top-level statements and queries
// ============================================
class Statement : public ASTNode {
 protected:
  Statement() = default;
};

class Query : public Statement {
 protected:
  Query() = default;
};

class SingleQuery : public ASTNode {
 protected:
  SingleQuery() = default;
};

class SinglePartQuery : public SingleQuery {
 public:
  SinglePartQuery() { node_type = ASTNodeType::SinglePartQuery; }
  std::vector<std::unique_ptr<ReadingClause>> reading_clauses;
  std::vector<std::unique_ptr<UpdatingClause>> updating_clauses;
  std::unique_ptr<Return> return_clause;
  void validate() const {
    if (updating_clauses.empty()) {
      assert(return_clause);
    }
  }
  void accept(ASTVisitor& visitor) override;
};

class MultiPartQuery : public SingleQuery {
 public:
  MultiPartQuery() { node_type = ASTNodeType::MultiPartQuery; }
  struct WithPart {
    std::vector<std::unique_ptr<ReadingClause>> reading_clauses;
    std::vector<std::unique_ptr<UpdatingClause>> updating_clauses;
    std::unique_ptr<With> with_clause;
  };
  std::vector<WithPart> parts;
  std::unique_ptr<SinglePartQuery> final_single_part_query;
  void validate() const {
    assert(!parts.empty());
    for (const auto& part : parts) {
      assert(part.with_clause);
    }
    assert(final_single_part_query);
  }
  void accept(ASTVisitor& visitor) override;
};

class UnionPart : public ASTNode {
 public:
  UnionPart() { node_type = ASTNodeType::UnionPart; }
  bool all = false;
  std::unique_ptr<SingleQuery> query;
  void accept(ASTVisitor& visitor) override;
};

class RegularQuery : public Query {
 public:
  RegularQuery() { node_type = ASTNodeType::RegularQuery; }
  std::unique_ptr<SingleQuery> single_query;
  std::vector<std::unique_ptr<UnionPart>> unions;
  void accept(ASTVisitor& visitor) override;
};

class StandaloneCall : public Query {
 public:
  StandaloneCall() { node_type = ASTNodeType::StandaloneCall; }
  std::string procedure_name;
  std::vector<std::unique_ptr<Expression>> arguments;
  struct YieldItem {
    std::optional<std::string> result_field;  // oC_ProcedureResultField
    std::string variable;
  };
  std::vector<YieldItem> yield_items;
  bool yield_star = false;
  std::unique_ptr<Expression> yield_where;
  void validate() const {
    if (yield_star) {
      assert(yield_items.empty());
      assert(!yield_where);
    } else if (yield_items.empty()) {
      assert(!yield_where);
    }
  }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Expression base
// ============================================
class Expression : public ASTNode {
 protected:
  Expression() { category = ASTNodeCategory::Expression; }
};

// ============================================
// Binary expressions
// ============================================
class BinaryExpression : public Expression {
 public:
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> right;
};

class OrExpression : public BinaryExpression {
 public:
  OrExpression() { node_type = ASTNodeType::OrExpression; }
  void accept(ASTVisitor& visitor) override;
};

class XorExpression : public BinaryExpression {
 public:
  XorExpression() { node_type = ASTNodeType::XorExpression; }
  void accept(ASTVisitor& visitor) override;
};

class AndExpression : public BinaryExpression {
 public:
  AndExpression() { node_type = ASTNodeType::AndExpression; }
  void accept(ASTVisitor& visitor) override;
};

class ComparisonExpression : public Expression {
 public:
  ComparisonExpression() { node_type = ASTNodeType::ComparisonExpression; }
  std::unique_ptr<Expression> left;
  std::string op;  // =, <>, <, >, <=, >=
  std::unique_ptr<Expression> right;
  void accept(ASTVisitor& visitor) override;
};

class ComparisonChainExpression : public Expression {
 public:
  ComparisonChainExpression() { node_type = ASTNodeType::ComparisonChainExpression; }
  std::unique_ptr<Expression> left;
  std::vector<std::pair<std::string, std::unique_ptr<Expression>>> rights;
  void validate() const { assert(!rights.empty()); }
  void accept(ASTVisitor& visitor) override;
};

class AddExpression : public BinaryExpression {
 public:
  AddExpression() { node_type = ASTNodeType::AddExpression; }
  void accept(ASTVisitor& visitor) override;
};

class SubtractExpression : public BinaryExpression {
 public:
  SubtractExpression() { node_type = ASTNodeType::SubtractExpression; }
  void accept(ASTVisitor& visitor) override;
};

class MultiplyExpression : public BinaryExpression {
 public:
  MultiplyExpression() { node_type = ASTNodeType::MultiplyExpression; }
  void accept(ASTVisitor& visitor) override;
};

class DivideExpression : public BinaryExpression {
 public:
  DivideExpression() { node_type = ASTNodeType::DivideExpression; }
  void accept(ASTVisitor& visitor) override;
};

class ModuloExpression : public BinaryExpression {
 public:
  ModuloExpression() { node_type = ASTNodeType::ModuloExpression; }
  void accept(ASTVisitor& visitor) override;
};

class PowerExpression : public BinaryExpression {
 public:
  PowerExpression() { node_type = ASTNodeType::PowerExpression; }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Unary expressions
// ============================================
class UnaryExpression : public Expression {
 public:
  std::unique_ptr<Expression> operand;
};

class NotExpression : public UnaryExpression {
 public:
  NotExpression() { node_type = ASTNodeType::NotExpression; }
  void accept(ASTVisitor& visitor) override;
};

class UnaryPlusExpression : public UnaryExpression {
 public:
  UnaryPlusExpression() { node_type = ASTNodeType::UnaryPlusExpression; }
  void accept(ASTVisitor& visitor) override;
};

class UnaryMinusExpression : public UnaryExpression {
 public:
  UnaryMinusExpression() { node_type = ASTNodeType::UnaryMinusExpression; }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Predicate expressions
// ============================================
class StringPredicateExpression : public Expression {
 public:
  StringPredicateExpression() { node_type = ASTNodeType::StringPredicateExpression; }
  std::unique_ptr<Expression> left;
  std::string op;  // STARTS WITH, ENDS WITH, CONTAINS
  std::unique_ptr<Expression> right;
  void accept(ASTVisitor& visitor) override;
};

class ListPredicateExpression : public Expression {
 public:
  ListPredicateExpression() { node_type = ASTNodeType::ListPredicateExpression; }
  std::unique_ptr<Expression> element;
  std::unique_ptr<Expression> list;
  void accept(ASTVisitor& visitor) override;
};

class LabelPredicateExpression : public Expression {
 public:
  LabelPredicateExpression() { node_type = ASTNodeType::LabelPredicateExpression; }
  std::unique_ptr<Expression> expr;
  std::vector<std::string> labels;
  void validate() const { assert(!labels.empty()); }
  void accept(ASTVisitor& visitor) override;
};

class NullPredicateExpression : public Expression {
 public:
  NullPredicateExpression() { node_type = ASTNodeType::NullPredicateExpression; }
  std::unique_ptr<Expression> operand;
  bool is_null = true;  // IS NULL or IS NOT NULL
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Literals
// ============================================
class Literal : public Expression {
 protected:
  Literal() = default;
};

class BooleanLiteral : public Literal {
 public:
  BooleanLiteral() { node_type = ASTNodeType::BooleanLiteral; }
  bool value = false;
  void accept(ASTVisitor& visitor) override;
};

class IntegerLiteral : public Literal {
 public:
  IntegerLiteral() { node_type = ASTNodeType::IntegerLiteral; }
  int64_t value = 0;
  void accept(ASTVisitor& visitor) override;
};

class DoubleLiteral : public Literal {
 public:
  DoubleLiteral() { node_type = ASTNodeType::DoubleLiteral; }
  double value = 0.0;
  void accept(ASTVisitor& visitor) override;
};

class StringLiteral : public Literal {
 public:
  StringLiteral() { node_type = ASTNodeType::StringLiteral; }
  std::string value;
  void accept(ASTVisitor& visitor) override;
};

class NullLiteral : public Literal {
 public:
  NullLiteral() { node_type = ASTNodeType::NullLiteral; }
  void accept(ASTVisitor& visitor) override;
};

class ListLiteral : public Literal {
 public:
  ListLiteral() { node_type = ASTNodeType::ListLiteral; }
  std::vector<std::unique_ptr<Expression>> elements;
  void accept(ASTVisitor& visitor) override;
};

class MapLiteral : public Literal {
 public:
  MapLiteral() { node_type = ASTNodeType::MapLiteral; }
  std::vector<std::pair<std::string, std::unique_ptr<Expression>>> entries;
  void accept(ASTVisitor& visitor) override;
};

class Properties : public ASTNode {
 public:
  Properties() { node_type = ASTNodeType::Properties; }
  std::unique_ptr<MapLiteral> map;
  std::unique_ptr<Parameter> parameter;
  void validate() const { assert((map && !parameter) || (!map && parameter)); }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Other atomic expressions
// ============================================
class Variable : public Expression {
 public:
  Variable() { node_type = ASTNodeType::Variable; }
  std::string name;
  void accept(ASTVisitor& visitor) override;
};

class Parameter : public Expression {
 public:
  Parameter() { node_type = ASTNodeType::Parameter; }
  std::string name;
  void accept(ASTVisitor& visitor) override;
};

class PropertyExpression : public Expression {
 public:
  PropertyExpression() { node_type = ASTNodeType::PropertyExpression; }
  std::unique_ptr<Expression> object;
  std::string property_key;
  void accept(ASTVisitor& visitor) override;
};

class ListIndexExpression : public Expression {
 public:
  ListIndexExpression() { node_type = ASTNodeType::ListIndexExpression; }
  std::unique_ptr<Expression> list;
  std::unique_ptr<Expression> index;
  void validate() const { assert(list && index); }
  void accept(ASTVisitor& visitor) override;
};

class ListSliceExpression : public Expression {
 public:
  ListSliceExpression() { node_type = ASTNodeType::ListSliceExpression; }
  std::unique_ptr<Expression> list;
  std::unique_ptr<Expression> start_index;  // optional
  std::unique_ptr<Expression> end_index;    // optional
  void validate() const { assert(list); }
  void accept(ASTVisitor& visitor) override;
};

class FunctionInvocation : public Expression {
 public:
  FunctionInvocation() { node_type = ASTNodeType::FunctionInvocation; }
  std::string function_name;
  bool distinct = false;
  std::vector<std::unique_ptr<Expression>> arguments;
  void accept(ASTVisitor& visitor) override;
};

class CountStarExpression : public Expression {
 public:
  CountStarExpression() { node_type = ASTNodeType::CountStarExpression; }
  void accept(ASTVisitor& visitor) override;
};

class CaseExpression : public Expression {
 public:
  CaseExpression() { node_type = ASTNodeType::CaseExpression; }
  std::unique_ptr<Expression> test;  // for simple CASE
  std::vector<
      std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>>
      alternatives;
  std::unique_ptr<Expression> else_expr;
  void validate() const { assert(!alternatives.empty()); }
  void accept(ASTVisitor& visitor) override;
};

class ParenthesizedExpression : public Expression {
 public:
  ParenthesizedExpression() { node_type = ASTNodeType::ParenthesizedExpression; }
  std::unique_ptr<Expression> expr;
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Comprehensions and pattern predicates
// ============================================
class ListComprehension : public Expression {
 public:
  ListComprehension() { node_type = ASTNodeType::ListComprehension; }
  std::string variable;
  std::unique_ptr<Expression> list_expr;
  std::unique_ptr<Expression> where_expr;
  std::unique_ptr<Expression> eval_expr;
  void accept(ASTVisitor& visitor) override;
};

class PatternComprehension : public Expression {
 public:
  PatternComprehension() { node_type = ASTNodeType::PatternComprehension; }
  std::string variable;
  std::unique_ptr<RelationshipsPattern> relationships_pattern;
  std::unique_ptr<Expression> where_expr;
  std::unique_ptr<Expression> eval_expr;
  void accept(ASTVisitor& visitor) override;
};

class PatternPredicateExpression : public Expression {
 public:
  PatternPredicateExpression() { node_type = ASTNodeType::PatternPredicateExpression; }
  std::unique_ptr<RelationshipsPattern> relationships_pattern;
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Quantifiers
// ============================================
class Quantifier : public Expression {
 public:
  std::string variable;
  std::unique_ptr<Expression> list_expr;
  std::unique_ptr<Expression> predicate;
};

class AllQuantifier : public Quantifier {
 public:
  AllQuantifier() { node_type = ASTNodeType::AllQuantifier; }
  void accept(ASTVisitor& visitor) override;
};

class AnyQuantifier : public Quantifier {
 public:
  AnyQuantifier() { node_type = ASTNodeType::AnyQuantifier; }
  void accept(ASTVisitor& visitor) override;
};

class NoneQuantifier : public Quantifier {
 public:
  NoneQuantifier() { node_type = ASTNodeType::NoneQuantifier; }
  void accept(ASTVisitor& visitor) override;
};

class SingleQuantifier : public Quantifier {
 public:
  SingleQuantifier() { node_type = ASTNodeType::SingleQuantifier; }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Existential subquery
// ============================================
class ExistentialSubquery : public Expression {
 public:
  ExistentialSubquery() { node_type = ASTNodeType::ExistentialSubquery; }
  std::unique_ptr<RegularQuery> query;
  std::unique_ptr<Pattern> pattern;
  std::unique_ptr<Expression> where_expr;
  void validate() const {
    const bool has_query = static_cast<bool>(query);
    const bool has_pattern = static_cast<bool>(pattern);
    assert(has_query ^ has_pattern);
    if (has_query) {
      assert(!where_expr);
    }
  }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Pattern
// ============================================
class Pattern : public ASTNode {
 public:
  Pattern() {
    node_type = ASTNodeType::Pattern;
    category = ASTNodeCategory::Pattern;
  }
  std::vector<std::unique_ptr<PatternPart>> parts;
  void validate() const { assert(!parts.empty()); }
  void accept(ASTVisitor& visitor) override;
};

class PatternPart : public ASTNode {
 public:
  PatternPart() {
    node_type = ASTNodeType::PatternPart;
    category = ASTNodeCategory::Pattern;
  }
  std::string variable;
  std::unique_ptr<PatternElement> element;
  void validate() const { assert(element); }
  void accept(ASTVisitor& visitor) override;
};

class PatternElement : public ASTNode {
 public:
  PatternElement() {
    node_type = ASTNodeType::PatternElement;
    category = ASTNodeCategory::Pattern;
  }
  std::unique_ptr<NodePattern> node_pattern;
  std::vector<std::pair<std::unique_ptr<RelationshipPattern>,
                        std::unique_ptr<NodePattern>>>
      chain;
  void validate() const { assert(node_pattern); }
  void accept(ASTVisitor& visitor) override;
};

class RelationshipsPattern : public ASTNode {
 public:
  RelationshipsPattern() {
    node_type = ASTNodeType::RelationshipsPattern;
    category = ASTNodeCategory::Pattern;
  }
  std::unique_ptr<NodePattern> node_pattern;
  std::vector<std::pair<std::unique_ptr<RelationshipPattern>,
                        std::unique_ptr<NodePattern>>>
      chain;  // must be non-empty to satisfy grammar
  void validate() const {
    assert(node_pattern);
    assert(!chain.empty());
  }
  void accept(ASTVisitor& visitor) override;
};

class NodePattern : public ASTNode {
 public:
  NodePattern() {
    node_type = ASTNodeType::NodePattern;
    category = ASTNodeCategory::Pattern;
  }
  std::string variable;
  std::vector<std::string> labels;
  std::unique_ptr<Properties> properties;
  void accept(ASTVisitor& visitor) override;
};

class RelationshipPattern : public ASTNode {
 public:
  RelationshipPattern() {
    node_type = ASTNodeType::RelationshipPattern;
    category = ASTNodeCategory::Pattern;
  }
  bool left_arrow = false;
  bool right_arrow = false;
  std::unique_ptr<RelationshipDetail> detail;
  void accept(ASTVisitor& visitor) override;
};

class RelationshipDetail : public ASTNode {
 public:
  RelationshipDetail() {
    node_type = ASTNodeType::RelationshipDetail;
    category = ASTNodeCategory::Pattern;
  }
  struct RangeLiteral {
    std::optional<int> min;
    std::optional<int> max;
  };
  std::string variable;
  std::vector<std::string> types;
  std::optional<RangeLiteral> range;  // *min..max
  std::unique_ptr<Properties> properties;
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// Clauses
// ============================================
class Clause : public ASTNode {
 protected:
  Clause() { category = ASTNodeCategory::Clause; }
};

// Reading clauses
class ReadingClause : public Clause {
 protected:
  ReadingClause() = default;
};

class Match : public ReadingClause {
 public:
  Match() { node_type = ASTNodeType::Match; }
  bool optional_match = false;
  std::unique_ptr<Pattern> pattern;
  std::unique_ptr<Expression> where;
  void accept(ASTVisitor& visitor) override;
};

class Unwind : public ReadingClause {
 public:
  Unwind() { node_type = ASTNodeType::Unwind; }
  std::unique_ptr<Expression> expression;
  std::string variable;
  void accept(ASTVisitor& visitor) override;
};

class InQueryCall : public ReadingClause {
 public:
  InQueryCall() { node_type = ASTNodeType::InQueryCall; }
  std::string procedure_name;
  std::vector<std::unique_ptr<Expression>> arguments;
  std::vector<StandaloneCall::YieldItem> yield_items;
  std::unique_ptr<Expression> yield_where;
  void validate() const { assert(!yield_where || !yield_items.empty()); }
  void accept(ASTVisitor& visitor) override;
};

// Updating clauses
class UpdatingClause : public Clause {
 protected:
  UpdatingClause() = default;
};

class Create : public UpdatingClause {
 public:
  Create() { node_type = ASTNodeType::Create; }
  std::unique_ptr<Pattern> pattern;
  void accept(ASTVisitor& visitor) override;
};

class Merge : public UpdatingClause {
 public:
  Merge() { node_type = ASTNodeType::Merge; }
  std::unique_ptr<PatternPart> pattern_part;
  std::vector<std::pair<bool, std::unique_ptr<Set>>>
      actions;  // bool: true=ON MATCH, false=ON CREATE
  void accept(ASTVisitor& visitor) override;
};

class Delete : public UpdatingClause {
 public:
  Delete() { node_type = ASTNodeType::Delete; }
  bool detach = false;
  std::vector<std::unique_ptr<Expression>> expressions;
  void accept(ASTVisitor& visitor) override;
};

class SetItem : public ASTNode {
 public:
  SetItem() { node_type = ASTNodeType::SetItem; }
  enum class Type { Property, Variable, Labels };
  Type type;
  std::unique_ptr<Expression> target;
  std::unique_ptr<Expression> value;
  std::vector<std::string> labels;
  bool plus_equal = false;
  void validate() const {
    assert(target);
    switch (type) {
      case Type::Property:
        assert(dynamic_cast<PropertyExpression*>(target.get()));
        assert(value);
        assert(!plus_equal);
        assert(labels.empty());
        break;
      case Type::Variable:
        assert(dynamic_cast<Variable*>(target.get()));
        assert(value);
        assert(labels.empty());
        break;
      case Type::Labels:
        assert(dynamic_cast<Variable*>(target.get()));
        assert(!value);
        assert(!labels.empty());
        assert(!plus_equal);
        break;
    }
    if (plus_equal) {
      assert(type == Type::Variable);
    }
  }
  void accept(ASTVisitor& visitor) override;
};

class Set : public UpdatingClause {
 public:
  Set() { node_type = ASTNodeType::Set; }
  std::vector<std::unique_ptr<SetItem>> items;
  void accept(ASTVisitor& visitor) override;
};

class RemoveItem : public ASTNode {
 public:
  RemoveItem() { node_type = ASTNodeType::RemoveItem; }
  enum class Type { Property, Labels };
  Type type;
  std::unique_ptr<Expression> target;
  std::vector<std::string> labels;
  void validate() const {
    assert(target);
    switch (type) {
      case Type::Property:
        assert(dynamic_cast<PropertyExpression*>(target.get()));
        assert(labels.empty());
        break;
      case Type::Labels:
        assert(dynamic_cast<Variable*>(target.get()));
        assert(!labels.empty());
        break;
    }
  }
  void accept(ASTVisitor& visitor) override;
};

class Remove : public UpdatingClause {
 public:
  Remove() { node_type = ASTNodeType::Remove; }
  std::vector<std::unique_ptr<RemoveItem>> items;
  void accept(ASTVisitor& visitor) override;
};

// Projection clauses
class SortItem : public ASTNode {
 public:
  SortItem() { node_type = ASTNodeType::SortItem; }
  std::unique_ptr<Expression> expression;
  bool ascending = true;
  void accept(ASTVisitor& visitor) override;
};

class ProjectionItem : public ASTNode {
 public:
  ProjectionItem() { node_type = ASTNodeType::ProjectionItem; }
  std::unique_ptr<Expression> expression;
  std::string alias;
  void accept(ASTVisitor& visitor) override;
};

class ProjectionBody : public ASTNode {
 public:
  ProjectionBody() { node_type = ASTNodeType::ProjectionBody; }
  bool distinct = false;
  bool star = false;
  std::vector<std::unique_ptr<ProjectionItem>> items;
  std::vector<std::unique_ptr<SortItem>> order_by;
  std::unique_ptr<Expression> skip;
  std::unique_ptr<Expression> limit;
  void validate() const { assert(star || !items.empty()); }
  void accept(ASTVisitor& visitor) override;
};

class ProjectionClause : public Clause {
 public:
  std::unique_ptr<ProjectionBody> body;
};

class With : public ProjectionClause {
 public:
  With() { node_type = ASTNodeType::With; }
  std::unique_ptr<Expression> where;
  void accept(ASTVisitor& visitor) override;
};

class Return : public ProjectionClause {
 public:
  Return() { node_type = ASTNodeType::Return; }
  void accept(ASTVisitor& visitor) override;
};

// ============================================
// accept implementations
// ============================================
inline void RegularQuery::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void StandaloneCall::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void SinglePartQuery::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void MultiPartQuery::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void UnionPart::accept(ASTVisitor& visitor) { visitor.visit(*this); }

inline void OrExpression::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void XorExpression::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void AndExpression::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ComparisonExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void ComparisonChainExpression::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void AddExpression::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void SubtractExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void MultiplyExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void DivideExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void ModuloExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void PowerExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void NotExpression::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void UnaryPlusExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void UnaryMinusExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void StringPredicateExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void ListPredicateExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void LabelPredicateExpression::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void NullPredicateExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}

inline void BooleanLiteral::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void IntegerLiteral::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void DoubleLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void StringLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void NullLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ListLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void MapLiteral::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Properties::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}

inline void Variable::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Parameter::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void PropertyExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void ListIndexExpression::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void ListSliceExpression::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void FunctionInvocation::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void CountStarExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void CaseExpression::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void ParenthesizedExpression::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void ListComprehension::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void PatternComprehension::accept(ASTVisitor& visitor) {
  if (relationships_pattern) {
    relationships_pattern->validate();
  }
  visitor.visit(*this);
}
inline void PatternPredicateExpression::accept(ASTVisitor& visitor) {
  if (relationships_pattern) {
    relationships_pattern->validate();
  }
  visitor.visit(*this);
}
inline void AllQuantifier::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void AnyQuantifier::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void NoneQuantifier::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void SingleQuantifier::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void ExistentialSubquery::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}

inline void Pattern::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void PatternPart::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void PatternElement::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void RelationshipsPattern::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void RelationshipPattern::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void NodePattern::accept(ASTVisitor& visitor) {
  if (properties) {
    properties->validate();
  }
  visitor.visit(*this);
}
inline void RelationshipDetail::accept(ASTVisitor& visitor) {
  if (properties) {
    properties->validate();
  }
  visitor.visit(*this);
}

inline void Match::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Unwind::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void InQueryCall::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void Create::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Merge::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Delete::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Set::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void SetItem::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void Remove::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void RemoveItem::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void ProjectionBody::accept(ASTVisitor& visitor) {
  validate();
  visitor.visit(*this);
}
inline void ProjectionItem::accept(ASTVisitor& visitor) {
  visitor.visit(*this);
}
inline void SortItem::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void With::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Return::accept(ASTVisitor& visitor) { visitor.visit(*this); }

}  // namespace ast
