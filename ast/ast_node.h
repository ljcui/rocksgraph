#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ast_const_visitor.h"

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

#define AST_NODE_TYPE_LIST(X)                                  \
  X(kUnknown, "Unknown")                                       \
  X(kSinglePartQuery, "SinglePartQuery")                       \
  X(kMultiPartQuery, "MultiPartQuery")                         \
  X(kUnionPart, "UnionPart")                                   \
  X(kRegularQuery, "RegularQuery")                             \
  X(kStandaloneCall, "StandaloneCall")                         \
  X(kOrExpression, "OrExpression")                             \
  X(kXorExpression, "XorExpression")                           \
  X(kAndExpression, "AndExpression")                           \
  X(kComparisonExpression, "ComparisonExpression")             \
  X(kComparisonChainExpression, "ComparisonChainExpression")   \
  X(kAddExpression, "AddExpression")                           \
  X(kSubtractExpression, "SubtractExpression")                 \
  X(kMultiplyExpression, "MultiplyExpression")                 \
  X(kDivideExpression, "DivideExpression")                     \
  X(kModuloExpression, "ModuloExpression")                     \
  X(kPowerExpression, "PowerExpression")                       \
  X(kNotExpression, "NotExpression")                           \
  X(kUnaryPlusExpression, "UnaryPlusExpression")               \
  X(kUnaryMinusExpression, "UnaryMinusExpression")             \
  X(kStringPredicateExpression, "StringPredicateExpression")   \
  X(kListPredicateExpression, "ListPredicateExpression")       \
  X(kLabelPredicateExpression, "LabelPredicateExpression")     \
  X(kNullPredicateExpression, "NullPredicateExpression")       \
  X(kBooleanLiteral, "BooleanLiteral")                         \
  X(kIntegerLiteral, "IntegerLiteral")                         \
  X(kDoubleLiteral, "DoubleLiteral")                           \
  X(kStringLiteral, "StringLiteral")                           \
  X(kNullLiteral, "NullLiteral")                               \
  X(kListLiteral, "ListLiteral")                               \
  X(kMapLiteral, "MapLiteral")                                 \
  X(kProperties, "Properties")                                 \
  X(kVariable, "Variable")                                     \
  X(kParameter, "Parameter")                                   \
  X(kPropertyExpression, "PropertyExpression")                 \
  X(kListIndexExpression, "ListIndexExpression")               \
  X(kListSliceExpression, "ListSliceExpression")               \
  X(kFunctionInvocation, "FunctionInvocation")                 \
  X(kCountStarExpression, "CountStarExpression")               \
  X(kCaseExpression, "CaseExpression")                         \
  X(kParenthesizedExpression, "ParenthesizedExpression")       \
  X(kListComprehension, "ListComprehension")                   \
  X(kPatternComprehension, "PatternComprehension")             \
  X(kPatternPredicateExpression, "PatternPredicateExpression") \
  X(kAllQuantifier, "AllQuantifier")                           \
  X(kAnyQuantifier, "AnyQuantifier")                           \
  X(kNoneQuantifier, "NoneQuantifier")                         \
  X(kSingleQuantifier, "SingleQuantifier")                     \
  X(kExistentialSubquery, "ExistentialSubquery")               \
  X(kPattern, "Pattern")                                       \
  X(kPatternPart, "PatternPart")                               \
  X(kPatternElement, "PatternElement")                         \
  X(kRelationshipsPattern, "RelationshipsPattern")             \
  X(kNodePattern, "NodePattern")                               \
  X(kRelationshipPattern, "RelationshipPattern")               \
  X(kRelationshipDetail, "RelationshipDetail")                 \
  X(kMatch, "Match")                                           \
  X(kUnwind, "Unwind")                                         \
  X(kInQueryCall, "InQueryCall")                               \
  X(kCreate, "Create")                                         \
  X(kMerge, "Merge")                                           \
  X(kDelete, "Delete")                                         \
  X(kSetItem, "SetItem")                                       \
  X(kSet, "Set")                                               \
  X(kRemoveItem, "RemoveItem")                                 \
  X(kRemove, "Remove")                                         \
  X(kSortItem, "SortItem")                                     \
  X(kProjectionItem, "ProjectionItem")                         \
  X(kProjectionBody, "ProjectionBody")                         \
  X(kWith, "With")                                             \
  X(kReturn, "Return")

enum class ASTNodeType {
#define AST_NODE_TYPE_ENUM(name, text) name,
  AST_NODE_TYPE_LIST(AST_NODE_TYPE_ENUM)
#undef AST_NODE_TYPE_ENUM
};

inline constexpr auto kASTNodeTypeNames = std::array{
#define AST_NODE_TYPE_NAME(name, text) std::string_view{text},
    AST_NODE_TYPE_LIST(AST_NODE_TYPE_NAME)
#undef AST_NODE_TYPE_NAME
};

static_assert(kASTNodeTypeNames.size() ==
              static_cast<std::size_t>(ASTNodeType::kReturn) + 1);

inline constexpr std::string_view ToString(ASTNodeType node_type) {
  const auto index = static_cast<std::size_t>(node_type);
  if (index >= kASTNodeTypeNames.size()) {
    return "Unknown";
  }
  return kASTNodeTypeNames[index];
}

#undef AST_NODE_TYPE_LIST

inline std::ostream& operator<<(std::ostream& out, ASTNodeType node_type) {
  out << ToString(node_type);
  return out;
}

enum class ASTNodeCategory {
  kUnknown,
  kExpression,
  kClause,
  kPattern,
};

inline constexpr auto kASTNodeCategoryNames = std::array{
    std::string_view{"Unknown"},
    std::string_view{"Expression"},
    std::string_view{"Clause"},
    std::string_view{"Pattern"},
};

static_assert(kASTNodeCategoryNames.size() ==
              static_cast<std::size_t>(ASTNodeCategory::kPattern) + 1);

inline constexpr std::string_view ToString(ASTNodeCategory category) {
  const auto index = static_cast<std::size_t>(category);
  if (index >= kASTNodeCategoryNames.size()) {
    return "Unknown";
  }
  return kASTNodeCategoryNames[index];
}

inline std::ostream& operator<<(std::ostream& out, ASTNodeCategory category) {
  out << ToString(category);
  return out;
}

// ============================================
// Base node
// ============================================
class ASTNode {
 public:
  ASTNodeType node_type = ASTNodeType::kUnknown;
  ASTNodeCategory category = ASTNodeCategory::kUnknown;
  virtual ~ASTNode() = default;
  [[nodiscard]] constexpr bool Is(ASTNodeType expected_type) const {
    return node_type == expected_type;
  }
  virtual void Accept(ASTVisitor& visitor) = 0;
  virtual void Accept(ASTConstVisitor& visitor) const {
    const_cast<ASTNode*>(this)->Accept(static_cast<ASTVisitor&>(visitor));
  }
};

template <typename Derived, typename Base>
[[nodiscard]] constexpr auto* CastAst(Base* node) {
  static_assert(std::is_base_of_v<ASTNode, std::remove_cv_t<Base>>);
  static_assert(std::is_base_of_v<ASTNode, std::remove_cv_t<Derived>>);
  using Result =
      std::conditional_t<std::is_const_v<Base>, const Derived, Derived>;
  return static_cast<Result*>(node);
}

template <typename Derived, typename Base>
[[nodiscard]] constexpr auto& CastAst(Base& node) {
  static_assert(std::is_base_of_v<ASTNode, std::remove_cv_t<Base>>);
  static_assert(std::is_base_of_v<ASTNode, std::remove_cv_t<Derived>>);
  using Result =
      std::conditional_t<std::is_const_v<Base>, const Derived, Derived>;
  return static_cast<Result&>(node);
}

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
  SinglePartQuery() { node_type = ASTNodeType::kSinglePartQuery; }
  std::vector<std::unique_ptr<ReadingClause>> reading_clauses;
  std::vector<std::unique_ptr<UpdatingClause>> updating_clauses;
  std::unique_ptr<Return> return_clause;
  void Validate() const {
    if (updating_clauses.empty()) {
      assert(return_clause);
    }
  }
  void Accept(ASTVisitor& visitor) override;
};

class MultiPartQuery : public SingleQuery {
 public:
  MultiPartQuery() { node_type = ASTNodeType::kMultiPartQuery; }
  struct WithPart {
    std::vector<std::unique_ptr<ReadingClause>> reading_clauses;
    std::vector<std::unique_ptr<UpdatingClause>> updating_clauses;
    std::unique_ptr<With> with_clause;
  };
  std::vector<WithPart> parts;
  std::unique_ptr<SinglePartQuery> final_single_part_query;
  void Validate() const {
    assert(!parts.empty());
    for (const auto& part : parts) {
      assert(part.with_clause);
    }
    assert(final_single_part_query);
  }
  void Accept(ASTVisitor& visitor) override;
};

class UnionPart : public ASTNode {
 public:
  UnionPart() { node_type = ASTNodeType::kUnionPart; }
  bool all = false;
  std::unique_ptr<SingleQuery> query;
  void Accept(ASTVisitor& visitor) override;
};

class RegularQuery : public Query {
 public:
  RegularQuery() { node_type = ASTNodeType::kRegularQuery; }
  std::unique_ptr<SingleQuery> single_query;
  std::vector<std::unique_ptr<UnionPart>> unions;
  void Accept(ASTVisitor& visitor) override;
};

class StandaloneCall : public Query {
 public:
  StandaloneCall() { node_type = ASTNodeType::kStandaloneCall; }
  std::string procedure_name;
  std::vector<std::unique_ptr<Expression>> arguments;
  struct YieldItem {
    std::optional<std::string> result_field;  // oC_ProcedureResultField
    std::string variable;
  };
  std::vector<YieldItem> yield_items;
  bool yield_star = false;
  std::unique_ptr<Expression> yield_where;
  void Validate() const {
    if (yield_star) {
      assert(yield_items.empty());
      assert(!yield_where);
    } else if (yield_items.empty()) {
      assert(!yield_where);
    }
  }
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Expression base
// ============================================
class Expression : public ASTNode {
 protected:
  Expression() { category = ASTNodeCategory::kExpression; }
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
  OrExpression() { node_type = ASTNodeType::kOrExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class XorExpression : public BinaryExpression {
 public:
  XorExpression() { node_type = ASTNodeType::kXorExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class AndExpression : public BinaryExpression {
 public:
  AndExpression() { node_type = ASTNodeType::kAndExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class ComparisonExpression : public Expression {
 public:
  ComparisonExpression() { node_type = ASTNodeType::kComparisonExpression; }
  std::unique_ptr<Expression> left;
  std::string op;  // =, <>, <, >, <=, >=
  std::unique_ptr<Expression> right;
  void Accept(ASTVisitor& visitor) override;
};

class ComparisonChainExpression : public Expression {
 public:
  ComparisonChainExpression() {
    node_type = ASTNodeType::kComparisonChainExpression;
  }
  std::unique_ptr<Expression> left;
  std::vector<std::pair<std::string, std::unique_ptr<Expression>>> rights;
  void Validate() const { assert(!rights.empty()); }
  void Accept(ASTVisitor& visitor) override;
};

class AddExpression : public BinaryExpression {
 public:
  AddExpression() { node_type = ASTNodeType::kAddExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class SubtractExpression : public BinaryExpression {
 public:
  SubtractExpression() { node_type = ASTNodeType::kSubtractExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class MultiplyExpression : public BinaryExpression {
 public:
  MultiplyExpression() { node_type = ASTNodeType::kMultiplyExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class DivideExpression : public BinaryExpression {
 public:
  DivideExpression() { node_type = ASTNodeType::kDivideExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class ModuloExpression : public BinaryExpression {
 public:
  ModuloExpression() { node_type = ASTNodeType::kModuloExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class PowerExpression : public BinaryExpression {
 public:
  PowerExpression() { node_type = ASTNodeType::kPowerExpression; }
  void Accept(ASTVisitor& visitor) override;
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
  NotExpression() { node_type = ASTNodeType::kNotExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class UnaryPlusExpression : public UnaryExpression {
 public:
  UnaryPlusExpression() { node_type = ASTNodeType::kUnaryPlusExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class UnaryMinusExpression : public UnaryExpression {
 public:
  UnaryMinusExpression() { node_type = ASTNodeType::kUnaryMinusExpression; }
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Predicate expressions
// ============================================
class StringPredicateExpression : public Expression {
 public:
  StringPredicateExpression() {
    node_type = ASTNodeType::kStringPredicateExpression;
  }
  std::unique_ptr<Expression> left;
  std::string op;  // STARTS WITH, ENDS WITH, CONTAINS
  std::unique_ptr<Expression> right;
  void Accept(ASTVisitor& visitor) override;
};

class ListPredicateExpression : public Expression {
 public:
  ListPredicateExpression() {
    node_type = ASTNodeType::kListPredicateExpression;
  }
  std::unique_ptr<Expression> element;
  std::unique_ptr<Expression> list;
  void Accept(ASTVisitor& visitor) override;
};

class LabelPredicateExpression : public Expression {
 public:
  LabelPredicateExpression() {
    node_type = ASTNodeType::kLabelPredicateExpression;
  }
  std::unique_ptr<Expression> expr;
  std::vector<std::string> labels;
  void Validate() const { assert(!labels.empty()); }
  void Accept(ASTVisitor& visitor) override;
};

class NullPredicateExpression : public Expression {
 public:
  NullPredicateExpression() {
    node_type = ASTNodeType::kNullPredicateExpression;
  }
  std::unique_ptr<Expression> operand;
  bool is_null = true;  // IS NULL or IS NOT NULL
  void Accept(ASTVisitor& visitor) override;
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
  BooleanLiteral() { node_type = ASTNodeType::kBooleanLiteral; }
  bool value = false;
  void Accept(ASTVisitor& visitor) override;
};

class IntegerLiteral : public Literal {
 public:
  IntegerLiteral() { node_type = ASTNodeType::kIntegerLiteral; }
  int64_t value = 0;
  void Accept(ASTVisitor& visitor) override;
};

class DoubleLiteral : public Literal {
 public:
  DoubleLiteral() { node_type = ASTNodeType::kDoubleLiteral; }
  double value = 0.0;
  void Accept(ASTVisitor& visitor) override;
};

class StringLiteral : public Literal {
 public:
  StringLiteral() { node_type = ASTNodeType::kStringLiteral; }
  std::string value;
  void Accept(ASTVisitor& visitor) override;
};

class NullLiteral : public Literal {
 public:
  NullLiteral() { node_type = ASTNodeType::kNullLiteral; }
  void Accept(ASTVisitor& visitor) override;
};

class ListLiteral : public Literal {
 public:
  ListLiteral() { node_type = ASTNodeType::kListLiteral; }
  std::vector<std::unique_ptr<Expression>> elements;
  void Accept(ASTVisitor& visitor) override;
};

class MapLiteral : public Literal {
 public:
  MapLiteral() { node_type = ASTNodeType::kMapLiteral; }
  std::vector<std::pair<std::string, std::unique_ptr<Expression>>> entries;
  void Accept(ASTVisitor& visitor) override;
};

class Properties : public ASTNode {
 public:
  Properties() { node_type = ASTNodeType::kProperties; }
  std::unique_ptr<MapLiteral> map;
  std::unique_ptr<Parameter> parameter;
  void Validate() const { assert((map && !parameter) || (!map && parameter)); }
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Other atomic expressions
// ============================================
class Variable : public Expression {
 public:
  Variable() { node_type = ASTNodeType::kVariable; }
  std::string name;
  void Accept(ASTVisitor& visitor) override;
};

class Parameter : public Expression {
 public:
  Parameter() { node_type = ASTNodeType::kParameter; }
  std::string name;
  void Accept(ASTVisitor& visitor) override;
};

class PropertyExpression : public Expression {
 public:
  PropertyExpression() { node_type = ASTNodeType::kPropertyExpression; }
  std::unique_ptr<Expression> object;
  std::string property_key;
  void Accept(ASTVisitor& visitor) override;
};

class ListIndexExpression : public Expression {
 public:
  ListIndexExpression() { node_type = ASTNodeType::kListIndexExpression; }
  std::unique_ptr<Expression> list;
  std::unique_ptr<Expression> index;
  void Validate() const { assert(list && index); }
  void Accept(ASTVisitor& visitor) override;
};

class ListSliceExpression : public Expression {
 public:
  ListSliceExpression() { node_type = ASTNodeType::kListSliceExpression; }
  std::unique_ptr<Expression> list;
  std::unique_ptr<Expression> start_index;  // optional
  std::unique_ptr<Expression> end_index;    // optional
  void Validate() const { assert(list); }
  void Accept(ASTVisitor& visitor) override;
};

class FunctionInvocation : public Expression {
 public:
  FunctionInvocation() { node_type = ASTNodeType::kFunctionInvocation; }
  std::string function_name;
  bool distinct = false;
  std::vector<std::unique_ptr<Expression>> arguments;
  void Accept(ASTVisitor& visitor) override;
};

class CountStarExpression : public Expression {
 public:
  CountStarExpression() { node_type = ASTNodeType::kCountStarExpression; }
  void Accept(ASTVisitor& visitor) override;
};

class CaseExpression : public Expression {
 public:
  CaseExpression() { node_type = ASTNodeType::kCaseExpression; }
  std::unique_ptr<Expression> test;  // for simple CASE
  std::vector<
      std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>>
      alternatives;
  std::unique_ptr<Expression> else_expr;
  void Validate() const { assert(!alternatives.empty()); }
  void Accept(ASTVisitor& visitor) override;
};

class ParenthesizedExpression : public Expression {
 public:
  ParenthesizedExpression() {
    node_type = ASTNodeType::kParenthesizedExpression;
  }
  std::unique_ptr<Expression> expr;
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Comprehensions and pattern predicates
// ============================================
class ListComprehension : public Expression {
 public:
  ListComprehension() { node_type = ASTNodeType::kListComprehension; }
  std::string variable;
  std::unique_ptr<Expression> list_expr;
  std::unique_ptr<Expression> where_expr;
  std::unique_ptr<Expression> eval_expr;
  void Accept(ASTVisitor& visitor) override;
};

class PatternComprehension : public Expression {
 public:
  PatternComprehension() { node_type = ASTNodeType::kPatternComprehension; }
  std::string variable;
  std::unique_ptr<RelationshipsPattern> relationships_pattern;
  std::unique_ptr<Expression> where_expr;
  std::unique_ptr<Expression> eval_expr;
  void Accept(ASTVisitor& visitor) override;
};

class PatternPredicateExpression : public Expression {
 public:
  PatternPredicateExpression() {
    node_type = ASTNodeType::kPatternPredicateExpression;
  }
  std::unique_ptr<RelationshipsPattern> relationships_pattern;
  void Accept(ASTVisitor& visitor) override;
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
  AllQuantifier() { node_type = ASTNodeType::kAllQuantifier; }
  void Accept(ASTVisitor& visitor) override;
};

class AnyQuantifier : public Quantifier {
 public:
  AnyQuantifier() { node_type = ASTNodeType::kAnyQuantifier; }
  void Accept(ASTVisitor& visitor) override;
};

class NoneQuantifier : public Quantifier {
 public:
  NoneQuantifier() { node_type = ASTNodeType::kNoneQuantifier; }
  void Accept(ASTVisitor& visitor) override;
};

class SingleQuantifier : public Quantifier {
 public:
  SingleQuantifier() { node_type = ASTNodeType::kSingleQuantifier; }
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Existential subquery
// ============================================
class ExistentialSubquery : public Expression {
 public:
  ExistentialSubquery() { node_type = ASTNodeType::kExistentialSubquery; }
  std::unique_ptr<RegularQuery> query;
  std::unique_ptr<Pattern> pattern;
  std::unique_ptr<Expression> where_expr;
  void Validate() const {
    const bool has_query = static_cast<bool>(query);
    const bool has_pattern = static_cast<bool>(pattern);
    assert(has_query ^ has_pattern);
    if (has_query) {
      assert(!where_expr);
    }
  }
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Pattern
// ============================================
class Pattern : public ASTNode {
 public:
  Pattern() {
    node_type = ASTNodeType::kPattern;
    category = ASTNodeCategory::kPattern;
  }
  std::vector<std::unique_ptr<PatternPart>> parts;
  void Validate() const { assert(!parts.empty()); }
  void Accept(ASTVisitor& visitor) override;
};

class PatternPart : public ASTNode {
 public:
  PatternPart() {
    node_type = ASTNodeType::kPatternPart;
    category = ASTNodeCategory::kPattern;
  }
  std::string variable;
  std::unique_ptr<PatternElement> element;
  void Validate() const { assert(element); }
  void Accept(ASTVisitor& visitor) override;
};

class PatternElement : public ASTNode {
 public:
  PatternElement() {
    node_type = ASTNodeType::kPatternElement;
    category = ASTNodeCategory::kPattern;
  }
  std::unique_ptr<NodePattern> node_pattern;
  std::vector<std::pair<std::unique_ptr<RelationshipPattern>,
                        std::unique_ptr<NodePattern>>>
      chain;
  void Validate() const { assert(node_pattern); }
  void Accept(ASTVisitor& visitor) override;
};

class RelationshipsPattern : public ASTNode {
 public:
  RelationshipsPattern() {
    node_type = ASTNodeType::kRelationshipsPattern;
    category = ASTNodeCategory::kPattern;
  }
  std::unique_ptr<NodePattern> node_pattern;
  std::vector<std::pair<std::unique_ptr<RelationshipPattern>,
                        std::unique_ptr<NodePattern>>>
      chain;  // must be non-empty to satisfy grammar
  void Validate() const {
    assert(node_pattern);
    assert(!chain.empty());
  }
  void Accept(ASTVisitor& visitor) override;
};

class NodePattern : public ASTNode {
 public:
  NodePattern() {
    node_type = ASTNodeType::kNodePattern;
    category = ASTNodeCategory::kPattern;
  }
  std::string variable;
  std::vector<std::string> labels;
  std::unique_ptr<Properties> properties;
  void Accept(ASTVisitor& visitor) override;
};

class RelationshipPattern : public ASTNode {
 public:
  RelationshipPattern() {
    node_type = ASTNodeType::kRelationshipPattern;
    category = ASTNodeCategory::kPattern;
  }
  bool left_arrow = false;
  bool right_arrow = false;
  std::unique_ptr<RelationshipDetail> detail;
  void Accept(ASTVisitor& visitor) override;
};

class RelationshipDetail : public ASTNode {
 public:
  RelationshipDetail() {
    node_type = ASTNodeType::kRelationshipDetail;
    category = ASTNodeCategory::kPattern;
  }
  struct RangeLiteral {
    std::optional<int> min;
    std::optional<int> max;
  };
  std::string variable;
  std::vector<std::string> types;
  std::optional<RangeLiteral> range;  // *min..max
  std::unique_ptr<Properties> properties;
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// Clauses
// ============================================
class Clause : public ASTNode {
 protected:
  Clause() { category = ASTNodeCategory::kClause; }
};

// Reading clauses
class ReadingClause : public Clause {
 protected:
  ReadingClause() = default;
};

class Match : public ReadingClause {
 public:
  Match() { node_type = ASTNodeType::kMatch; }
  bool optional_match = false;
  std::unique_ptr<Pattern> pattern;
  std::unique_ptr<Expression> where;
  void Accept(ASTVisitor& visitor) override;
};

class Unwind : public ReadingClause {
 public:
  Unwind() { node_type = ASTNodeType::kUnwind; }
  std::unique_ptr<Expression> expression;
  std::string variable;
  void Accept(ASTVisitor& visitor) override;
};

class InQueryCall : public ReadingClause {
 public:
  InQueryCall() { node_type = ASTNodeType::kInQueryCall; }
  std::string procedure_name;
  std::vector<std::unique_ptr<Expression>> arguments;
  std::vector<StandaloneCall::YieldItem> yield_items;
  std::unique_ptr<Expression> yield_where;
  void Validate() const { assert(!yield_where || !yield_items.empty()); }
  void Accept(ASTVisitor& visitor) override;
};

// Updating clauses
class UpdatingClause : public Clause {
 protected:
  UpdatingClause() = default;
};

class Create : public UpdatingClause {
 public:
  Create() { node_type = ASTNodeType::kCreate; }
  std::unique_ptr<Pattern> pattern;
  void Accept(ASTVisitor& visitor) override;
};

class Merge : public UpdatingClause {
 public:
  Merge() { node_type = ASTNodeType::kMerge; }
  std::unique_ptr<PatternPart> pattern_part;
  std::vector<std::pair<bool, std::unique_ptr<Set>>>
      actions;  // bool: true=ON MATCH, false=ON CREATE
  void Accept(ASTVisitor& visitor) override;
};

class Delete : public UpdatingClause {
 public:
  Delete() { node_type = ASTNodeType::kDelete; }
  bool detach = false;
  std::vector<std::unique_ptr<Expression>> expressions;
  void Accept(ASTVisitor& visitor) override;
};

class SetItem : public ASTNode {
 public:
  SetItem() { node_type = ASTNodeType::kSetItem; }
  enum class Type { kProperty, kVariable, kLabels };
  Type type;
  std::unique_ptr<Expression> target;
  std::unique_ptr<Expression> value;
  std::vector<std::string> labels;
  bool plus_equal = false;
  void Validate() const {
    assert(target);
    switch (type) {
      case Type::kProperty:
        assert(target->Is(ASTNodeType::kPropertyExpression));
        assert(value);
        assert(!plus_equal);
        assert(labels.empty());
        break;
      case Type::kVariable:
        assert(target->Is(ASTNodeType::kVariable));
        assert(value);
        assert(labels.empty());
        break;
      case Type::kLabels:
        assert(target->Is(ASTNodeType::kVariable));
        assert(!value);
        assert(!labels.empty());
        assert(!plus_equal);
        break;
    }
    if (plus_equal) {
      assert(type == Type::kVariable);
    }
  }
  void Accept(ASTVisitor& visitor) override;
};

inline constexpr std::string_view ToString(SetItem::Type type) {
  constexpr auto kSetItemTypeNames = std::array{
      std::string_view{"Property"},
      std::string_view{"Variable"},
      std::string_view{"Labels"},
  };
  static_assert(kSetItemTypeNames.size() ==
                static_cast<std::size_t>(SetItem::Type::kLabels) + 1);
  const auto index = static_cast<std::size_t>(type);
  if (index >= kSetItemTypeNames.size()) {
    return "Unknown";
  }
  return kSetItemTypeNames[index];
}

inline std::ostream& operator<<(std::ostream& out, SetItem::Type type) {
  out << ToString(type);
  return out;
}

class Set : public UpdatingClause {
 public:
  Set() { node_type = ASTNodeType::kSet; }
  std::vector<std::unique_ptr<SetItem>> items;
  void Accept(ASTVisitor& visitor) override;
};

class RemoveItem : public ASTNode {
 public:
  RemoveItem() { node_type = ASTNodeType::kRemoveItem; }
  enum class Type { kProperty, kLabels };
  Type type;
  std::unique_ptr<Expression> target;
  std::vector<std::string> labels;
  void Validate() const {
    assert(target);
    switch (type) {
      case Type::kProperty:
        assert(target->Is(ASTNodeType::kPropertyExpression));
        assert(labels.empty());
        break;
      case Type::kLabels:
        assert(target->Is(ASTNodeType::kVariable));
        assert(!labels.empty());
        break;
    }
  }
  void Accept(ASTVisitor& visitor) override;
};

inline constexpr std::string_view ToString(RemoveItem::Type type) {
  constexpr auto kRemoveItemTypeNames = std::array{
      std::string_view{"Property"},
      std::string_view{"Labels"},
  };
  static_assert(kRemoveItemTypeNames.size() ==
                static_cast<std::size_t>(RemoveItem::Type::kLabels) + 1);
  const auto index = static_cast<std::size_t>(type);
  if (index >= kRemoveItemTypeNames.size()) {
    return "Unknown";
  }
  return kRemoveItemTypeNames[index];
}

inline std::ostream& operator<<(std::ostream& out, RemoveItem::Type type) {
  out << ToString(type);
  return out;
}

class Remove : public UpdatingClause {
 public:
  Remove() { node_type = ASTNodeType::kRemove; }
  std::vector<std::unique_ptr<RemoveItem>> items;
  void Accept(ASTVisitor& visitor) override;
};

// Projection clauses
class SortItem : public ASTNode {
 public:
  SortItem() { node_type = ASTNodeType::kSortItem; }
  std::unique_ptr<Expression> expression;
  bool ascending = true;
  void Accept(ASTVisitor& visitor) override;
};

class ProjectionItem : public ASTNode {
 public:
  ProjectionItem() { node_type = ASTNodeType::kProjectionItem; }
  std::unique_ptr<Expression> expression;
  std::string alias;
  void Accept(ASTVisitor& visitor) override;
};

class ProjectionBody : public ASTNode {
 public:
  ProjectionBody() { node_type = ASTNodeType::kProjectionBody; }
  bool distinct = false;
  bool star = false;
  bool empty_star_expansion = false;
  std::vector<std::unique_ptr<ProjectionItem>> items;
  std::vector<std::unique_ptr<SortItem>> order_by;
  std::unique_ptr<Expression> skip;
  std::unique_ptr<Expression> limit;
  void Validate() const {
    assert(star || empty_star_expansion || !items.empty());
  }
  void Accept(ASTVisitor& visitor) override;
};

class ProjectionClause : public Clause {
 public:
  std::unique_ptr<ProjectionBody> body;
};

class With : public ProjectionClause {
 public:
  With() { node_type = ASTNodeType::kWith; }
  std::unique_ptr<Expression> where;
  void Accept(ASTVisitor& visitor) override;
};

class Return : public ProjectionClause {
 public:
  Return() { node_type = ASTNodeType::kReturn; }
  void Accept(ASTVisitor& visitor) override;
};

// ============================================
// accept implementations
// ============================================
inline void RegularQuery::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void StandaloneCall::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void SinglePartQuery::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void MultiPartQuery::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void UnionPart::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }

inline void OrExpression::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void XorExpression::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void AndExpression::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void ComparisonExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void ComparisonChainExpression::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void AddExpression::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void SubtractExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void MultiplyExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void DivideExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void ModuloExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void PowerExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void NotExpression::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void UnaryPlusExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void UnaryMinusExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void StringPredicateExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void ListPredicateExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void LabelPredicateExpression::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void NullPredicateExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}

inline void BooleanLiteral::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void IntegerLiteral::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void DoubleLiteral::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void StringLiteral::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void NullLiteral::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void ListLiteral::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void MapLiteral::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Properties::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}

inline void Variable::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Parameter::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void PropertyExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void ListIndexExpression::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void ListSliceExpression::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void FunctionInvocation::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void CountStarExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void CaseExpression::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void ParenthesizedExpression::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void ListComprehension::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void PatternComprehension::Accept(ASTVisitor& visitor) {
  if (relationships_pattern) {
    relationships_pattern->Validate();
  }
  visitor.Visit(*this);
}
inline void PatternPredicateExpression::Accept(ASTVisitor& visitor) {
  if (relationships_pattern) {
    relationships_pattern->Validate();
  }
  visitor.Visit(*this);
}
inline void AllQuantifier::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void AnyQuantifier::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void NoneQuantifier::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void SingleQuantifier::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void ExistentialSubquery::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}

inline void Pattern::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void PatternPart::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void PatternElement::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void RelationshipsPattern::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void RelationshipPattern::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void NodePattern::Accept(ASTVisitor& visitor) {
  if (properties) {
    properties->Validate();
  }
  visitor.Visit(*this);
}
inline void RelationshipDetail::Accept(ASTVisitor& visitor) {
  if (properties) {
    properties->Validate();
  }
  visitor.Visit(*this);
}

inline void Match::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Unwind::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void InQueryCall::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void Create::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Merge::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Delete::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Set::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void SetItem::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void Remove::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void RemoveItem::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void ProjectionBody::Accept(ASTVisitor& visitor) {
  Validate();
  visitor.Visit(*this);
}
inline void ProjectionItem::Accept(ASTVisitor& visitor) {
  visitor.Visit(*this);
}
inline void SortItem::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void With::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }
inline void Return::Accept(ASTVisitor& visitor) { visitor.Visit(*this); }

}  // namespace ast
