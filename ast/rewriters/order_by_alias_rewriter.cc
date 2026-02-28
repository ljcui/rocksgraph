#include "order_by_alias_rewriter.h"

#include <string>
#include <utility>
#include <vector>

#include "../ast_equal.h"

namespace ast {

void OrderByAliasRewriter::Visit(ProjectionBody &node) {
  ASTRewriter::Visit(node);
  if (node.items.empty() || node.order_by.empty()) {
    return;
  }

  struct AliasEntry {
    const std::string *alias;
    const Expression *expression;
  };

  std::vector<AliasEntry> aliases;
  aliases.reserve(node.items.size());
  for (const auto &item : node.items) {
    if (!item || item->alias.empty() || !item->expression) {
      continue;
    }
    aliases.push_back(AliasEntry{&item->alias, item->expression.get()});
  }

  if (aliases.empty()) {
    return;
  }

  for (const auto &sort_item : node.order_by) {
    if (!sort_item || !sort_item->expression) {
      continue;
    }
    if (sort_item->expression->Is(ASTNodeType::kVariable)) {
      const auto *var =
          static_cast<const Variable *>(sort_item->expression.get());
      bool matches_alias = false;
      for (const auto &alias : aliases) {
        if (var->name == *alias.alias) {
          matches_alias = true;
          break;
        }
      }
      if (matches_alias) {
        continue;
      }
    }

    for (const auto &alias : aliases) {
      if (ASTEqual::Equal(sort_item->expression.get(), alias.expression)) {
        auto var = std::make_unique<Variable>();
        var->name = *alias.alias;
        sort_item->expression = std::move(var);
        break;
      }
    }
  }
}

}  // namespace ast
