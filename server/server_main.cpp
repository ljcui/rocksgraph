/**
 * Copyright 2024 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

//
// Created by botu.wzy
//

#include <sys/resource.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tabulate/table.hpp>
#include <thread>
#include <unordered_set>

#include "bolt/bolt_server.h"
#include "common/flags.h"
#include "common/logger.h"
#include "common/version.h"
#include "server/lgraph_server.h"
#include "server/raft_server.h"
#include "service.h"

DEFINE_string(mode, "run", "Service mode: run, start, restart, or stop");

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
  std::string version;
  version.append(std::to_string(LGRAPH_VERSION_MAJOR))
      .append(".")
      .append(std::to_string(LGRAPH_VERSION_MINOR))
      .append(".")
      .append(std::to_string(LGRAPH_VERSION_PATCH));
  info << "\nTuGraph v" << version << "\nCompiled from " << GIT_BRANCH
       << " branch\nCommit " << GIT_COMMIT_HASH;
  info << "\nCPP compiler version: " << CXX_COMPILER_ID << " "
       << CXX_COMPILER_VERSION << ".";
  return info.str();
}
void PrintWelcome() {
  std::string version;
  version.append(std::to_string(5))
      .append(".")
      .append(std::to_string(0))
      .append(".")
      .append(std::to_string(0));
  std::ostringstream info;
  info << "\n"
       << "********************************************************************"
          "**"
       << "\n"
       << "*                  TuGraph Graph Database v" << version
       << std::string(26 - version.size(), ' ') << "*" << "\n"
       << "*                                                                   "
          " *"
       << "\n"
       << "*    Copyright(C) 2018-2023 Ant Group. All rights reserved.         "
          " *"
       << "\n"
       << "*                                                                   "
          " *"
       << "\n"
       << "********************************************************************"
          "**"
       << "\n";
  {
    tabulate::Table table;
    table.format().trim_mode(tabulate::Format::TrimMode::kNone).locale("C");
    info << "Compile Information:\n";
    table.add_row({"Branch", GIT_BRANCH});
    table.add_row({"Commit", GIT_COMMIT_HASH});
    table.add_row({"BuildType", BUILD_TYPE});
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
    tabulate::Table table;
    table.format().trim_mode(tabulate::Format::TrimMode::kNone).locale("C");
    table.add_row({"coredump file limit size", std::to_string(rlim.rlim_cur)});
    table.add_row({"coredump file path", path});
    info << "System environment Information:\n";
    info << table << "\n";
  }
  {
    tabulate::Table table;
    table.format().trim_mode(tabulate::Format::TrimMode::kNone).locale("C");
    std::vector<gflags::CommandLineFlagInfo> flags;
    GetAllFlags(&flags);
    for (auto& flag : flags) {
      if (inner_flags.count(flag.name)) {
        continue;
      }
      table.add_row({flag.name, flag.current_value});
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

class LGraphDaemon : public Service {
 public:
  LGraphDaemon() : Service("lgraph_server", FLAGS_pid_file) {}
  int Run() override {
    if (!SetupLogger()) return -1;
    SetupSignalHandler();
    PrintWelcome();
    LGraphServer server(
        {.data_path = FLAGS_data_path,
         .local_node_options = {.host = FLAGS_host,
                                .bolt_port = FLAGS_bolt_port,
                                .raft_port = FLAGS_raft_port},
         .bolt_io_thread_num = FLAGS_bolt_io_thread_num,
         .bolt_worker_thread_num = FLAGS_bolt_worker_thread_num,
         .max_bolt_connections = FLAGS_max_bolt_connections,
         .galaxy_options = {
             .block_cache_size = FLAGS_graph_block_cache,
             .raft_log_block_cache_size = FLAGS_raft_log_block_cache,
             .raft_scheduler_shards = FLAGS_raft_scheduler_shards,
             .assistant_thread_num = FLAGS_assistant_thread_num,
             .ft_apply_interval = FLAGS_ft_apply_interval,
             .ft_writer_threads = FLAGS_ft_writer_threads,
             .ft_writer_memory_budget = FLAGS_ft_writer_memory_budget,
             .vt_apply_interval = FLAGS_vt_apply_interval}});
    g_shutdown_signal = 0;
    try {
      if (!server.Start()) {
        throw std::runtime_error("failed to start lgraph server");
      }
      while (g_shutdown_signal == 0 && server.Started()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (g_shutdown_signal != 0) {
        LOG_INFO("Received signal {}, shutdown",
                 strsignal(static_cast<int>(g_shutdown_signal)));
      } else if (!server.Started()) {
        throw std::runtime_error("lgraph server exited unexpectedly");
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
};
}  // namespace server
using namespace server;
int main(int argc, char* argv[]) {
  gflags::SetVersionString(Version());
  gflags::SetUsageMessage("Usage: " + std::string(argv[0]) + " [options]");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  server::LGraphDaemon daemon;
  spdlog::set_pattern("%v");
  if (FLAGS_mode == "run") {
    return daemon.Run();
  } else if (FLAGS_mode == "start") {
    return daemon.Start();
  } else if (FLAGS_mode == "restart") {
    return daemon.Restart();
  } else if (FLAGS_mode == "stop") {
    return daemon.Stop();
  }
}
