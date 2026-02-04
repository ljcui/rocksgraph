#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_printer.h"

namespace {

void printUsage(const char *prog) {
  std::cerr << "Usage: " << prog
            << " [--rewrite] [--] <cypher...>\n"
            << "       " << prog << " [--rewrite] < <file.cypher>\n";
}

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

}  // namespace

int main(int argc, char **argv) {
  bool rewrite = false;
  bool passthrough = false;
  std::vector<std::string> parts;
  parts.reserve(static_cast<size_t>(argc > 1 ? argc - 1 : 0));

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (!passthrough && arg == "--help") {
      printUsage(argv[0]);
      return 0;
    }
    if (!passthrough && arg == "--rewrite") {
      rewrite = true;
      continue;
    }
    if (!passthrough && arg == "--") {
      passthrough = true;
      continue;
    }
    parts.push_back(std::move(arg));
  }

  std::string input;
  if (!parts.empty()) {
    input = joinArgs(parts);
  } else {
    std::ostringstream oss;
    oss << std::cin.rdbuf();
    input = oss.str();
  }

  if (input.empty()) {
    printUsage(argv[0]);
    return 1;
  }

  ast::ParseResult result =
      rewrite ? ast::parseCypherAndRewrite(input) : ast::parseCypher(input);
  if (!result.errors.empty()) {
    for (const auto &err : result.errors) {
      std::cerr << "Parse error: " << err << "\n";
    }
    return 1;
  }
  if (!result.statement) {
    std::cerr << "Parse error: failed to build AST\n";
    return 1;
  }

  ast::ASTPrinter printer(std::cout);
  printer.print(*result.statement);
  return 0;
}
