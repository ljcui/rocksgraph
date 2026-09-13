#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common/exception.h"
#include "gflags/gflags.h"
#include "graphdb/assistant_pool.h"
#include "graphdb/graph_db.h"
#include "runtime/query_executor.h"
#include "spdlog/spdlog.h"
#include "transaction/transaction.h"

DEFINE_string(db_path, "", "Path to the persistent GraphDB database.");

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
  std::cerr << "Usage:\n  cypher_run --db_path=<path> [--] <cypher...>\n";
}

void RollbackIfActive(txn::Transaction *transaction) noexcept {
  if (transaction == nullptr ||
      transaction->GetState() != txn::Transaction::State::kActive) {
    return;
  }
  try {
    transaction->Rollback();
  } catch (...) {
  }
}

}  // namespace

int main(int argc, char **argv) {
  gflags::SetUsageMessage(
      "Usage:\n  cypher_run --db_path=<path> [--] <cypher...>");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_db_path.empty()) {
    spdlog::error("Missing required --db_path.");
    PrintUsage();
    return 1;
  }
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

  std::unique_ptr<graphdb::GraphDB> graph;
  std::unique_ptr<txn::Transaction> transaction;
  try {
    graphdb::GraphDBOptions options;
    options.assistant_pool = std::make_shared<graphdb::AssistantPool>(1);
    graph = graphdb::GraphDB::Open(FLAGS_db_path, options);
    transaction = graph->BeginTransaction();
    PrintResult(rg::ExecuteQuery(*transaction, JoinArgs(parts)));
    transaction->Commit();
  } catch (const common::Exception &e) {
    RollbackIfActive(transaction.get());
    spdlog::error("Query error: {}", e.Message());
    return 1;
  } catch (const std::exception &e) {
    RollbackIfActive(transaction.get());
    spdlog::error("Query error: {}", e.what());
    return 1;
  }
  return 0;
}
