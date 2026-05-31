#include <iostream>
#include <string>
#include <vector>

#include "common/exception.h"
#include "gflags/gflags.h"
#include "runtime/query_executor.h"
#include "spdlog/spdlog.h"

DEFINE_bool(seed_demo_graph, true,
            "Load a small in-memory demo graph before executing the query.");

namespace {

std::string JoinArgs(const std::vector<std::string> &parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out.push_back(' ');
    }
    out += parts[i];
  }
  return out;
}

void SeedDemoGraph(rg::InMemoryGraph *graph) {
  auto ada = graph->CreateNode(
      {"Person"}, {{"name", rg::Value("Ada")}, {"age", rg::Value(36)}});
  auto grace = graph->CreateNode(
      {"Person"}, {{"name", rg::Value("Grace")}, {"age", rg::Value(85)}});
  auto cpp = graph->CreateNode({"Language"}, {{"name", rg::Value("C++")}});
  graph->CreateRelationship(ada, grace, "KNOWS", {{"since", rg::Value(2020)}});
  graph->CreateRelationship(ada, cpp, "USES", {{"since", rg::Value(2024)}});
  graph->AddNodeIndex({"Person"}, "name");
  graph->AddRelationshipIndex({"KNOWS"}, "since");
}

void PrintResult(const rg::QueryResult &result) {
  for (std::size_t i = 0; i < result.columns.size(); ++i) {
    if (i > 0) {
      std::cout << '\t';
    }
    std::cout << result.columns[i];
  }
  std::cout << '\n';

  for (const auto &row : result.rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) {
        std::cout << '\t';
      }
      std::cout << row[i].ToString();
    }
    std::cout << '\n';
  }
}

void PrintUsage() {
  std::cerr << "Usage:\n  rg-server [--seed_demo_graph] [--] <cypher...>\n";
}

}  // namespace

int main(int argc, char **argv) {
  gflags::SetUsageMessage(
      "Usage:\n  rg-server [--seed_demo_graph] [--] <cypher...>");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (argc <= 1) {
    spdlog::error("Missing cypher statement.");
    PrintUsage();
    return 1;
  }

  std::vector<std::string> parts;
  parts.reserve(static_cast<std::size_t>(argc - 1));
  for (int i = 1; i < argc; ++i) {
    parts.emplace_back(argv[i]);
  }

  rg::InMemoryGraph graph;
  if (FLAGS_seed_demo_graph) {
    SeedDemoGraph(&graph);
  }

  try {
    PrintResult(rg::ExecuteQuery(graph, JoinArgs(parts)));
  } catch (const common::Exception &e) {
    spdlog::error("Query error: {}", e.Message());
    return 1;
  } catch (const std::exception &e) {
    spdlog::error("Query error: {}", e.what());
    return 1;
  }
  return 0;
}
