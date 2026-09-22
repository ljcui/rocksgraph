#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rg {

class PhysicalPlan;

struct PlanCacheKey {
  std::string cypher;
  std::size_t max_idp_candidates_per_relationship_count = 128;
  std::uint64_t graph_identity = 0;
  const void *planner_catalog_identity = nullptr;
  const void *planner_statistics_identity = nullptr;
  std::uint64_t planner_catalog_version = 0;
  std::uint64_t planning_context_version = 0;

  bool operator==(const PlanCacheKey &) const = default;
};

class CachedPlan final {
 public:
  CachedPlan(std::shared_ptr<const PhysicalPlan> physical_plan,
             std::vector<std::string> required_parameters);

  [[nodiscard]] const std::shared_ptr<const PhysicalPlan> &PhysicalPlanPtr()
      const noexcept;
  [[nodiscard]] const std::vector<std::string> &RequiredParameters()
      const noexcept;

 private:
  std::shared_ptr<const PhysicalPlan> physical_plan_;
  std::vector<std::string> required_parameters_;
};

struct PlanCacheStats {
  std::size_t entries = 0;
  std::size_t capacity = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t compilations = 0;
  std::uint64_t in_flight_waits = 0;
  std::uint64_t evictions = 0;
};

class PlanCache final {
 public:
  using Compiler = std::function<std::shared_ptr<const CachedPlan>()>;

  explicit PlanCache(std::size_t capacity);
  PlanCache(const PlanCache &) = delete;
  PlanCache &operator=(const PlanCache &) = delete;
  ~PlanCache();

  // When capacity is nonzero, the compiler is called at most once concurrently
  // for a key. Compilation failures and null results are never cached.
  [[nodiscard]] std::shared_ptr<const CachedPlan> LookupOrCompile(
      const PlanCacheKey &key, const Compiler &compiler);

  // Clear does not cancel callers that already started compilation. Their
  // result is returned to those callers but is not inserted after this call.
  void Clear();
  [[nodiscard]] PlanCacheStats GetStats() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rg
