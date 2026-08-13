#include "semantic_table.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast_const_walker.h"
#include "builtin_function.h"
#include "builtin_procedure.h"
#include "common/exception.h"
#include "expression_dependency.h"
#include "expression_to_string.h"

namespace ast {
namespace {

using TypeMap = std::unordered_map<std::string, SemanticVariableType>;

const std::unordered_set<std::string> &EmptyStringSet() {
  static const std::unordered_set<std::string> kEmpty;
  return kEmpty;
}

const std::vector<std::string> &EmptyStringVector() {
  static const std::vector<std::string> kEmpty;
  return kEmpty;
}

const TypeMap &EmptyTypeMap() {
  static const TypeMap kEmpty;
  return kEmpty;
}

bool StringEquals(const std::string &value, std::string_view expected) {
  return std::string_view(value) == expected;
}

std::optional<SemanticVariableType> LookupVariableType(const TypeMap &types,
                                                       std::string_view name) {
  for (const auto &entry : types) {
    if (StringEquals(entry.first, name)) {
      return entry.second;
    }
  }
  return std::nullopt;
}

bool IsAggregateFunction(std::string_view function_name) {
  const BuiltinFunction *function = FindBuiltinFunction(function_name);
  return function != nullptr && function->aggregate;
}

std::optional<SemanticVariableType> LookupProcedureYieldType(
    std::string_view procedure_name, std::string_view field_name) {
  const BuiltinProcedure *procedure = FindBuiltinProcedure(procedure_name);
  if (procedure == nullptr) {
    return std::nullopt;
  }
  const BuiltinProcedureYield *yield =
      FindBuiltinProcedureYield(*procedure, field_name);
  return yield != nullptr ? std::optional(yield->type) : std::nullopt;
}

std::vector<std::string> LookupProcedureYieldFields(
    std::string_view procedure_name) {
  const BuiltinProcedure *procedure = FindBuiltinProcedure(procedure_name);
  if (procedure == nullptr) {
    return {};
  }
  std::vector<std::string> fields;
  fields.reserve(procedure->yields.size());
  for (const auto &yield : procedure->yields) {
    fields.push_back(yield.name);
  }
  return fields;
}

std::optional<bool> LookupProcedureReadOnly(std::string_view procedure_name) {
  const BuiltinProcedure *procedure = FindBuiltinProcedure(procedure_name);
  if (procedure == nullptr) {
    return std::nullopt;
  }
  return procedure->read_only;
}

bool IsAggregationExpression(const Expression &expression) {
  class AggregationScanner final : public ASTConstWalker {
   public:
    bool Scan(const Expression &expression) {
      contains_ = false;
      expression.Accept(*this);
      return contains_;
    }

   protected:
    void Visit(const FunctionInvocation &node) override {
      if (IsAggregateFunction(node.function_name)) {
        contains_ = true;
      }
      ASTConstWalker::Visit(node);
    }

    void Visit(const CountStarExpression &node) override {
      (void)node;
      contains_ = true;
    }

   private:
    bool contains_ = false;
  };

  AggregationScanner scanner;
  return scanner.Scan(expression);
}

std::string ProjectionItemName(const ProjectionItem &item) {
  if (!item.alias.empty()) {
    return item.alias;
  }
  CHECK(item.expression != nullptr, common::InternalError,
        "projection item expression is null");
  std::string name = ExpressionToString(*item.expression);
  CHECK(!name.empty(), common::InternalError,
        "projection item name stringify failed");
  return name;
}

}  // namespace

class SemanticTableAnalyzer final : public ASTConstWalker {
 public:
  SemanticTable Analyze(const ASTNode &node) {
    table_ = SemanticTable();
    scope_stack_.clear();
    scope_stack_.emplace_back();
    Walk(node);
    scope_stack_.clear();
    return std::move(table_);
  }

 protected:
  void WalkExpression(const std::unique_ptr<Expression> &expr) override {
    if (!expr) {
      return;
    }
    RecordScope(*expr);
    table_.RecordExpressionDependencies(
        *expr, CollectExpressionDependencies(*expr, CurrentScopeSymbols()));
    table_.RecordAggregation(*expr, IsAggregationExpression(*expr));
    ASTConstWalker::WalkExpression(expr);
  }

  void Visit(const RegularQuery &node) override {
    const Scope base = CurrentScope();
    if (node.single_query) {
      PushScope(base);
      WalkMaybe(node.single_query);
      PopScope();
    }
    for (const auto &part : node.unions) {
      if (!part || !part->query) {
        continue;
      }
      PushScope(base);
      WalkMaybe(part->query);
      PopScope();
    }
  }

  void Visit(const SinglePartQuery &node) override {
    WalkList(node.reading_clauses);
    WalkList(node.updating_clauses);
    WalkMaybe(node.return_clause);
  }

  void Visit(const MultiPartQuery &node) override {
    for (const auto &part : node.parts) {
      WalkList(part.reading_clauses);
      WalkList(part.updating_clauses);
      WalkMaybe(part.with_clause);
    }
    WalkMaybe(node.final_single_part_query);
  }

  void Visit(const StandaloneCall &node) override {
    WalkList(node.arguments);
    if (node.yield_star || node.yield_items.empty()) {
      DefineProcedureYieldStar(node.procedure_name);
    } else {
      for (const auto &item : node.yield_items) {
        DefineProcedureYieldItem(node.procedure_name, item);
      }
    }
    RecordScope(node);
    WalkMaybe(node.yield_where);
  }

  void Visit(const Match &node) override {
    if (node.pattern) {
      DefinePatternBindings(*node.pattern);
      WalkMaybe(node.pattern);
    }
    RecordScope(node);
    WalkMaybe(node.where);
  }

  void Visit(const Unwind &node) override {
    WalkMaybe(node.expression);
    Define(node.variable, SemanticVariableType::kUnknown);
    RecordScope(node);
  }

  void Visit(const InQueryCall &node) override {
    WalkList(node.arguments);
    for (const auto &item : node.yield_items) {
      DefineProcedureYieldItem(node.procedure_name, item);
    }
    RecordScope(node);
    WalkMaybe(node.yield_where);
  }

  void Visit(const Create &node) override {
    if (node.pattern) {
      DefinePatternBindings(*node.pattern);
      WalkMaybe(node.pattern);
    }
    RecordScope(node);
  }

  void Visit(const Merge &node) override {
    if (node.pattern_part) {
      DefinePatternBindings(*node.pattern_part);
      WalkMaybe(node.pattern_part);
    }
    RecordScope(node);
    for (const auto &action : node.actions) {
      WalkMaybe(action.second);
    }
  }

  void Visit(const ProjectionBody &node) override {
    AnalyzeProjectionBody(node, CurrentScope());
  }

  void Visit(const With &node) override {
    CHECK(node.body != nullptr, common::InternalError, "WITH body is null");
    const Scope pre = CurrentScope();
    const Scope projected = AnalyzeProjectionBody(*node.body, pre);
    ReplaceCurrentScope(projected);
    RecordScope(node);
    WalkMaybe(node.where);
  }

  void Visit(const Return &node) override {
    CHECK(node.body != nullptr, common::InternalError, "RETURN body is null");
    RecordScope(node);
    AnalyzeProjectionBody(*node.body, CurrentScope());
  }

  void Visit(const ListComprehension &node) override {
    WalkMaybe(node.list_expr);
    PushScope(CurrentScope());
    Define(node.variable, SemanticVariableType::kUnknown);
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(const PatternComprehension &node) override {
    PushScope(CurrentScope());
    Define(node.variable, SemanticVariableType::kPath);
    if (node.relationships_pattern) {
      DefinePatternBindings(*node.relationships_pattern);
      WalkMaybe(node.relationships_pattern);
    }
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(const PatternPredicateExpression &node) override {
    PushScope(CurrentScope());
    if (node.relationships_pattern) {
      DefinePatternBindings(*node.relationships_pattern);
      WalkMaybe(node.relationships_pattern);
    }
    PopScope();
  }

  void Visit(const AllQuantifier &node) override { VisitQuantifier(node); }
  void Visit(const AnyQuantifier &node) override { VisitQuantifier(node); }
  void Visit(const NoneQuantifier &node) override { VisitQuantifier(node); }
  void Visit(const SingleQuantifier &node) override { VisitQuantifier(node); }

  void Visit(const ExistentialSubquery &node) override {
    PushScope(CurrentScope());
    if (node.query) {
      WalkMaybe(node.query);
    } else {
      if (node.pattern) {
        DefinePatternBindings(*node.pattern);
        WalkMaybe(node.pattern);
      }
      WalkMaybe(node.where_expr);
    }
    PopScope();
  }

 private:
  struct Scope {
    TypeMap types;

    void Set(const std::string &name, SemanticVariableType type) {
      if (name.empty()) {
        return;
      }
      const auto it = types.find(name);
      if (it == types.end() || it->second == type) {
        types[name] = type;
        return;
      }
      types[name] = SemanticVariableType::kUnknown;
    }

    [[nodiscard]] std::optional<SemanticVariableType> Find(
        const std::string &name) const {
      const auto it = types.find(name);
      if (it == types.end()) {
        return std::nullopt;
      }
      return it->second;
    }
  };

  void VisitQuantifier(const Quantifier &node) {
    WalkMaybe(node.list_expr);
    PushScope(CurrentScope());
    Define(node.variable, SemanticVariableType::kUnknown);
    WalkMaybe(node.predicate);
    PopScope();
  }

  Scope &CurrentScope() { return scope_stack_.back(); }
  [[nodiscard]] const Scope &CurrentScope() const {
    return scope_stack_.back();
  }

  void PushScope(Scope scope) { scope_stack_.push_back(std::move(scope)); }

  void PopScope() {
    CHECK(!scope_stack_.empty(), common::InternalError,
          "semantic table scope stack is empty");
    scope_stack_.pop_back();
  }

  void ReplaceCurrentScope(const Scope &scope) { scope_stack_.back() = scope; }

  void RecordScope(const ASTNode &node) {
    table_.RecordVariableTypes(node, CurrentScope().types);
  }

  std::unordered_set<std::string> CurrentScopeSymbols() const {
    std::unordered_set<std::string> symbols;
    for (const auto &entry : CurrentScope().types) {
      symbols.insert(entry.first);
    }
    return symbols;
  }

  void Define(const std::string &name, SemanticVariableType type) {
    if (name.empty()) {
      return;
    }
    CurrentScope().Set(name, type);
    table_.RecordVariableType(name, type);
  }

  void DefineProcedureYieldItem(std::string_view procedure_name,
                                const StandaloneCall::YieldItem &yield_item) {
    const std::string_view field_name =
        yield_item.result_field.has_value()
            ? std::string_view(*yield_item.result_field)
            : std::string_view(yield_item.variable);
    const SemanticVariableType type =
        LookupProcedureYieldType(procedure_name, field_name)
            .value_or(SemanticVariableType::kUnknown);
    Define(yield_item.variable, type);
  }

  void DefineProcedureYieldStar(std::string_view procedure_name) {
    const BuiltinProcedure *procedure = FindBuiltinProcedure(procedure_name);
    if (procedure == nullptr) {
      return;
    }
    for (const auto &yield : procedure->yields) {
      Define(yield.name, yield.type);
    }
  }

  [[nodiscard]] std::optional<SemanticVariableType> Lookup(
      const std::string &name) const {
    for (const auto &scope : std::ranges::reverse_view(scope_stack_)) {
      const auto type = scope.Find(name);
      if (type.has_value()) {
        return type;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] SemanticVariableType MergeExpressionTypes(
      const std::vector<SemanticVariableType> &types) const {
    if (types.empty()) {
      return SemanticVariableType::kUnknown;
    }
    SemanticVariableType result = types.front();
    for (SemanticVariableType type : types) {
      if (type != result) {
        return SemanticVariableType::kUnknown;
      }
    }
    return result;
  }

  [[nodiscard]] SemanticVariableType InferListElementType(
      const Expression &expression) const {
    switch (expression.node_type) {
      case ASTNodeType::kParenthesizedExpression: {
        const auto &parenthesized =
            CastAst<ParenthesizedExpression>(expression);
        if (parenthesized.expr) {
          return InferListElementType(*parenthesized.expr);
        }
        return SemanticVariableType::kUnknown;
      }
      case ASTNodeType::kListLiteral: {
        const auto &list = CastAst<ListLiteral>(expression);
        std::vector<SemanticVariableType> element_types;
        element_types.reserve(list.elements.size());
        for (const auto &element : list.elements) {
          if (element) {
            element_types.push_back(InferExpressionType(*element));
          }
        }
        return MergeExpressionTypes(element_types);
      }
      case ASTNodeType::kListComprehension: {
        const auto &comprehension = CastAst<ListComprehension>(expression);
        if (comprehension.eval_expr) {
          return InferExpressionType(*comprehension.eval_expr);
        }
        return SemanticVariableType::kUnknown;
      }
      case ASTNodeType::kPatternComprehension: {
        const auto &comprehension = CastAst<PatternComprehension>(expression);
        if (comprehension.eval_expr) {
          return InferExpressionType(*comprehension.eval_expr);
        }
        return SemanticVariableType::kUnknown;
      }
      case ASTNodeType::kFunctionInvocation: {
        const auto &function = CastAst<FunctionInvocation>(expression);
        const BuiltinFunction *builtin =
            FindBuiltinFunction(function.function_name);
        if (builtin != nullptr && builtin->list_element_type.has_value()) {
          return *builtin->list_element_type;
        }
        if (builtin != nullptr &&
            (builtin->kind == BuiltinFunctionKind::kTail ||
             builtin->kind == BuiltinFunctionKind::kReverse) &&
            function.arguments.size() == 1 && function.arguments[0]) {
          return InferListElementType(*function.arguments[0]);
        }
        if (builtin != nullptr &&
            builtin->kind == BuiltinFunctionKind::kCollect &&
            function.arguments.size() == 1 && function.arguments[0]) {
          return InferExpressionType(*function.arguments[0]);
        }
        return SemanticVariableType::kUnknown;
      }
      default:
        return SemanticVariableType::kUnknown;
    }
  }

  [[nodiscard]] SemanticVariableType InferExpressionType(
      const Expression &expression) const {
    switch (expression.node_type) {
      case ASTNodeType::kVariable: {
        const auto &variable = CastAst<Variable>(expression);
        return Lookup(variable.name).value_or(SemanticVariableType::kUnknown);
      }
      case ASTNodeType::kOrExpression:
      case ASTNodeType::kXorExpression:
      case ASTNodeType::kAndExpression:
      case ASTNodeType::kComparisonExpression:
      case ASTNodeType::kComparisonChainExpression:
      case ASTNodeType::kNotExpression:
      case ASTNodeType::kStringPredicateExpression:
      case ASTNodeType::kListPredicateExpression:
      case ASTNodeType::kLabelPredicateExpression:
      case ASTNodeType::kNullPredicateExpression:
      case ASTNodeType::kBooleanLiteral:
      case ASTNodeType::kPatternPredicateExpression:
      case ASTNodeType::kAllQuantifier:
      case ASTNodeType::kAnyQuantifier:
      case ASTNodeType::kNoneQuantifier:
      case ASTNodeType::kSingleQuantifier:
      case ASTNodeType::kExistentialSubquery:
        return SemanticVariableType::kScalar;
      case ASTNodeType::kListLiteral:
      case ASTNodeType::kListComprehension:
      case ASTNodeType::kPatternComprehension:
        return SemanticVariableType::kList;
      case ASTNodeType::kMapLiteral:
        return SemanticVariableType::kMap;
      case ASTNodeType::kListIndexExpression: {
        const auto &list_index = CastAst<ListIndexExpression>(expression);
        if (list_index.list) {
          return InferListElementType(*list_index.list);
        }
        return SemanticVariableType::kUnknown;
      }
      case ASTNodeType::kListSliceExpression:
        return SemanticVariableType::kList;
      case ASTNodeType::kFunctionInvocation: {
        const auto &function = CastAst<FunctionInvocation>(expression);
        const BuiltinFunction *builtin =
            FindBuiltinFunction(function.function_name);
        if (builtin == nullptr) {
          return SemanticVariableType::kUnknown;
        }
        if (function.arguments.size() == 1 && function.arguments[0]) {
          if (builtin->kind == BuiltinFunctionKind::kLast) {
            return InferListElementType(*function.arguments[0]);
          }
          if (builtin->kind == BuiltinFunctionKind::kReverse) {
            return InferExpressionType(*function.arguments[0]);
          }
        }
        return builtin->result_type;
      }
      case ASTNodeType::kCountStarExpression:
        return SemanticVariableType::kScalar;
      case ASTNodeType::kCaseExpression: {
        const auto &case_expression = CastAst<CaseExpression>(expression);
        std::vector<SemanticVariableType> result_types;
        result_types.reserve(case_expression.alternatives.size() + 1);
        for (const auto &alternative : case_expression.alternatives) {
          if (alternative.second) {
            result_types.push_back(InferExpressionType(*alternative.second));
          }
        }
        if (case_expression.else_expr) {
          result_types.push_back(
              InferExpressionType(*case_expression.else_expr));
        }
        return MergeExpressionTypes(result_types);
      }
      case ASTNodeType::kParenthesizedExpression: {
        const auto &parenthesized =
            CastAst<ParenthesizedExpression>(expression);
        if (parenthesized.expr) {
          return InferExpressionType(*parenthesized.expr);
        }
        return SemanticVariableType::kUnknown;
      }
      default:
        return SemanticVariableType::kScalar;
    }
  }

  Scope AnalyzeProjectionBody(const ProjectionBody &body, Scope input_scope) {
    PushScope(input_scope);
    RecordScope(body);
    for (const auto &item : body.items) {
      if (item && item->expression) {
        RecordScope(*item);
        WalkMaybe(item->expression);
      }
    }

    Scope projected;
    std::vector<std::string> outputs;
    if (body.star) {
      projected = input_scope;
      outputs.reserve(input_scope.types.size() + body.items.size());
      for (const auto &entry : input_scope.types) {
        outputs.push_back(entry.first);
      }
      std::sort(outputs.begin(), outputs.end());
    } else {
      outputs.reserve(body.items.size());
    }

    for (const auto &item : body.items) {
      if (!item || !item->expression) {
        continue;
      }
      const std::string name = ProjectionItemName(*item);
      const SemanticVariableType type = InferExpressionType(*item->expression);
      projected.Set(name, type);
      table_.RecordVariableType(name, type);
      outputs.push_back(name);
    }
    table_.RecordProjectionOutputs(body, std::move(outputs));

    Scope order_scope = input_scope;
    for (const auto &entry : projected.types) {
      order_scope.Set(entry.first, entry.second);
    }
    ReplaceCurrentScope(order_scope);
    for (const auto &item : body.order_by) {
      if (item) {
        RecordScope(*item);
        WalkMaybe(item->expression);
      }
    }
    WalkMaybe(body.skip);
    WalkMaybe(body.limit);
    PopScope();
    return projected;
  }

  void DefinePatternBindings(const Pattern &pattern) {
    for (const auto &part : pattern.parts) {
      if (part) {
        DefinePatternBindings(*part);
      }
    }
  }

  void DefinePatternBindings(const PatternPart &part) {
    Define(part.variable, SemanticVariableType::kPath);
    if (part.element) {
      DefinePatternBindings(*part.element);
    }
  }

  void DefinePatternBindings(const PatternElement &element) {
    if (element.node_pattern) {
      DefinePatternBindings(*element.node_pattern);
    }
    for (const auto &link : element.chain) {
      if (link.first) {
        DefinePatternBindings(*link.first);
      }
      if (link.second) {
        DefinePatternBindings(*link.second);
      }
    }
  }

  void DefinePatternBindings(const RelationshipsPattern &pattern) {
    if (pattern.node_pattern) {
      DefinePatternBindings(*pattern.node_pattern);
    }
    for (const auto &link : pattern.chain) {
      if (link.first) {
        DefinePatternBindings(*link.first);
      }
      if (link.second) {
        DefinePatternBindings(*link.second);
      }
    }
  }

  void DefinePatternBindings(const NodePattern &node) {
    Define(node.variable, SemanticVariableType::kNode);
  }

  void DefinePatternBindings(const RelationshipPattern &pattern) {
    if (pattern.detail) {
      DefinePatternBindings(*pattern.detail);
    }
  }

  void DefinePatternBindings(const RelationshipDetail &detail) {
    Define(detail.variable, SemanticVariableType::kRelationship);
  }

  SemanticTable table_;
  std::vector<Scope> scope_stack_;
};

std::string_view ToString(SemanticVariableType type) {
  constexpr auto k_names = std::array{
      std::string_view{"Unknown"},      std::string_view{"Node"},
      std::string_view{"Relationship"}, std::string_view{"Path"},
      std::string_view{"Scalar"},       std::string_view{"List"},
      std::string_view{"Map"},
  };
  const auto index = static_cast<std::size_t>(type);
  if (index >= k_names.size()) {
    return "Unknown";
  }
  return k_names[index];
}

std::ostream &operator<<(std::ostream &out, SemanticVariableType type) {
  out << ToString(type);
  return out;
}

std::optional<SemanticVariableType> SemanticTable::VariableType(
    std::string_view name) const {
  return LookupVariableType(variable_types_, name);
}

std::optional<SemanticVariableType> SemanticTable::VariableTypeAt(
    const ASTNode &node, std::string_view name) const {
  const auto it = scoped_variable_types_.find(&node);
  if (it == scoped_variable_types_.end()) {
    return std::nullopt;
  }
  return LookupVariableType(it->second, name);
}

const std::unordered_map<std::string, SemanticVariableType> &
SemanticTable::VariableTypesAt(const ASTNode &node) const {
  const auto it = scoped_variable_types_.find(&node);
  if (it == scoped_variable_types_.end()) {
    return EmptyTypeMap();
  }
  return it->second;
}

std::optional<SemanticVariableType> SemanticTable::KnownFunctionResultType(
    std::string_view function_name) const {
  const BuiltinFunction *function = FindBuiltinFunction(function_name);
  if (function == nullptr) {
    return std::nullopt;
  }
  return function->result_type;
}

std::optional<SemanticVariableType> SemanticTable::KnownProcedureYieldType(
    std::string_view procedure_name, std::string_view field_name) const {
  return LookupProcedureYieldType(procedure_name, field_name);
}

std::vector<std::string> SemanticTable::KnownProcedureYieldFields(
    std::string_view procedure_name) const {
  return LookupProcedureYieldFields(procedure_name);
}

std::optional<bool> SemanticTable::KnownProcedureReadOnly(
    std::string_view procedure_name) const {
  return LookupProcedureReadOnly(procedure_name);
}

const std::unordered_set<std::string> &SemanticTable::ExpressionDependencies(
    const Expression &expression) const {
  const auto it = expression_dependencies_.find(&expression);
  if (it == expression_dependencies_.end()) {
    return EmptyStringSet();
  }
  return it->second;
}

bool SemanticTable::ContainsAggregation(const Expression &expression) const {
  return aggregation_expressions_.contains(&expression);
}

const std::vector<std::string> &SemanticTable::ProjectionOutputs(
    const ProjectionBody &body) const {
  const auto it = projection_outputs_.find(&body);
  if (it == projection_outputs_.end()) {
    return EmptyStringVector();
  }
  return it->second;
}

void SemanticTable::RecordVariableType(std::string_view name,
                                       SemanticVariableType type) {
  if (name.empty()) {
    return;
  }
  const std::string key(name);
  const auto it = variable_types_.find(key);
  if (it == variable_types_.end() || it->second == type) {
    variable_types_[key] = type;
    return;
  }
  variable_types_[key] = SemanticVariableType::kUnknown;
}

void SemanticTable::RecordVariableTypes(
    const ASTNode &node,
    std::unordered_map<std::string, SemanticVariableType> types) {
  scoped_variable_types_[&node] = std::move(types);
}

void SemanticTable::RecordExpressionDependencies(
    const Expression &expression, std::unordered_set<std::string> symbols) {
  expression_dependencies_[&expression] = std::move(symbols);
}

void SemanticTable::RecordAggregation(const Expression &expression,
                                      bool contains) {
  if (contains) {
    aggregation_expressions_.insert(&expression);
    return;
  }
  aggregation_expressions_.erase(&expression);
}

void SemanticTable::RecordProjectionOutputs(const ProjectionBody &body,
                                            std::vector<std::string> outputs) {
  projection_outputs_[&body] = std::move(outputs);
}

SemanticTable AnalyzeSemanticTable(const ASTNode &node) {
  SemanticTableAnalyzer analyzer;
  return analyzer.Analyze(node);
}

}  // namespace ast
