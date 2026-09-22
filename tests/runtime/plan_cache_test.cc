#include "runtime/plan_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/exception.h"
#include "planner/planned_query.h"
#include "runtime/physical_plan.h"
#include "runtime/query_executor.h"
#include "tests/common/exception_test_utils.h"
#include "tests/runtime/graphdb_test_utils.h"

namespace {

std::shared_ptr<const rg::CachedPlan> Compile(std::string_view cypher) {
  planner::PlannedQuery query = planner::PlanCypher(cypher);
  auto physical = std::make_shared<rg::PhysicalPlan>(
      rg::CreatePhysicalPlan(query.LogicalPlan()));
  return std::make_shared<rg::CachedPlan>(std::move(physical),
                                          std::vector<std::string>{});
}

TEST(PlanCacheTest, ReusesPlansAndEvictsLeastRecentlyUsedEntry) {
  rg::PlanCache cache(2);
  const rg::PlanCacheKey first{.cypher = "RETURN 1 AS value"};
  const rg::PlanCacheKey second{.cypher = "RETURN 2 AS value"};
  const rg::PlanCacheKey third{.cypher = "RETURN 3 AS value"};

  auto first_plan =
      cache.LookupOrCompile(first, [&] { return Compile(first.cypher); });
  auto second_plan =
      cache.LookupOrCompile(second, [&] { return Compile(second.cypher); });
  EXPECT_EQ(cache.LookupOrCompile(first, [&] { return Compile(first.cypher); }),
            first_plan);
  (void)cache.LookupOrCompile(third, [&] { return Compile(third.cypher); });
  auto recompiled_second =
      cache.LookupOrCompile(second, [&] { return Compile(second.cypher); });

  EXPECT_NE(recompiled_second, second_plan);
  const rg::PlanCacheStats stats = cache.GetStats();
  EXPECT_EQ(stats.entries, 2U);
  EXPECT_EQ(stats.hits, 1U);
  EXPECT_EQ(stats.misses, 4U);
  EXPECT_EQ(stats.compilations, 4U);
  EXPECT_EQ(stats.evictions, 2U);
}

TEST(PlanCacheTest, SeparatesPlannerContextsAndClearForcesRecompilation) {
  rg::PlanCache cache(8);
  rg::PlanCacheKey key{
      .cypher = "RETURN 1 AS value",
      .graph_identity = 1,
      .planner_catalog_identity = reinterpret_cast<const void *>(2),
      .planner_catalog_version = 3};

  auto original =
      cache.LookupOrCompile(key, [&] { return Compile(key.cypher); });
  rg::PlanCacheKey changed = key;
  ++changed.planning_context_version;
  auto versioned =
      cache.LookupOrCompile(changed, [&] { return Compile(changed.cypher); });
  EXPECT_NE(versioned, original);

  cache.Clear();
  auto after_clear =
      cache.LookupOrCompile(key, [&] { return Compile(key.cypher); });
  EXPECT_NE(after_clear, original);
  EXPECT_EQ(cache.GetStats().entries, 1U);
}

TEST(PlanCacheTest, DeduplicatesConcurrentCompilation) {
  rg::PlanCache cache(4);
  const rg::PlanCacheKey key{.cypher = "RETURN 1 AS value"};
  std::atomic<std::size_t> compiler_calls = 0;
  std::promise<void> compiler_started;
  auto compiler_started_future = compiler_started.get_future();
  std::promise<void> release_compiler;
  std::shared_future<void> release_future =
      release_compiler.get_future().share();
  auto compiler = [&]() {
    if (compiler_calls.fetch_add(1) == 0) {
      compiler_started.set_value();
    }
    release_future.wait();
    return Compile(key.cypher);
  };

  auto owner = std::async(std::launch::async,
                          [&] { return cache.LookupOrCompile(key, compiler); });
  ASSERT_EQ(compiler_started_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  constexpr std::size_t kWaiters = 8;
  std::vector<std::future<std::shared_ptr<const rg::CachedPlan>>> waiters;
  waiters.reserve(kWaiters);
  for (std::size_t i = 0; i < kWaiters; ++i) {
    waiters.push_back(std::async(std::launch::async, [&] {
      return cache.LookupOrCompile(key, compiler);
    }));
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (cache.GetStats().in_flight_waits != kWaiters &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool all_waiting = cache.GetStats().in_flight_waits == kWaiters;
  release_compiler.set_value();

  auto plan = owner.get();
  for (auto &waiter : waiters) {
    EXPECT_EQ(waiter.get(), plan);
  }
  EXPECT_TRUE(all_waiting);
  EXPECT_EQ(compiler_calls.load(), 1U);
  EXPECT_EQ(cache.GetStats().compilations, 1U);
}

TEST(PlanCacheTest, DoesNotCacheCompilationFailures) {
  rg::PlanCache cache(4);
  const rg::PlanCacheKey key{.cypher = "invalid"};
  std::size_t compiler_calls = 0;
  auto compiler = [&]() -> std::shared_ptr<const rg::CachedPlan> {
    ++compiler_calls;
    RG_THROW(common::ErrorCode::SemanticError, "test compilation failure");
  };

  RG_EXPECT_ERROR((void)cache.LookupOrCompile(key, compiler),
                  common::ErrorCode::SemanticError);
  RG_EXPECT_ERROR((void)cache.LookupOrCompile(key, compiler),
                  common::ErrorCode::SemanticError);
  EXPECT_EQ(compiler_calls, 2U);
  EXPECT_EQ(cache.GetStats().entries, 0U);
}

TEST(PlanCacheTest, ExecuteQueryReusesPlanAcrossParameterValues) {
  rg::test::GraphDBTestDatabase graph;
  rg::PlanCache cache(8);

  rg::QueryOptions first_options;
  first_options.plan_cache = &cache;
  first_options.parameters.emplace("value", rg::Value(1));
  rg::QueryResult first = rg::test::ExecuteGraphDBQueryAndCommit(
      graph.Graph(), "RETURN $value AS value", first_options);

  rg::QueryOptions second_options;
  second_options.plan_cache = &cache;
  second_options.parameters.emplace("value", rg::Value(2));
  rg::QueryResult second = rg::test::ExecuteGraphDBQueryAndCommit(
      graph.Graph(), "RETURN $value AS value", second_options);

  ASSERT_EQ(first.rows.size(), 1U);
  ASSERT_EQ(second.rows.size(), 1U);
  EXPECT_EQ(first.rows[0][0], rg::Value(1));
  EXPECT_EQ(second.rows[0][0], rg::Value(2));
  EXPECT_EQ(cache.GetStats().hits, 1U);
  EXPECT_EQ(cache.GetStats().compilations, 1U);

  rg::QueryOptions missing_options;
  missing_options.plan_cache = &cache;
  RG_EXPECT_ERROR((void)rg::test::ExecuteGraphDBQueryAndCommit(
                      graph.Graph(), "RETURN $value AS value", missing_options),
                  common::ErrorCode::InvalidParameter);
  EXPECT_EQ(cache.GetStats().hits, 2U);
}

TEST(PlanCacheTest, GraphCatalogVersionInvalidatesPlansAfterIndexChanges) {
  rg::test::GraphDBTestDatabase graph;
  graph.CreateNode({"Person"}, {{"name", rg::Value("Ada")}});
  rg::PlanCache cache(8);
  rg::QueryOptions options;
  options.plan_cache = &cache;
  constexpr std::string_view kQuery =
      "MATCH (n:Person) WHERE n.name = 'Ada' RETURN n.name AS name";

  (void)rg::test::ExecuteGraphDBQueryAndCommit(graph.Graph(), kQuery, options);
  const std::string index_name = graph.AddNodeIndex({"Person"}, "name");
  (void)rg::test::ExecuteGraphDBQueryAndCommit(graph.Graph(), kQuery, options);
  graph.Graph().DeleteVertexPropertyIndex(index_name);
  (void)rg::test::ExecuteGraphDBQueryAndCommit(graph.Graph(), kQuery, options);

  const rg::PlanCacheStats stats = cache.GetStats();
  EXPECT_EQ(stats.hits, 0U);
  EXPECT_EQ(stats.misses, 3U);
  EXPECT_EQ(stats.compilations, 3U);
  EXPECT_EQ(stats.entries, 3U);
}

}  // namespace
