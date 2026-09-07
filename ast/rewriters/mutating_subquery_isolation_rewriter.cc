#include "mutating_subquery_isolation_rewriter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../ast_const_walker.h"

namespace ast {
namespace {

class NameCollector final : public ASTConstWalker {
 public:
  std::unordered_set<std::string> Collect(const ASTNode &node) {
    names_.clear();
    node.Accept(*this);
    return std::move(names_);
  }

 protected:
  void Visit(const Variable &node) override { Add(node.name); }

  void Visit(const NodePattern &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const RelationshipDetail &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const PatternPart &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const ProjectionItem &node) override {
    Add(node.alias);
    ASTConstWalker::Visit(node);
  }

  void Visit(const Unwind &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const InQueryCall &node) override {
    for (const auto &item : node.yield_items) {
      Add(item.variable);
    }
    ASTConstWalker::Visit(node);
  }

  void Visit(const StandaloneCall &node) override {
    for (const auto &item : node.yield_items) {
      Add(item.variable);
    }
    ASTConstWalker::Visit(node);
  }

  void Visit(const ListComprehension &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const PatternComprehension &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const AllQuantifier &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const AnyQuantifier &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const NoneQuantifier &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

  void Visit(const SingleQuantifier &node) override {
    Add(node.variable);
    ASTConstWalker::Visit(node);
  }

 private:
  void Add(const std::string &name) {
    if (!name.empty()) {
      names_.insert(name);
    }
  }

  std::unordered_set<std::string> names_;
};

class SubqueryScanner final : public ASTConstWalker {
 public:
  bool Scan(const Expression &expression) {
    found_ = false;
    expression.Accept(*this);
    return found_;
  }

 protected:
  void Visit(const ExistentialSubquery &node) override {
    (void)node;
    found_ = true;
  }

  void Visit(const PatternComprehension &node) override {
    (void)node;
    found_ = true;
  }

 private:
  bool found_ = false;
};

void CollectCreatePropertyValues(
    PatternElement &element,
    std::vector<std::unique_ptr<Expression> *> *values) {
  const auto collect = [&](NodePattern *node) {
    if (node == nullptr || node->properties == nullptr ||
        node->properties->map == nullptr) {
      return;
    }
    for (auto &entry : node->properties->map->entries) {
      values->push_back(&entry.second);
    }
  };
  collect(element.node_pattern.get());
  for (auto &link : element.chain) {
    if (link.first != nullptr && link.first->detail != nullptr &&
        link.first->detail->properties != nullptr &&
        link.first->detail->properties->map != nullptr) {
      for (auto &entry : link.first->detail->properties->map->entries) {
        values->push_back(&entry.second);
      }
    }
    collect(link.second.get());
  }
}

std::vector<std::unique_ptr<Expression> *> UpdateExpressions(
    UpdatingClause &clause) {
  std::vector<std::unique_ptr<Expression> *> expressions;
  if (clause.Is(ASTNodeType::kCreate)) {
    auto &create = CastAst<Create>(clause);
    if (create.pattern != nullptr) {
      for (auto &part : create.pattern->parts) {
        if (part != nullptr && part->element != nullptr) {
          CollectCreatePropertyValues(*part->element, &expressions);
        }
      }
    }
    return expressions;
  }
  if (clause.Is(ASTNodeType::kSet)) {
    auto &set = CastAst<Set>(clause);
    for (auto &item : set.items) {
      if (item == nullptr) {
        continue;
      }
      if (item->type == SetItem::Type::kProperty && item->target != nullptr) {
        auto *property = CastAst<PropertyExpression>(item->target.get());
        if (property->object != nullptr) {
          expressions.push_back(&property->object);
        }
      }
      if (item->value != nullptr) {
        expressions.push_back(&item->value);
      }
    }
    return expressions;
  }
  if (clause.Is(ASTNodeType::kDelete)) {
    auto &del = CastAst<Delete>(clause);
    for (auto &expression : del.expressions) {
      expressions.push_back(&expression);
    }
    return expressions;
  }
  if (clause.Is(ASTNodeType::kRemove)) {
    auto &remove = CastAst<Remove>(clause);
    for (auto &item : remove.items) {
      if (item == nullptr || item->type != RemoveItem::Type::kProperty ||
          item->target == nullptr) {
        continue;
      }
      auto *property = CastAst<PropertyExpression>(item->target.get());
      if (property->object != nullptr) {
        expressions.push_back(&property->object);
      }
    }
  }
  return expressions;
}

bool ContainsSubquery(const Expression &expression) {
  SubqueryScanner scanner;
  return scanner.Scan(expression);
}

std::vector<std::unique_ptr<UpdatingClause>> NormalizeUpdatingClauses(
    std::vector<std::unique_ptr<UpdatingClause>> clauses) {
  std::vector<std::unique_ptr<UpdatingClause>> normalized;
  for (auto &clause : clauses) {
    if (clause == nullptr || !clause->Is(ASTNodeType::kSet)) {
      normalized.push_back(std::move(clause));
      continue;
    }

    auto *set = CastAst<Set>(clause.get());
    bool split = false;
    for (const auto &item : set->items) {
      if (item != nullptr &&
          ((item->target != nullptr && ContainsSubquery(*item->target)) ||
           (item->value != nullptr && ContainsSubquery(*item->value)))) {
        split = true;
        break;
      }
    }
    if (!split || set->items.size() <= 1) {
      normalized.push_back(std::move(clause));
      continue;
    }

    for (auto &item : set->items) {
      auto single = std::make_unique<Set>();
      single->items.push_back(std::move(item));
      normalized.push_back(std::move(single));
    }
  }
  return normalized;
}

std::unique_ptr<Variable> MakeVariable(const std::string &name) {
  auto variable = std::make_unique<Variable>();
  variable->name = name;
  return variable;
}

}  // namespace

void MutatingSubqueryIsolationRewriter::Visit(RegularQuery &node) {
  Prepare(node);
  RewriteSingleQuery(node.single_query);
  RewriteList(node.unions);
}

void MutatingSubqueryIsolationRewriter::Visit(UnionPart &node) {
  RewriteSingleQuery(node.query);
}

void MutatingSubqueryIsolationRewriter::Prepare(ASTNode &node) {
  if (prepared_) {
    return;
  }
  used_names_ = NameCollector{}.Collect(node);
  prepared_ = true;
}

void MutatingSubqueryIsolationRewriter::RewriteSingleQuery(
    std::unique_ptr<SingleQuery> &query) {
  if (query == nullptr) {
    return;
  }
  if (query->Is(ASTNodeType::kSinglePartQuery)) {
    query = RewriteSinglePart(std::unique_ptr<SinglePartQuery>(
        CastAst<SinglePartQuery>(query.release())));
    return;
  }
  if (query->Is(ASTNodeType::kMultiPartQuery)) {
    RewriteMultiPart(CastAst<MultiPartQuery>(*query));
  }
}

std::unique_ptr<SingleQuery>
MutatingSubqueryIsolationRewriter::RewriteSinglePart(
    std::unique_ptr<SinglePartQuery> query) {
  RewriteList(query->reading_clauses);
  RewriteMaybe(query->return_clause);

  std::vector<MultiPartQuery::WithPart> prefixes;
  std::vector<std::unique_ptr<UpdatingClause>> pending;
  auto clauses = NormalizeUpdatingClauses(std::move(query->updating_clauses));
  for (auto &clause : clauses) {
    if (clause == nullptr) {
      continue;
    }
    clause->Accept(*this);
    std::unique_ptr<With> isolated = IsolateExpressions(*clause);
    if (isolated != nullptr) {
      MultiPartQuery::WithPart prefix;
      if (prefixes.empty()) {
        prefix.reading_clauses = std::move(query->reading_clauses);
      }
      prefix.updating_clauses = std::move(pending);
      prefix.with_clause = std::move(isolated);
      prefixes.push_back(std::move(prefix));
    }
    pending.push_back(std::move(clause));
  }
  query->updating_clauses = std::move(pending);
  if (prefixes.empty()) {
    return query;
  }

  auto multi = std::make_unique<MultiPartQuery>();
  multi->parts = std::move(prefixes);
  multi->final_single_part_query = std::move(query);
  return multi;
}

void MutatingSubqueryIsolationRewriter::RewriteMultiPart(
    MultiPartQuery &query) {
  std::vector<MultiPartQuery::WithPart> rewritten_parts;
  for (auto &part : query.parts) {
    RewriteList(part.reading_clauses);
    RewriteMaybe(part.with_clause);

    std::vector<std::unique_ptr<UpdatingClause>> pending;
    bool first_segment = true;
    auto clauses = NormalizeUpdatingClauses(std::move(part.updating_clauses));
    for (auto &clause : clauses) {
      if (clause == nullptr) {
        continue;
      }
      clause->Accept(*this);
      std::unique_ptr<With> isolated = IsolateExpressions(*clause);
      if (isolated != nullptr) {
        MultiPartQuery::WithPart prefix;
        if (first_segment) {
          prefix.reading_clauses = std::move(part.reading_clauses);
          first_segment = false;
        }
        prefix.updating_clauses = std::move(pending);
        prefix.with_clause = std::move(isolated);
        rewritten_parts.push_back(std::move(prefix));
      }
      pending.push_back(std::move(clause));
    }
    part.updating_clauses = std::move(pending);
    rewritten_parts.push_back(std::move(part));
  }
  query.parts = std::move(rewritten_parts);

  if (query.final_single_part_query == nullptr) {
    return;
  }
  std::unique_ptr<SingleQuery> rewritten_final =
      RewriteSinglePart(std::move(query.final_single_part_query));
  if (rewritten_final->Is(ASTNodeType::kSinglePartQuery)) {
    query.final_single_part_query = std::unique_ptr<SinglePartQuery>(
        CastAst<SinglePartQuery>(rewritten_final.release()));
    return;
  }
  auto tail = std::unique_ptr<MultiPartQuery>(
      CastAst<MultiPartQuery>(rewritten_final.release()));
  for (auto &part : tail->parts) {
    query.parts.push_back(std::move(part));
  }
  query.final_single_part_query = std::move(tail->final_single_part_query);
}

std::unique_ptr<With> MutatingSubqueryIsolationRewriter::IsolateExpressions(
    UpdatingClause &clause) {
  auto with = std::make_unique<With>();
  with->body = std::make_unique<ProjectionBody>();
  with->body->star = true;

  SubqueryScanner scanner;
  for (std::unique_ptr<Expression> *expression : UpdateExpressions(clause)) {
    if (expression == nullptr || *expression == nullptr ||
        !scanner.Scan(**expression)) {
      continue;
    }
    const std::string alias = NextAlias();
    auto item = std::make_unique<ProjectionItem>();
    item->expression = std::move(*expression);
    item->alias = alias;
    with->body->items.push_back(std::move(item));
    *expression = MakeVariable(alias);
  }
  return with->body->items.empty() ? nullptr : std::move(with);
}

std::string MutatingSubqueryIsolationRewriter::NextAlias() {
  for (;;) {
    std::string alias = "__update_expr_" + std::to_string(next_alias_id_++);
    if (used_names_.insert(alias).second) {
      return alias;
    }
  }
}

}  // namespace ast
