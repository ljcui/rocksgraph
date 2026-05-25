#include <iostream>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "ast/ast_printer.h"
#include "gflags/gflags.h"
#include "ir/logical_plan_builder.h"
#include "ir/logical_plan_printer.h"
#include "ir/planner_query.h"
#include "ir/planner_query_printer.h"
#include "spdlog/spdlog.h"

DEFINE_string(mode, "ast", "Dump mode: ast, planner_query, or logical_plan.");
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
  std::cerr << gflags::ProgramUsage() << "\n\n";
  const std::vector<std::string> flag_names = {"mode", "rewrite"};
  for (const std::string &name : flag_names) {
    const auto info = gflags::GetCommandLineFlagInfoOrDie(name.c_str());
    std::cerr << "  -" << info.name << " (" << info.description
              << ") type: " << info.type << " default: " << info.default_value
              << "\n";
  }
}

int main(int argc, char **argv) {
  using common::Exception;
  gflags::SetUsageMessage(
      "Usage:\n  cypher_dump [--mode=ast|planner_query|logical_plan] "
      "[--rewrite] [--] <cypher...>");
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
    if (FLAGS_mode == "ast") {
      auto statement = FLAGS_rewrite ? ast::ParseCypherAndRewrite(input)
                                     : ast::ParseCypher(input);
      ast::ASTPrinter printer(std::cout);
      printer.Print(*statement);
      return 0;
    }
    if (FLAGS_mode == "planner_query") {
      auto statement = ast::ParseCypherAndRewrite(input);
      auto planner_query = ir::CreatePlannerQuery(*statement);
      ir::PrintPlannerQuery(*planner_query, std::cout);
      return 0;
    }
    if (FLAGS_mode == "logical_plan") {
      auto statement = ast::ParseCypherAndRewrite(input);
      auto planner_query = ir::CreatePlannerQuery(*statement);
      auto logical_plan = ir::CreateLogicalPlan(*planner_query);
      ir::PrintLogicalPlan(
          *logical_plan, std::cout,
          ir::LogicalPlanPrinterOptions{.include_metadata = true});
      return 0;
    }
    spdlog::error("Unsupported dump mode: {}", FLAGS_mode);
    PrintMinimalUsage();
    return 1;
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
