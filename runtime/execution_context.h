#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/exception.h"
#include "value/value.h"

namespace rg {

class GraphReader;

using QueryParameters = Value::Map;

class BoundQueryParameters final {
 public:
  explicit BoundQueryParameters(const QueryParameters &parameters) {
    values_.reserve(parameters.size());
    for (const auto &[name, value] : parameters) {
      offsets_.emplace(name, values_.size());
      values_.push_back(value);
    }
  }

  [[nodiscard]] const Value *Find(const std::string &name) const {
    const auto found = offsets_.find(name);
    return found == offsets_.end() ? nullptr : &values_[found->second];
  }
  [[nodiscard]] std::optional<std::size_t> Offset(
      const std::string &name) const {
    const auto found = offsets_.find(name);
    return found == offsets_.end() ? std::nullopt
                                   : std::optional<std::size_t>(found->second);
  }
  [[nodiscard]] const Value &At(std::size_t offset) const {
    CHECK(offset < values_.size(), common::InternalError,
          "parameter offset is out of range");
    return values_[offset];
  }

 private:
  std::unordered_map<std::string, std::size_t> offsets_;
  std::vector<Value> values_;
};

class QueryCancellationToken final {
 public:
  void Cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool IsCancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> cancelled_ = false;
};

class QueryMemoryTracker final {
 public:
  explicit QueryMemoryTracker(
      std::size_t limit_bytes = std::numeric_limits<std::size_t>::max())
      : limit_bytes_(limit_bytes) {}

  void Reserve(std::size_t bytes) {
    CHECK(bytes <= limit_bytes_ - current_bytes_,
          common::MemoryLimitExceededError, "query memory limit exceeded");
    current_bytes_ += bytes;
    peak_bytes_ = std::max(peak_bytes_, current_bytes_);
  }

  void Release(std::size_t bytes) noexcept {
    current_bytes_ = bytes >= current_bytes_ ? 0 : current_bytes_ - bytes;
  }

  [[nodiscard]] std::size_t CurrentBytes() const noexcept {
    return current_bytes_;
  }
  [[nodiscard]] std::size_t PeakBytes() const noexcept { return peak_bytes_; }

 private:
  std::size_t limit_bytes_ = std::numeric_limits<std::size_t>::max();
  std::size_t current_bytes_ = 0;
  std::size_t peak_bytes_ = 0;
};

struct QueryExecutionOptions {
  std::shared_ptr<QueryCancellationToken> cancellation;
  std::size_t memory_limit_bytes = std::numeric_limits<std::size_t>::max();
};

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
  const BoundQueryParameters *bound_parameters = nullptr;
  QueryCancellationToken *cancellation = nullptr;
  QueryMemoryTracker *memory_tracker = nullptr;
  ExecutionClock clock = ExecutionClock::Start();

  void CheckCancelled() const {
    CHECK(cancellation == nullptr || !cancellation->IsCancelled(),
          common::QueryCancelledError, "query execution was cancelled");
  }

  [[nodiscard]] const Value *FindParameter(const std::string &name) const {
    if (bound_parameters != nullptr) {
      return bound_parameters->Find(name);
    }
    if (parameters == nullptr) {
      return nullptr;
    }
    const auto found = parameters->find(name);
    return found == parameters->end() ? nullptr : &found->second;
  }
};

}  // namespace rg
