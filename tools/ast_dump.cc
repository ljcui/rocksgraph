#include <iostream>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "ast/ast_printer.h"
#include "gflags/gflags.h"
#include "spdlog/spdlog.h"

DEFINE_bool(rewrite, false, "Rewrite the cypher statement before printing.");

std::string JoinArgs(const std::vector<std::string> &parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += " ";
    }
    out += parts[i];
  }
  return out;
}

void PrintMinimalUsage() {
  const auto info = gflags::GetCommandLineFlagInfoOrDie("rewrite");
  std::cerr << gflags::ProgramUsage() << "\n\n"
            << "  -" << info.name << " (" << info.description
            << ") type: " << info.type << " default: " << info.default_value
            << "\n";
}

int main(int argc, char **argv) {
  using common::Exception;
  gflags::SetUsageMessage("Usage:\n  ast_dump [--rewrite] [--] <cypher...>");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (argc <= 1) {
    spdlog::error("Missing cypher statement.");
    PrintMinimalUsage();
    return 1;
  }

  std::vector<std::string> parts;
  parts.reserve(static_cast<size_t>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    parts.emplace_back(argv[i]);
  }
  std::string input = JoinArgs(parts);

  try {
    auto statement = FLAGS_rewrite ? ast::ParseCypherAndRewrite(input)
                                   : ast::ParseCypher(input);
    ast::ASTPrinter printer(std::cout);
    printer.Print(*statement);
    return 0;
  } catch (const ast::ParseError &e) {
    for (const auto &err : e.Errors()) {
      spdlog::error("Parse error: {}", err);
    }
  } catch (const ast::SemanticError &e) {
    for (const auto &err : e.Errors()) {
      spdlog::error("Semantic error: {}", err);
    }
  } catch (const Exception &e) {
    spdlog::error("Internal error: {}", e.what());
  }
  return 1;
}
