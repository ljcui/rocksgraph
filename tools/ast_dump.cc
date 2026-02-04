#include <iostream>
#include <string>
#include <vector>

#include "gflags/gflags.h"
#include "spdlog/spdlog.h"

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
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

void printMinimalUsage() {
  const auto info = gflags::GetCommandLineFlagInfoOrDie("rewrite");
  std::cerr << gflags::ProgramUsage() << "\n\n"
            << "  -" << info.name << " (" << info.description
            << ") type: " << info.type << " default: " << info.default_value
            << "\n";
}

int main(int argc, char **argv) {
  using common::Exception;
  gflags::SetUsageMessage(
      "Usage:\n  ast_dump [--rewrite] [--] <cypher...>");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (argc <= 1) {
    spdlog::error("Missing cypher statement.");
    printMinimalUsage();
    return 1;
  }

  std::vector<std::string> parts;
  parts.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    parts.emplace_back(argv[i]);
  }
  std::string input = joinArgs(parts);

  try {
    auto statement = FLAGS_rewrite ? ast::parseCypherAndRewrite(input)
                                   : ast::parseCypher(input);
    ast::ASTPrinter printer(std::cout);
    printer.print(*statement);
    return 0;
  } catch (const ast::ParseError &e) {
    for (const auto &err : e.errors()) {
      spdlog::error("Parse error: {}", err);
    }
  } catch (const ast::SemanticError &e) {
    for (const auto &err : e.errors()) {
      spdlog::error("Semantic error: {}", err);
    }
  } catch (const Exception &e) {
    spdlog::error("Internal error: {}", e.what());
  }
  return 1;
}
