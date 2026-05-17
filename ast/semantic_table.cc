#include "semantic_table.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast_const_walker.h"
#include "common/exception.h"
#include "expression_dependency.h"
#include "expression_to_string.h"

namespace ast {
namespace {

using TypeMap = std::unordered_map<std::string, SemanticVariableType>;

const std::unordered_set<std::string> &EmptyStringSet() {
  static const std::unordered_set<std::string> empty;
  return empty;
}

const std::vector<std::string> &EmptyStringVector() {
  static const std::vector<std::string> empty;
  return empty;
}

bool StringEquals(const std::string &value, std::string_view expected) {
  return std::string_view(value) == expected;
}

std::string LowerAscii(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool IsAggregateFunction(std::string_view function_name) {
  const std::string name = LowerAscii(function_name);
  static const std::unordered_set<std::string> aggregates = {
      "avg",
      "collect",
      "count",
      "max",
      "min",
      "percentilecont",
      "percentiledisc",
      "stdev",
      "stdevp",
      "sum",
  };
  return aggregates.contains(name);
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
    for (const auto &item : node.yield_items) {
      Define(item.variable, SemanticVariableType::kUnknown);
    }
    WalkMaybe(node.yield_where);
  }

  void Visit(const Match &node) override {
    if (node.pattern) {
      DefinePatternBindings(*node.pattern);
      WalkMaybe(node.pattern);
    }
    WalkMaybe(node.where);
  }

  void Visit(const Unwind &node) override {
    WalkMaybe(node.expression);
    Define(node.variable, SemanticVariableType::kUnknown);
  }

  void Visit(const InQueryCall &node) override {
    WalkList(node.arguments);
    for (const auto &item : node.yield_items) {
      Define(item.variable, SemanticVariableType::kUnknown);
    }
    WalkMaybe(node.yield_where);
  }

  void Visit(const Create &node) override {
    if (node.pattern) {
      DefinePatternBindings(*node.pattern);
      WalkMaybe(node.pattern);
    }
  }

  void Visit(const Merge &node) override {
    if (node.pattern_part) {
      DefinePatternBindings(*node.pattern_part);
      WalkMaybe(node.pattern_part);
    }
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
    WalkMaybe(node.where);
  }

  void Visit(const Return &node) override {
    CHECK(node.body != nullptr, common::InternalError, "RETURN body is null");
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

  void PushScope(const Scope &scope) { scope_stack_.push_back(scope); }

  void PopScope() {
    CHECK(!scope_stack_.empty(), common::InternalError,
          "semantic table scope stack is empty");
    scope_stack_.pop_back();
  }

  void ReplaceCurrentScope(const Scope &scope) { scope_stack_.back() = scope; }

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

  [[nodiscard]] std::optional<SemanticVariableType> Lookup(
      const std::string &name) const {
    for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
      const auto type = it->Find(name);
      if (type.has_value()) {
        return type;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] SemanticVariableType InferExpressionType(
      const Expression &expression) const {
    switch (expression.node_type) {
      case ASTNodeType::kVariable: {
        const auto &variable = CastAst<Variable>(expression);
        return Lookup(variable.name).value_or(SemanticVariableType::kUnknown);
      }
      case ASTNodeType::kListLiteral:
      case ASTNodeType::kListComprehension:
      case ASTNodeType::kPatternComprehension:
        return SemanticVariableType::kList;
      case ASTNodeType::kMapLiteral:
        return SemanticVariableType::kMap;
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

  Scope AnalyzeProjectionBody(const ProjectionBody &body,
                              const Scope &input_scope) {
    PushScope(input_scope);
    for (const auto &item : body.items) {
      if (item && item->expression) {
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
    WalkList(body.order_by);
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
  constexpr auto kNames = std::array{
      std::string_view{"Unknown"},      std::string_view{"Node"},
      std::string_view{"Relationship"}, std::string_view{"Path"},
      std::string_view{"Scalar"},       std::string_view{"List"},
      std::string_view{"Map"},
  };
  const auto index = static_cast<std::size_t>(type);
  if (index >= kNames.size()) {
    return "Unknown";
  }
  return kNames[index];
}

std::ostream &operator<<(std::ostream &out, SemanticVariableType type) {
  out << ToString(type);
  return out;
}

std::optional<SemanticVariableType> SemanticTable::VariableType(
    std::string_view name) const {
  for (const auto &entry : variable_types_) {
    if (StringEquals(entry.first, name)) {
      return entry.second;
    }
  }
  return std::nullopt;
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
