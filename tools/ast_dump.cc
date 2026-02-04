#include <iostream>
#include <string>
#include <vector>

#include "gflags/gflags.h"
#include "spdlog/spdlog.h"

#include "ast/ast_builder.h"
#include "ast/ast_printer.h"

DEFINE_bool(rewrite, false, "Rewrite the cypher statement before printing.");

std::string joinArgs(const std::vector<std::string> &parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += " ";
    }
    out += parts[i];
  }
  return out;
}

int main(int argc, char **argv) {
  gflags::SetUsageMessage(
      "Usage:\n  ast_dump [--rewrite] [--] <cypher...>");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (argc <= 1) {
    spdlog::error("Missing cypher statement.");
    gflags::ShowUsageWithFlags(argv[0]);
    return 1;
  }

  std::vector<std::string> parts;
  parts.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    parts.emplace_back(argv[i]);
  }
  std::string input = joinArgs(parts);

  ast::ParseResult result =
      FLAGS_rewrite ? ast::parseCypherAndRewrite(input) : ast::parseCypher(input);
  if (!result.errors.empty()) {
    for (const auto &err : result.errors) {
      spdlog::error("Parse error: {}", err);
    }
    return 1;
  }
  if (!result.statement) {
    spdlog::error("Parse error: failed to build AST");
    return 1;
  }

  ast::ASTPrinter printer(std::cout);
  printer.print(*result.statement);
  return 0;
}
