#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "ast/ast_node.h"

TEST(ASTNodeTypeToStringTest, BasicNodeTypes) {
  EXPECT_EQ(ast::ToString(ast::ASTNodeType::kUnknown), "Unknown");
  EXPECT_EQ(ast::ToString(ast::ASTNodeType::kRegularQuery), "RegularQuery");
  EXPECT_EQ(ast::ToString(ast::ASTNodeType::kVariable), "Variable");
  EXPECT_EQ(ast::ToString(ast::ASTNodeType::kMatch), "Match");
  EXPECT_EQ(ast::ToString(ast::ASTNodeType::kReturn), "Return");
}

TEST(ASTNodeTypeToStringTest, SupportsOstreamOutput) {
  std::ostringstream out;
  out << ast::ASTNodeType::kFunctionInvocation;
  EXPECT_EQ(out.str(), "FunctionInvocation");
}

TEST(ASTNodeCategoryToStringTest, BasicCategories) {
  EXPECT_EQ(ast::ToString(ast::ASTNodeCategory::kUnknown), "Unknown");
  EXPECT_EQ(ast::ToString(ast::ASTNodeCategory::kExpression), "Expression");
  EXPECT_EQ(ast::ToString(ast::ASTNodeCategory::kClause), "Clause");
  EXPECT_EQ(ast::ToString(ast::ASTNodeCategory::kPattern), "Pattern");
}

TEST(ASTNodeCategoryToStringTest, SupportsOstreamOutput) {
  std::ostringstream out;
  out << ast::ASTNodeCategory::kClause;
  EXPECT_EQ(out.str(), "Clause");
}

TEST(SetItemTypeToStringTest, BasicTypes) {
  EXPECT_EQ(ast::ToString(ast::SetItem::Type::kProperty), "Property");
  EXPECT_EQ(ast::ToString(ast::SetItem::Type::kVariable), "Variable");
  EXPECT_EQ(ast::ToString(ast::SetItem::Type::kLabels), "Labels");
}

TEST(SetItemTypeToStringTest, SupportsOstreamOutput) {
  std::ostringstream out;
  out << ast::SetItem::Type::kVariable;
  EXPECT_EQ(out.str(), "Variable");
}

TEST(RemoveItemTypeToStringTest, BasicTypes) {
  EXPECT_EQ(ast::ToString(ast::RemoveItem::Type::kProperty), "Property");
  EXPECT_EQ(ast::ToString(ast::RemoveItem::Type::kLabels), "Labels");
}

TEST(RemoveItemTypeToStringTest, SupportsOstreamOutput) {
  std::ostringstream out;
  out << ast::RemoveItem::Type::kLabels;
  EXPECT_EQ(out.str(), "Labels");
}
