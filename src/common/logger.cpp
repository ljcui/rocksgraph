#include "logger.h"

#include <spdlog/sinks/rotating_file_sink.h>

#include <iostream>

#include "flags.h"

void SetupQueryLogger() {
  auto query_logger = spdlog::rotating_logger_mt(
      "query_logger", FLAGS_query_log_path + "/query.log",
      FLAGS_query_log_max_size, FLAGS_query_log_max_files);
  query_logger->set_level(spdlog::level::level_enum::info);
  query_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e %t] %v");
}

bool SetupLogger() {
  try {
    auto logger = spdlog::rotating_logger_mt(
        "lgraph_logger", FLAGS_log_path + "/lgraph.log", FLAGS_log_max_size,
        FLAGS_log_max_files);
    logger->set_level(spdlog::level::from_str(FLAGS_log_level));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e %t %l %s:%#] %v");
    spdlog::set_default_logger(logger);
    if (FLAGS_enable_query_log) {
      SetupQueryLogger();
    }
    spdlog::flush_every(std::chrono::seconds(FLAGS_log_flush_interval));
  } catch (const std::exception& e) {
    std::cerr << "Log init failed: " << e.what() << std::endl;
    return false;
  }
  return true;
}