#include <sys/resource.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "bolt/bolt_server.h"
#include "common/exception.h"
#include "common/flags.h"
#include "common/logger.h"
#include "common/table_printer.h"
#include "common/version.h"
#include "server/graph_server.h"
#include "server/raft_server.h"

std::unordered_set<std::string> inner_flags = {"flagfile",
                                               "fromenv",
                                               "tryfromenv",
                                               "undefok",
                                               "tab_completion_columns",
                                               "tab_completion_word",
                                               "help",
                                               "helpfull",
                                               "helpmatch",
                                               "helpon",
                                               "helppackage",
                                               "helpshort",
                                               "helpxml",
                                               "version"};

using namespace bolt;
namespace server {
namespace {

volatile std::sig_atomic_t g_shutdown_signal = 0;

}  // namespace

std::string Version() {
  std::ostringstream info;
  info << "\nRocksGraph\nCompiled from " << common::kGitBranch
       << " branch\nCommit " << common::kGitCommitHash;
  info << "\nCPP compiler version: " << common::kCxxCompilerId << " "
       << common::kCxxCompilerVersion << ".";
  return info.str();
}
void PrintWelcome() {
  std::ostringstream info;
  {
    common::TablePrinter table;
    info << "Compile Information:\n";
    table.AddRow({"Branch", common::kGitBranch});
    table.AddRow({"Commit", common::kGitCommitHash});
    table.AddRow({"BuildType", common::kBuildType});
    info << table << "\n";
  }
  {
    struct rlimit rlim {};
    getrlimit(RLIMIT_CORE, &rlim);
    std::ifstream file("/proc/sys/kernel/core_pattern");
    std::string path;
    if (file.is_open()) {
      std::string content((std::istreambuf_iterator<char>(file)),
                          (std::istreambuf_iterator<char>()));
      path = content;
      file.close();
    } else {
      LOG_ERROR("Failed to read /proc/sys/kernel/core_pattern");
    }
    common::TablePrinter table;
    table.AddRow({"coredump file limit size", std::to_string(rlim.rlim_cur)});
    table.AddRow({"coredump file path", path});
    info << "System environment Information:\n";
    info << table << "\n";
  }
  {
    common::TablePrinter table;
    std::vector<gflags::CommandLineFlagInfo> flags;
    GetAllFlags(&flags);
    for (auto& flag : flags) {
      if (inner_flags.count(flag.name)) {
        continue;
      }
      table.AddRow({flag.name, flag.current_value});
    }
    info << "Config Information:\n";
    info << table;
  }
  LOG_INFO(info.str());
  spdlog::default_logger()->flush();
}
void ShutDownHandler(int sig) { g_shutdown_signal = sig; }
void CrashHandler(int sig) {
  LOG_ERROR("Received signal {}, crash", strsignal(sig));
  spdlog::default_logger()->flush();
  struct sigaction sa {};
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = SIG_DFL;
  sigaction(sig, &sa, nullptr);
  kill(getpid(), sig);
}
void SetupSignalHandler() {
  {
    // shutdown
    struct sigaction sa {};
    sa.sa_handler = ShutDownHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
  }
  {
    // crash
    struct sigaction sa {};
    sa.sa_handler = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
  }
}

int RunServer() {
  if (!SetupLogger()) return -1;
  SetupSignalHandler();
  PrintWelcome();
  GraphServer server(
      {.data_path = FLAGS_data_path,
       .local_node_options = {.host = FLAGS_host,
                              .bolt_port = FLAGS_bolt_port,
                              .raft_port = FLAGS_raft_port},
       .bolt_io_thread_num = FLAGS_bolt_io_thread_num,
       .bolt_worker_thread_num = FLAGS_bolt_worker_thread_num,
       .max_bolt_connections = FLAGS_max_bolt_connections,
       .graph_manager_options = {
           .block_cache_size = FLAGS_graph_block_cache,
           .raft_log_block_cache_size = FLAGS_raft_log_block_cache,
           .raft_scheduler_shards = FLAGS_raft_scheduler_shards,
           .assistant_thread_num = FLAGS_assistant_thread_num,
           .ft_apply_interval = FLAGS_ft_apply_interval,
           .ft_writer_threads = FLAGS_ft_writer_threads,
           .ft_writer_memory_budget = FLAGS_ft_writer_memory_budget,
           .vt_apply_interval = FLAGS_vt_apply_interval,
           .plan_cache_capacity = FLAGS_plan_cache_capacity}});
  g_shutdown_signal = 0;
  try {
    if (!server.Start()) {
      RG_THROW(common::ErrorCode::InternalError, "failed to start rg-server");
    }
    while (g_shutdown_signal == 0 && server.Started()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (g_shutdown_signal != 0) {
      LOG_INFO("Received signal {}, shutdown",
               strsignal(static_cast<int>(g_shutdown_signal)));
    } else if (!server.Started()) {
      RG_THROW(common::ErrorCode::InternalError,
               "rg-server exited unexpectedly");
    }
    server.Stop();
    spdlog::shutdown();
    return 0;
  } catch (const std::exception& e) {
    server.Stop();
    LOG_ERROR(e.what());
    return -1;
  }
}
}  // namespace server
int main(int argc, char* argv[]) {
  gflags::SetVersionString(server::Version());
  gflags::SetUsageMessage("Usage: " + std::string(argv[0]) + " [options]");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  spdlog::set_pattern("%v");
  return server::RunServer();
}
