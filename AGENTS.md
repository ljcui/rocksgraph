# Repository Guidelines
## 项目背景
- 这是一个正在开发中的单机图数据库项目，编程语言是c++，查询语言使用opencypher，测试框架使用gtest。

## 项目结构与模块组织
- `ast/`：Cypher 语法（`Cypher.g4`）、生成的解析器/词法器源码，以及 AST 工具（builder、printer、rewriter、equality）。
- `tests/`：GoogleTest 单元测试（例如 `*_test.cc`）。
- `main.cc`：`rg-server` 可执行文件入口。
- `cmake/`：CMake 辅助模块（ANTLR4 runtime 探测）。
- `build/` 与 `cmake-build-debug/`：构建产物目录，应保持未跟踪。

## 构建、测试与开发命令
- 配置：`cmake -S . -B build`（生成构建文件）。
- 构建：`cmake --build build`（编译 `rg-server`）。
- 测试：`ctest --test-dir build`（在检测到 GTest 时运行测试）。

说明：
- 需要 ANTLR4 runtime（`find_package(ANTLR4Runtime REQUIRED)`）。
- 仅在检测到 GTest 时构建测试（默认 `-DBUILD_TESTS=ON`）。

## 编码风格与命名规范
- 采用 C++20（由 `CMakeLists.txt` 强制）。
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

## 回答问题规范
- 回答问题时一律使用中文。
