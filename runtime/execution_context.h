#pragma once

#include <chrono>
#include <functional>

#include "value/value.h"

namespace rg {

class GraphReader;

using QueryParameters = Value::Map;

struct ExecutionClock {
  using SystemClock = std::chrono::system_clock;
  using TimePoint = SystemClock::time_point;

  TimePoint transaction_time;
  TimePoint statement_time;
  std::function<TimePoint()> realtime_now;

  [[nodiscard]] static ExecutionClock Start() {
    const TimePoint now = SystemClock::now();
    return {.transaction_time = now, .statement_time = now, .realtime_now = [] {
              return SystemClock::now();
            }};
  }

  [[nodiscard]] TimePoint Realtime() const {
    return realtime_now ? realtime_now() : SystemClock::now();
  }
};

struct ExecutionContext {
  const GraphReader *graph_reader = nullptr;
  const QueryParameters *parameters = nullptr;
  ExecutionClock clock = ExecutionClock::Start();

  [[nodiscard]] const Value *FindParameter(const std::string &name) const {
    if (parameters == nullptr) {
      return nullptr;
    }
    const auto found = parameters->find(name);
    return found == parameters->end() ? nullptr : &found->second;
  }
};

}  // namespace rg
