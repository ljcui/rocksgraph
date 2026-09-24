#include "bolt/bolt_server.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bolt/connection.h"
#include "bolt/worker_pool.h"

TEST(BoltServerTest, PostCloseRunsOnConnectionIOService) {
  boost::asio::io_service io_service;
  auto conn = std::make_shared<bolt::BoltConnection>(
      io_service,
      [](bolt::BoltConnection&, bolt::BoltMsg, std::vector<std::any>) {});
  conn->socket().open(boost::asio::ip::tcp::v4());

  std::thread worker([conn] {
    conn->PostClose();
    conn->PostClose();
  });
  worker.join();

  EXPECT_TRUE(conn->socket().is_open());
  EXPECT_FALSE(conn->has_closed());
  EXPECT_EQ(io_service.poll(), 2);
  EXPECT_FALSE(conn->socket().is_open());
  EXPECT_TRUE(conn->has_closed());
}

TEST(BoltServerTest, ClosesInsteadOfDispatchingMalformedMessage) {
  boost::asio::io_service server_io;
  boost::asio::ip::tcp::acceptor acceptor(server_io,
                                          {boost::asio::ip::tcp::v4(), 0});
  std::atomic<bool> handled{false};
  auto conn = std::make_shared<bolt::BoltConnection>(
      server_io, [&handled](bolt::BoltConnection&, bolt::BoltMsg,
                            std::vector<std::any>) { handled.store(true); });
  acceptor.async_accept(conn->socket(),
                        [conn](const boost::system::error_code& ec) {
                          if (!ec) {
                            conn->Start();
                          }
                        });

  boost::asio::io_service client_io;
  boost::asio::ip::tcp::socket client(client_io);
  client.connect(acceptor.local_endpoint());
  std::thread server_thread([&server_io] { server_io.run(); });

  std::array<uint8_t, 20> handshake{};
  handshake[0] = 0x60;
  handshake[1] = 0x60;
  handshake[2] = 0xb0;
  handshake[3] = 0x17;
  for (size_t i = 0; i < 4; i++) {
    handshake[i * 4 + 6] = 4;
    handshake[i * 4 + 7] = 4;
  }
  boost::asio::write(client, boost::asio::buffer(handshake));

  std::array<uint8_t, 4> selected_version{};
  boost::asio::read(client, boost::asio::buffer(selected_version));

  const std::string malformed_message = {
      char(0xb1), char(0x01), char(0xa1), char(0x88), 'p', 'r',        'i',
      'n',        'c',        'i',        'p',        'a', char(0x85), 'x'};
  std::array<uint8_t, 2> chunk_size{
      static_cast<uint8_t>(malformed_message.size() >> 8),
      static_cast<uint8_t>(malformed_message.size())};
  boost::asio::write(client, boost::asio::buffer(chunk_size));
  boost::asio::write(client, boost::asio::buffer(malformed_message));
  const std::array<uint8_t, 2> end_of_message{0, 0};
  boost::asio::write(client, boost::asio::buffer(end_of_message));

  for (int i = 0; i < 100 && !conn->has_closed(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(conn->has_closed());
  EXPECT_FALSE(handled.load());

  boost::system::error_code ec;
  client.close(ec);
  server_io.stop();
  server_thread.join();
}

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
