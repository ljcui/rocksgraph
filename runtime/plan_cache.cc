#include "runtime/plan_cache.h"

#include <condition_variable>
#include <exception>
#include <iterator>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/exception.h"
#include "runtime/physical_plan.h"

namespace rg {
namespace {

void HashCombine(std::size_t *seed, std::size_t value) {
  *seed ^= value + 0x9e3779b9U + (*seed << 6U) + (*seed >> 2U);
}

struct PlanCacheKeyHash {
  std::size_t operator()(const PlanCacheKey &key) const noexcept {
    std::size_t hash = std::hash<std::string>{}(key.cypher);
    HashCombine(&hash, std::hash<std::size_t>{}(
                           key.max_idp_candidates_per_relationship_count));
    HashCombine(&hash, std::hash<std::uint64_t>{}(key.graph_identity));
    HashCombine(&hash, std::hash<const void *>{}(key.planner_catalog_identity));
    HashCombine(&hash,
                std::hash<const void *>{}(key.planner_statistics_identity));
    HashCombine(&hash, std::hash<std::uint64_t>{}(key.planner_catalog_version));
    HashCombine(&hash,
                std::hash<std::uint64_t>{}(key.planning_context_version));
    return hash;
  }
};

struct InFlightKey {
  PlanCacheKey plan;
  std::uint64_t epoch = 0;

  bool operator==(const InFlightKey &) const = default;
};

struct InFlightKeyHash {
  std::size_t operator()(const InFlightKey &key) const noexcept {
    std::size_t hash = PlanCacheKeyHash{}(key.plan);
    HashCombine(&hash, std::hash<std::uint64_t>{}(key.epoch));
    return hash;
  }
};

}  // namespace

class PlanCache::Impl final {
 public:
  explicit Impl(std::size_t capacity) : capacity_(capacity) {}

  struct Entry {
    std::shared_ptr<const CachedPlan> plan;
    std::list<PlanCacheKey>::iterator lru_position;
  };

  struct InFlight {
    std::condition_variable completed;
    std::shared_ptr<const CachedPlan> plan;
    std::exception_ptr error;
    bool done = false;
  };

  std::size_t capacity_ = 0;
  mutable std::mutex mutex_;
  std::list<PlanCacheKey> lru_;
  std::unordered_map<PlanCacheKey, Entry, PlanCacheKeyHash> entries_;
  std::unordered_map<InFlightKey, std::shared_ptr<InFlight>, InFlightKeyHash>
      in_flight_;
  std::uint64_t epoch_ = 0;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  std::uint64_t compilations_ = 0;
  std::uint64_t in_flight_waits_ = 0;
  std::uint64_t evictions_ = 0;
};

CachedPlan::CachedPlan(std::shared_ptr<const PhysicalPlan> physical_plan,
                       std::vector<std::string> required_parameters)
    : physical_plan_(std::move(physical_plan)),
      required_parameters_(std::move(required_parameters)) {
  RG_CHECK(physical_plan_ != nullptr, common::ErrorCode::InternalError,
           "cached physical plan is null");
}

const std::shared_ptr<const PhysicalPlan> &CachedPlan::PhysicalPlanPtr()
    const noexcept {
  return physical_plan_;
}

const std::vector<std::string> &CachedPlan::RequiredParameters()
    const noexcept {
  return required_parameters_;
}

PlanCache::PlanCache(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

PlanCache::~PlanCache() = default;

std::shared_ptr<const CachedPlan> PlanCache::LookupOrCompile(
    const PlanCacheKey &key, const Compiler &compiler) {
  RG_CHECK(static_cast<bool>(compiler), common::ErrorCode::InvalidParameter,
           "plan compiler is empty");

  std::shared_ptr<Impl::InFlight> in_flight;
  InFlightKey in_flight_key;
  {
    std::unique_lock lock(impl_->mutex_);
    if (impl_->capacity_ == 0) {
      ++impl_->misses_;
      ++impl_->compilations_;
      lock.unlock();
      auto plan = compiler();
      RG_CHECK(plan != nullptr, common::ErrorCode::InternalError,
               "plan compiler returned null");
      return plan;
    }

    auto entry = impl_->entries_.find(key);
    if (entry != impl_->entries_.end()) {
      impl_->lru_.splice(impl_->lru_.begin(), impl_->lru_,
                         entry->second.lru_position);
      ++impl_->hits_;
      return entry->second.plan;
    }

    ++impl_->misses_;
    in_flight_key = InFlightKey{.plan = key, .epoch = impl_->epoch_};
    auto active = impl_->in_flight_.find(in_flight_key);
    if (active != impl_->in_flight_.end()) {
      in_flight = active->second;
      ++impl_->in_flight_waits_;
      in_flight->completed.wait(lock, [&] { return in_flight->done; });
      if (in_flight->error != nullptr) {
        std::rethrow_exception(in_flight->error);
      }
      return in_flight->plan;
    }

    in_flight = std::make_shared<Impl::InFlight>();
    impl_->in_flight_.emplace(in_flight_key, in_flight);
    ++impl_->compilations_;
  }

  std::shared_ptr<const CachedPlan> plan;
  std::exception_ptr error;
  try {
    plan = compiler();
    RG_CHECK(plan != nullptr, common::ErrorCode::InternalError,
             "plan compiler returned null");
  } catch (...) {
    error = std::current_exception();
  }

  {
    std::lock_guard lock(impl_->mutex_);
    if (error == nullptr && in_flight_key.epoch == impl_->epoch_) {
      try {
        impl_->lru_.push_front(key);
        try {
          const auto [entry, inserted] = impl_->entries_.emplace(
              key,
              Impl::Entry{.plan = plan, .lru_position = impl_->lru_.begin()});
          (void)entry;
          if (!inserted) {
            impl_->lru_.pop_front();
          }
        } catch (...) {
          impl_->lru_.pop_front();
        }
        if (impl_->entries_.size() > impl_->capacity_) {
          auto least_recent = std::prev(impl_->lru_.end());
          impl_->entries_.erase(*least_recent);
          impl_->lru_.erase(least_recent);
          ++impl_->evictions_;
        }
      } catch (...) {
        // Cache allocation failure must not fail a successfully compiled query
        // or leave callers waiting for its single-flight result.
      }
    }
    in_flight->plan = plan;
    in_flight->error = error;
    in_flight->done = true;
    impl_->in_flight_.erase(in_flight_key);
  }
  in_flight->completed.notify_all();

  if (error != nullptr) {
    std::rethrow_exception(error);
  }
  return plan;
}

void PlanCache::Clear() {
  std::lock_guard lock(impl_->mutex_);
  impl_->entries_.clear();
  impl_->lru_.clear();
  ++impl_->epoch_;
}

PlanCacheStats PlanCache::GetStats() const {
  std::lock_guard lock(impl_->mutex_);
  return PlanCacheStats{.entries = impl_->entries_.size(),
                        .capacity = impl_->capacity_,
                        .hits = impl_->hits_,
                        .misses = impl_->misses_,
                        .compilations = impl_->compilations_,
                        .in_flight_waits = impl_->in_flight_waits_,
                        .evictions = impl_->evictions_};
}

}  // namespace rg
