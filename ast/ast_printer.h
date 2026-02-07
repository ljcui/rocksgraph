#pragma once

#include <ostream>

#include "ast_node.h"

namespace ast {

class ASTPrinter : public ASTVisitor {
 public:
  explicit ASTPrinter(std::ostream &out);
  void Print(ASTNode &node);

#define AST_VISITOR_NODE(node_type) void Visit(node_type &node) override;
#include "ast_visitor_node_list.def"
#undef AST_VISITOR_NODE

 private:
  class IndentGuard {
   public:
    explicit IndentGuard(ASTPrinter &printer) : printer_(printer) {
      printer_.Indent();
    }

    ~IndentGuard() { printer_.Dedent(); }

    IndentGuard(const IndentGuard &) = delete;
    IndentGuard &operator=(const IndentGuard &) = delete;

   private:
    ASTPrinter &printer_;
  };

  void Line(const std::string &text);
  void LineNodeType(const ASTNode &node);
  void Indent();
  void Dedent();
  void PrintYieldItems(
      const std::vector<StandaloneCall::YieldItem> &yield_items);

  template <typename T>
  void VisitMaybe(const std::unique_ptr<T> &ptr) {
    if (ptr) {
      ptr->Accept(*this);
    }
  }

  template <typename T>
  void VisitList(const std::vector<std::unique_ptr<T>> &list) {
    for (const auto &item : list) {
      VisitMaybe(item);
    }
  }

  std::ostream &out_;
  int indent_ = 0;
};

}  // namespace ast
