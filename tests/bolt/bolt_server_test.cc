#include "bolt/bolt_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include "bolt/worker_pool.h"

TEST(BoltServerTest, StopDrainsQueuedWorkerTasks) {
  auto worker_pool =
      std::make_shared<bolt::BoltWorkerPool>(1, "bolt-test-", "bolt-test");
  bolt::BoltServer server;
  ASSERT_TRUE(server.Start(
      0, 1, 10,
      [](bolt::BoltConnection&, bolt::BoltMsg, std::vector<std::any>) {},
      worker_pool));

  std::promise<void> task_started;
  auto started = task_started.get_future();
  std::promise<void> release_task;
  auto release = release_task.get_future().share();
  std::atomic<int> completed{0};
  auto strand = worker_pool->MakeStrand();
  ASSERT_TRUE(worker_pool->Post(strand, [&] {
    task_started.set_value();
    release.wait();
    completed.fetch_add(1);
  }));
  const bool queued =
      worker_pool->Post(strand, [&] { completed.fetch_add(1); });
  const auto started_status = started.wait_for(std::chrono::seconds(2));
  std::weak_ptr<bolt::BoltWorkerPool> weak_pool = worker_pool;
  worker_pool.reset();

  auto stopped = std::async(std::launch::async, [&] { server.Stop(); });
  const auto blocked_status = stopped.wait_for(std::chrono::milliseconds(50));
  release_task.set_value();
  stopped.get();

  EXPECT_TRUE(queued);
  EXPECT_EQ(started_status, std::future_status::ready);
  EXPECT_EQ(blocked_status, std::future_status::timeout);
  EXPECT_EQ(completed.load(), 2);
  EXPECT_TRUE(weak_pool.expired());
}
