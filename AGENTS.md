# Repository Guidelines

## 项目背景
- 这是一个正在开发中的单机图数据库项目，编程语言为 C++20。
- 当前代码重心在 OpenCypher 前端：解析、AST 语义校验、重写、序列化，以及查询 IR 构建。
- 主要依赖：ANTLR4 Runtime、GoogleTest、gflags、spdlog。
- `rg-server` 入口（`main.cc`）目前是占位实现；可用命令行工具是 `ast_dump`。

## 项目结构与模块组织
- `ast/`：Cypher 前端核心。
  - `ast/cypher/Cypher.g4` 与生成文件 `Cypher*.cc/.h`。
  - `ast_builder.*`：解析入口（`ParseCypher` / `ParseCypherAndRewrite`）。
  - `semantic_validator.*`：变量作用域与语义校验。
  - `rewriters/`：默认重写流水线与各类 AST Rewriter。
  - `ast_to_cypher.*`、`expression_to_string.*`：表达式/AST 转回 Cypher 文本。
  - `ast_clone.*`、`ast_equal.*`、`ast_walker.*`、`ast_printer.*`：AST 工具层。
- `ir/`：查询中间表示（`QueryIR`）构建逻辑，当前以读查询规划为主。
- `value/`：统一 Value 类型系统（标量、容器、图元素、时空/空间类型）。
- `common/exception.h`：通用异常定义与 `THROW/CHECK` 宏。
- `tools/ast_dump.cc`：调试工具，可打印 AST，支持 `--rewrite`。
- `tests/`：GoogleTest 测试（AST 重写、AST→Cypher、语义校验、IR、Value）。
- `cmake/`：CMake 辅助模块（ANTLR4 runtime 探测）。

## 构建、测试与开发命令
- 配置：`cmake -S . -B build`
- 全量构建：`cmake --build build -j8`
- 运行全部测试：`ctest --test-dir build`
- 运行单测子集：`ctest --test-dir build -R planner_query_test`
- 代码格式化：`cmake --build build --target format`
- 静态检查：`cmake --build build --target clang_tidy`
- 工具执行要求：执行构建命令时，`timeout_ms` 最多设置为 30000（30 秒）。

## 编码风格与命名规范
- 使用 C++20（`CMakeLists.txt` 强制）。
- 使用 Google 风格（`.clang-format`，2 空格缩进，K&R 大括号风格）。
- 文件名采用 `snake_case`，测试文件采用 `*_test.cc`。
- 类型/函数命名语义化，避免不必要缩写。
- 禁止手改生成代码（`ast/cypher/Cypher*.cc/.h`）。

## 前端与 IR 开发约定
- 解析入口：
  - `ast::ParseCypher`：解析 + 语义校验。
  - `ast::ParseCypherAndRewrite`：解析 + 语义校验 + 默认重写流水线。
- 新增重写规则时：
  - 在 `ast/rewriters/` 增加实现；
  - 在 `rewriter_registry.cc` 明确顺序（顺序有语义依赖）；
  - 在 `tests/ast_rewriter_test.cc` 增加正反用例。
- `ir::BuildStatement` 当前只覆盖部分语义（以 RegularQuery/只读场景为主）；新增 IR 能力时需同步扩展异常与测试断言。
- 异常分层：通用异常放 `common/exception.h`；AST 解析/语义异常放 `ast/ast_exception.h`。

## 测试规范
- 新增测试放在 `tests/`，命名为 `*_test.cc`。
- AST 重写测试优先使用“输入 Cypher / 期望 Cypher”并通过 `ASTEqual` 比较。
- IR 测试需同时覆盖结构字段断言与错误路径（`EXPECT_THROW`）。
- 当前无覆盖率门槛，但要求新增功能有对应单测。

## 生成代码与语法变更
- `ast/cypher/Cypher*.cc/.h` 由 `ast/cypher/Cypher.g4` 生成（当前生成器版本见文件头注释）。
- 修改语法后，使用本地 ANTLR C++ 流程重新生成，并提交生成结果。
- `ast/cypher/` 下生成文件默认不参与 clang-tidy/format。

## Git 与提交规范
- `build/`、`cmake-build-debug/`、`.idea/` 保持忽略且不纳入提交。
- 仓库提供 `.githooks/pre-commit` 自动格式化暂存区 C/C++ 文件；建议启用：`git config core.hooksPath .githooks`。
- Git commit message 必须使用英文。

## 回答问题规范
- 回答问题时一律使用中文。
