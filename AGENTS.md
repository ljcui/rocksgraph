# 回答语言
- 请总是使用中文回答。
# 项目信息
- 这是一个正在开发中的单机图数据库项目，编程语言是c++，查询语言是opencypher，测试框架使用gtest。
- 目前只完成了AST的部分功能
# AST抽象语法树
- 代码在ast目录下
- Cypher.g4 是 OpenCypher的g4语法文件。
- CypherLexer.cpp,CypherParser.cpp,CypherVisitor.cpp是使用antlr4工具根据Cypher.g4生成的c++文件。
- ast.h里面是AST类的实现代码，实现逻辑需要严格参考Cypher.g4里面的内容。
- ast_builder.cpp里面是构建AST的相关代码。
- ast_printer.h 里面是打印AST的相关代码。