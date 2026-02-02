# Repository Guidelines
## 项目背景
- 这是一个正在开发中的单机图数据库项目，编程语言是c++，查询语言使用opencypher，测试框架使用gtest。

## 项目结构与模块组织
- `ast/`：Cypher 语法（`Cypher.g4`）、生成的解析器/词法器源码，以及 AST 工具（builder、printer、rewriter、equality）。
- `value/`：Value 值系统与数据类型定义（标量、集合、图元素、时空/空间类型）。
- `tests/`：GoogleTest 单元测试（例如 `*_test.cc`）。
- `main.cc`：`rg-server` 可执行文件入口。
- `cmake/`：CMake 辅助模块（ANTLR4 runtime 探测）。
- `build/` 与 `cmake-build-debug/`：构建产物目录，应保持未跟踪。

## 构建、测试与开发命令
- 配置：`cmake -S . -B build`（生成构建文件）。
- 构建：`cmake --build build -j8`（编译默认目标，包含 `rg-server` 与测试）。
- 测试：`ctest --test-dir build`（需先完成构建；运行全部测试）。
- 工具执行要求：执行构建命令时，`timeout_ms` 最多设置为 30000（30 秒）。

## 编码风格与命名规范
- 采用 C++20（由 `CMakeLists.txt` 强制）。
- 采用Google C++编程风格（由`.clang-format`强制）。
- 缩进 2 空格；大括号同行（K&R 风格）。
- 文件名使用 `snake_case`（如 `ast_rewriter.cc`），测试文件使用 `*_test.cc`。
- 函数与类型命名简短且语义明确（如 `ReturnStarRewriter`）。

## 测试规范
- 测试框架：GoogleTest。
- 位置：新增测试放在 `tests/` 下，命名 `*_test.cc`。
- 建议针对单一 AST 功能或重写行为编写聚焦测试；当前无覆盖率门槛。

## 生成代码与语法变更
- `ast/Cypher*.cc/.h` 由 `ast/Cypher.g4` 生成。
- 避免手工修改生成文件；语法变更后用本地 ANTLR 流程重新生成。

## Git 忽略项
- `build/`、`cmake-build-debug/`、`.idea/` 应保持忽略与未跟踪。

## 提交规范
- Git commit 提交信息必须使用英文。

## 回答问题规范
- 回答问题时一律使用中文。
