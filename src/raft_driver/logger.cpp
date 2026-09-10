/**
 * Copyright 2026 AntGroup CO., Ltd.
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

/*
 * written by botu.wzy
 */

#include "common/logger.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include "raft/util.h"

namespace eraft {
namespace {

std::string format_log_message(const char* pattern, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);
  const int required_size = std::vsnprintf(nullptr, 0, pattern, args_copy);
  va_end(args_copy);

  if (required_size < 0) {
    return "failed to format eraft log message";
  }

  std::string message(static_cast<size_t>(required_size), '\0');
  std::vsnprintf(message.data(), message.size() + 1, pattern, args);
  return message;
}

void log_message(spdlog::level::level_enum level, const char* file, int line,
                 const char* pattern, va_list args) {
  if (!spdlog::should_log(level)) {
    return;
  }
  const std::string message = format_log_message(pattern, args);
  spdlog::log(spdlog::source_loc{file, line, ""}, level, "{}", message);
}

}  // namespace

void log_debug(const char* file, int line, const char* pattern, ...) {
  va_list args;
  va_start(args, pattern);
  log_message(spdlog::level::debug, file, line, pattern, args);
  va_end(args);
}

void log_info(const char* file, int line, const char* pattern, ...) {
  va_list args;
  va_start(args, pattern);
  log_message(spdlog::level::info, file, line, pattern, args);
  va_end(args);
}

void log_warn(const char* file, int line, const char* pattern, ...) {
  va_list args;
  va_start(args, pattern);
  log_message(spdlog::level::warn, file, line, pattern, args);
  va_end(args);
}

void log_error(const char* file, int line, const char* pattern, ...) {
  va_list args;
  va_start(args, pattern);
  log_message(spdlog::level::err, file, line, pattern, args);
  va_end(args);
}

void log_fatal(const char* file, int line, const char* pattern, ...) {
  va_list args;
  va_start(args, pattern);
  const std::string message = format_log_message(pattern, args);
  va_end(args);

  spdlog::log(spdlog::source_loc{file, line, ""}, spdlog::level::critical, "{}",
              message);
  spdlog::default_logger()->flush();
  throw PanicException("panic");
}

}  // namespace eraft
