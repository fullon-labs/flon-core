#include <boost/test/unit_test.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/transaction_history_plugin/transaction_history_plugin.hpp>
#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <eosio/transaction_history_plugin/async_worker.hpp>
#include <eosio/transaction_history_plugin/rollback_manager.hpp>
#include <eosio/chain/name.hpp>
#include <eosio/chain/types.hpp>
#include <filesystem>
#include <ctime>
#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <vector>

using namespace eosio;
using namespace eosio::testing;

BOOST_AUTO_TEST_SUITE(transaction_history_plugin_tests)

BOOST_AUTO_TEST_CASE(rocksdb_manager_basic_operations) {
   rocksdb_manager manager;
   std::string test_path = "/tmp/test_rocksdb_" + std::to_string(std::time(nullptr));

   // Test database opening
   BOOST_REQUIRE(manager.open(test_path));

   // Test basic put/get operations
   BOOST_CHECK(manager.put("test_key", "test_value"));

   std::string value;
   BOOST_CHECK(manager.get("test_key", value));
   BOOST_CHECK_EQUAL(value, "test_value");

   // Test non-existent key
   BOOST_CHECK(!manager.get("non_existent_key", value));

   // Test object serialization
   transaction_id_type test_id;
   BOOST_CHECK(manager.put_object("test_id", test_id));

   transaction_id_type retrieved_id;
   BOOST_CHECK(manager.get_object("test_id", retrieved_id));
   BOOST_CHECK_EQUAL(test_id, retrieved_id);

   // Test batch operations
   std::vector<std::pair<std::string, std::string>> writes = {
      {"batch_key1", "batch_value1"},
      {"batch_key2", "batch_value2"}
   };
   BOOST_CHECK(manager.batch_write(writes));

   std::string batch_value;
   BOOST_CHECK(manager.get("batch_key1", batch_value));
   BOOST_CHECK_EQUAL(batch_value, "batch_value1");

   // HTTP-facing statistics use RocksDB metadata and never require exact
   // per-prefix counts from a full keyspace scan.
   const auto stats = fc::json::from_string(manager.get_database_stats()).get_object();
   BOOST_CHECK(stats.contains("estimated_total_keys"));
   BOOST_CHECK(stats.contains("rocksdb_metadata"));
   BOOST_CHECK(!stats.contains("key_counts"));

   manager.close();

   // Cleanup
   std::filesystem::remove_all(test_path);
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_json_validation_and_cleanup) {
   fc::temp_directory temp_dir;
   rocksdb_manager manager;
   BOOST_REQUIRE(manager.open((temp_dir.path() / "history").string()));

   auto history_record = [](uint32_t block_num) {
      return std::map<std::string, fc::variant>{{"block_num", block_num}, {"payload", "test"}};
   };

   BOOST_REQUIRE(manager.put_object("trx:before", history_record(99)));
   BOOST_REQUIRE(manager.put_object("acc:before", history_record(99)));
   BOOST_REQUIRE(manager.put_object("trx:from", history_record(100)));
   BOOST_REQUIRE(manager.put_object("acc:after", history_record(101)));
   BOOST_REQUIRE(manager.put("trx:invalid", "not-json"));
   std::vector<std::pair<std::string, std::string>> invalid_records;
   invalid_records.reserve(10005);
   for (size_t i = 0; i < 10005; ++i) {
      invalid_records.emplace_back("trx:invalid-batch-" + std::to_string(i), "not-json");
   }
   BOOST_REQUIRE(manager.batch_write(invalid_records));
   BOOST_REQUIRE(manager.update_last_block_number(101));

   BOOST_REQUIRE(manager.validate_and_repair_database());

   std::string value;
   BOOST_CHECK(manager.get("trx:before", value));
   BOOST_CHECK(manager.get("acc:before", value));
   BOOST_CHECK(manager.get("trx:from", value));
   BOOST_CHECK(manager.get("acc:after", value));
   BOOST_CHECK(!manager.get("trx:invalid", value));
   auto iterator = std::unique_ptr<rocksdb::Iterator>(manager.new_iterator());
   size_t invalid_records_remaining = 0;
   for (iterator->Seek("trx:invalid-batch-");
        iterator->Valid() && iterator->key().starts_with("trx:invalid-batch-");
        iterator->Next()) {
      ++invalid_records_remaining;
   }
   BOOST_CHECK_EQUAL(invalid_records_remaining, 0u);

   BOOST_REQUIRE(manager.clear_from_block(100));
   BOOST_CHECK(manager.get("trx:before", value));
   BOOST_CHECK(manager.get("acc:before", value));
   BOOST_CHECK(!manager.get("trx:from", value));
   BOOST_CHECK(!manager.get("acc:after", value));

   manager.close();
}

BOOST_AUTO_TEST_CASE(async_worker_task_execution) {
   async_worker worker;
   worker.start();

   std::atomic<int> counter{0};
   std::atomic<bool> task1_completed{false};
   std::atomic<bool> task2_completed{false};

   // Test simple task execution
   auto future1 = worker.enqueue_task([&counter, &task1_completed]() {
      counter++;
      task1_completed = true;
      return 42;
   });

   auto future2 = worker.enqueue_task([&counter, &task2_completed]() {
      counter++;
      task2_completed = true;
   });

   // Wait for tasks to complete
   BOOST_CHECK_EQUAL(future1.get(), 42);
   future2.wait();

   // Verify tasks were executed
   BOOST_CHECK(task1_completed.load());
   BOOST_CHECK(task2_completed.load());
   BOOST_CHECK_EQUAL(counter.load(), 2);

   worker.stop();
}

BOOST_AUTO_TEST_CASE(async_worker_queue_limits) {
   async_worker nonblocking_byte_limited_worker;
   BOOST_REQUIRE(nonblocking_byte_limited_worker.try_enqueue_task_with_size(
      async_worker::max_pending_bytes, [] {}));
   BOOST_CHECK(!nonblocking_byte_limited_worker.try_enqueue_task_with_size(1, [] {}));
   nonblocking_byte_limited_worker.start();
   nonblocking_byte_limited_worker.stop();

   async_worker byte_limited_worker;
   auto future = byte_limited_worker.enqueue_task_with_size(
      async_worker::max_pending_bytes, [] {});
   BOOST_CHECK_EQUAL(byte_limited_worker.pending_bytes(), async_worker::max_pending_bytes);
   BOOST_CHECK_THROW(
      byte_limited_worker.enqueue_task_with_size(async_worker::max_pending_bytes + 1, [] {}),
      std::runtime_error);
   byte_limited_worker.start();
   future.wait();
   byte_limited_worker.stop();

   // Reproduce the single-worker self-enqueue case: keep the worker inside one
   // task, fill the queue from this thread, then let that task try to enqueue.
   // The nested enqueue must return instead of waiting for itself to make room.
   async_worker count_limited_worker;
   std::promise<void> outer_started;
   auto outer_started_future = outer_started.get_future();
   std::promise<void> release_outer;
   auto release_future = release_outer.get_future().share();
   std::atomic<bool> nested_enqueue_succeeded{true};

   count_limited_worker.start();
   auto outer_future = count_limited_worker.enqueue_task([&] {
      outer_started.set_value();
      release_future.wait();
      nested_enqueue_succeeded = count_limited_worker.try_enqueue_task([] {});
   });
   outer_started_future.wait();

   for (size_t i = 0; i < async_worker::max_pending_tasks; ++i) {
      BOOST_REQUIRE(count_limited_worker.try_enqueue_task([] {}));
   }
   release_outer.set_value();
   outer_future.get();
   BOOST_CHECK(!nested_enqueue_succeeded.load());
   count_limited_worker.stop();
}

BOOST_AUTO_TEST_CASE(async_worker_required_tasks_apply_backpressure) {
   async_worker worker(1, 1024);
   BOOST_CHECK_EQUAL(worker.pending_task_limit(), 1u);
   BOOST_CHECK_EQUAL(worker.pending_byte_limit(), 1024u);

   std::promise<void> outer_started;
   auto outer_started_future = outer_started.get_future();
   std::promise<void> release_outer;
   auto release_outer_future = release_outer.get_future().share();
   std::mutex order_mutex;
   std::vector<int> execution_order;

   worker.start();
   auto outer = worker.enqueue_task([&] {
      outer_started.set_value();
      release_outer_future.wait();
      std::lock_guard<std::mutex> lock(order_mutex);
      execution_order.push_back(1);
   });
   outer_started_future.wait();

   auto queued = worker.enqueue_task([&] {
      std::lock_guard<std::mutex> lock(order_mutex);
      execution_order.push_back(2);
   });
   BOOST_CHECK_EQUAL(worker.pending_tasks(), 1u);

   auto producer = std::async(std::launch::async, [&] {
      return worker.enqueue_task_with_backpressure([&] {
         std::lock_guard<std::mutex> lock(order_mutex);
         execution_order.push_back(3);
      });
   });
   BOOST_CHECK(producer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

   release_outer.set_value();
   BOOST_REQUIRE(producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
   BOOST_CHECK(producer.get());
   outer.get();
   queued.get();
   worker.stop();

   const std::vector<int> expected{1, 2, 3};
   BOOST_CHECK_EQUAL_COLLECTIONS(execution_order.begin(), execution_order.end(),
                                 expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(async_worker_backpressure_stops_cleanly) {
   BOOST_CHECK_THROW(async_worker(0, 1), std::invalid_argument);
   BOOST_CHECK_THROW(async_worker(1, 0), std::invalid_argument);

   async_worker worker(1, 1);
   BOOST_REQUIRE(worker.try_enqueue_task_with_size(1, [] {}));

   auto producer = std::async(std::launch::async, [&] {
      return worker.enqueue_task_with_backpressure_and_size(1, [] {});
   });
   BOOST_CHECK(producer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

   worker.stop();
   BOOST_REQUIRE(producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
   BOOST_CHECK(!producer.get());
}

BOOST_AUTO_TEST_CASE(rollback_manager_operations) {
   fc::temp_directory temp_dir;
   auto db = std::make_shared<rocksdb_manager>();
   std::string test_path = (temp_dir.path() / "history").string();
   BOOST_REQUIRE(db->open(test_path));

   {
      rollback_manager manager(db);

      // Test creating rollback points
      BOOST_CHECK(manager.create_rollback_point(100));
      BOOST_CHECK(manager.create_rollback_point(200));
      BOOST_CHECK(manager.create_rollback_point(300));
      BOOST_CHECK(manager.has_rollback_point(100));
      BOOST_CHECK(!manager.has_rollback_point(400));
      BOOST_CHECK_EQUAL(manager.rollback_point_count(), 3u);

      // Test getting latest rollback point
      auto latest = manager.get_latest_rollback_point();
      BOOST_REQUIRE(latest.has_value());
      BOOST_CHECK_EQUAL(*latest, 300u);
   }

   // Rollback metadata and external checkpoint paths survive manager restart.
   rollback_manager reloaded_manager(db);
   auto latest = reloaded_manager.get_latest_rollback_point();
   BOOST_REQUIRE(latest.has_value());
   BOOST_CHECK_EQUAL(*latest, 300u);
   BOOST_CHECK_EQUAL(reloaded_manager.rollback_point_count(), 3u);

   BOOST_REQUIRE(reloaded_manager.rollback_to_block(200));
   rollback_manager after_rollback(db);
   latest = after_rollback.get_latest_rollback_point();
   BOOST_REQUIRE(latest.has_value());
   BOOST_CHECK_EQUAL(*latest, 200u);
   BOOST_CHECK_EQUAL(after_rollback.rollback_point_count(), 2u);

   // Test cleanup
   after_rollback.cleanup_old_rollback_points(1);
   BOOST_CHECK_EQUAL(after_rollback.rollback_point_count(), 1u);

   db->close();
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_checkpoint_rollback_preserves_database) {
   fc::temp_directory temp_dir;
   const auto database_path = temp_dir.path() / "history";
   rocksdb_manager manager;
   BOOST_REQUIRE(manager.open(database_path.string()));

   BOOST_REQUIRE(manager.put("value", "at-checkpoint"));
   BOOST_REQUIRE(manager.batch_write({
      {"_internal_last_accepted_block_num", "100"},
      {"_internal_last_accepted_block_id", "branch-at-checkpoint"}
   }, {}));
   BOOST_REQUIRE(manager.create_checkpoint(100));

   const std::filesystem::path checkpoint_path = manager.get_checkpoint_path(100);
   BOOST_CHECK(std::filesystem::exists(checkpoint_path / "CURRENT"));
   BOOST_CHECK(checkpoint_path.parent_path() != database_path);

   BOOST_REQUIRE(manager.put("value", "after-checkpoint"));
   BOOST_REQUIRE(manager.put("new-value", "must-disappear"));
   BOOST_REQUIRE(manager.put("_internal_last_accepted_block_id", "different-branch"));
   BOOST_REQUIRE(manager.rollback_to_block(100));

   std::string value;
   BOOST_REQUIRE(manager.get("value", value));
   BOOST_CHECK_EQUAL(value, "at-checkpoint");
   BOOST_CHECK(!manager.get("new-value", value));
   BOOST_REQUIRE(manager.get("_internal_last_accepted_block_id", value));
   BOOST_CHECK_EQUAL(value, "branch-at-checkpoint");
   BOOST_CHECK(std::filesystem::exists(checkpoint_path / "CURRENT"));

   manager.close();
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_rollback_excludes_live_queries) {
   fc::temp_directory temp_dir;
   rocksdb_manager manager;
   BOOST_REQUIRE(manager.open((temp_dir.path() / "history").string()));
   BOOST_REQUIRE(manager.put("value", "at-checkpoint"));
   BOOST_REQUIRE(manager.create_checkpoint(42));
   BOOST_REQUIRE(manager.put("value", "after-checkpoint"));

   auto query_lock = manager.acquire_read_lock();
   std::promise<void> rollback_started;
   auto rollback_started_future = rollback_started.get_future();
   auto rollback = std::async(std::launch::async, [&] {
      rollback_started.set_value();
      return manager.rollback_to_block(42);
   });
   rollback_started_future.wait();
   BOOST_CHECK(rollback.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

   query_lock.unlock();
   BOOST_REQUIRE(rollback.get());
   std::string value;
   BOOST_REQUIRE(manager.get("value", value));
   BOOST_CHECK_EQUAL(value, "at-checkpoint");
   manager.close();
}

BOOST_AUTO_TEST_CASE(rollback_manager_removes_only_irreversible_checkpoints) {
   fc::temp_directory temp_dir;
   auto db = std::make_shared<rocksdb_manager>();
   BOOST_REQUIRE(db->open((temp_dir.path() / "history").string()));

   rollback_manager manager(db);
   BOOST_REQUIRE(manager.create_rollback_point(100));
   BOOST_REQUIRE(manager.create_rollback_point(200));
   BOOST_REQUIRE(manager.create_rollback_point(300));
   manager.cleanup_irreversible_rollback_points(250);

   BOOST_CHECK(!std::filesystem::exists(db->get_checkpoint_path(100)));
   BOOST_CHECK(!std::filesystem::exists(db->get_checkpoint_path(200)));
   BOOST_CHECK(std::filesystem::exists(db->get_checkpoint_path(300)));
   const auto latest = manager.get_latest_rollback_point();
   BOOST_REQUIRE(latest.has_value());
   BOOST_CHECK_EQUAL(*latest, 300u);
   BOOST_CHECK_EQUAL(manager.rollback_point_count(), 1u);

   db->close();
}

BOOST_AUTO_TEST_CASE(rollback_manager_preserves_reversible_checkpoint_floor) {
   fc::temp_directory temp_dir;
   auto db = std::make_shared<rocksdb_manager>();
   BOOST_REQUIRE(db->open((temp_dir.path() / "history").string()));

   rollback_manager manager(db);
   BOOST_REQUIRE(manager.create_rollback_point(100));
   BOOST_REQUIRE(manager.create_rollback_point(200));
   BOOST_REQUIRE(manager.create_rollback_point(300));
   manager.cleanup_old_rollback_points(1, 0, 200);

   BOOST_CHECK(!manager.has_rollback_point(100));
   BOOST_CHECK(manager.has_rollback_point(200));
   BOOST_CHECK(manager.has_rollback_point(300));
   BOOST_CHECK_EQUAL(manager.rollback_point_count(), 2u);
   db->close();
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_recovers_interrupted_rollback_swap) {
   fc::temp_directory temp_dir;
   const auto database_path = temp_dir.path() / "history";
   const auto backup_path = temp_dir.path() / "history.rollback_backup";
   const auto staging_path = temp_dir.path() / "history.rollback_staging";
   rocksdb_manager manager;
   BOOST_REQUIRE(manager.open(database_path.string()));
   BOOST_REQUIRE(manager.put("value", "checkpoint"));
   BOOST_REQUIRE(manager.create_checkpoint(10));
   BOOST_REQUIRE(manager.put("value", "live-before-crash"));
   const auto checkpoint_path = manager.get_checkpoint_path(10);
   manager.close();

   // Simulate a crash after live -> backup and before staging -> live.
   std::filesystem::copy(checkpoint_path, staging_path,
                         std::filesystem::copy_options::recursive);
   std::filesystem::rename(database_path, backup_path);

   BOOST_REQUIRE(manager.open(database_path.string()));
   std::string value;
   BOOST_REQUIRE(manager.get("value", value));
   BOOST_CHECK_EQUAL(value, "live-before-crash");
   BOOST_CHECK(!std::filesystem::exists(backup_path));
   BOOST_CHECK(!std::filesystem::exists(staging_path));
   manager.close();
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_close_waits_for_active_query) {
   fc::temp_directory temp_dir;
   rocksdb_manager manager;
   BOOST_REQUIRE(manager.open((temp_dir.path() / "history").string()));

   auto query_lock = manager.acquire_read_lock();
   auto close_future = std::async(std::launch::async, [&manager] { manager.close(); });
   BOOST_CHECK(close_future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
   query_lock.unlock();
   BOOST_CHECK(close_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_force_clean_removes_chain_identity_and_checkpoints) {
   fc::temp_directory temp_dir;
   const auto database_path = temp_dir.path() / "history";
   rocksdb_manager manager;
   BOOST_REQUIRE(manager.open(database_path.string()));
   BOOST_REQUIRE(manager.put("trx:test", R"({"block_num":1})"));
   BOOST_REQUIRE(manager.put("_internal_last_accepted_block_num", "1"));
   BOOST_REQUIRE(manager.put("_internal_last_accepted_block_id", "old-chain"));
   BOOST_REQUIRE(manager.create_checkpoint(1));
   const auto checkpoint_root = std::filesystem::path(manager.get_checkpoint_path(1)).parent_path();

   BOOST_REQUIRE(manager.clear_all_data());
   std::string value;
   BOOST_CHECK(!manager.get("trx:test", value));
   BOOST_CHECK(!manager.get("_internal_last_accepted_block_num", value));
   BOOST_CHECK(!manager.get("_internal_last_accepted_block_id", value));
   BOOST_CHECK(!std::filesystem::exists(checkpoint_root));
   manager.close();
}

BOOST_AUTO_TEST_CASE(transaction_history_key_generation) {
   transaction_id_type test_id;
   name test_account("testaccount");
   uint64_t test_seq = 12345;
   uint32_t test_block = 67890;
   transaction_history_plugin plugin;

   // Test key generation
   std::string trx_key = plugin.make_transaction_key(test_id);
   BOOST_CHECK(trx_key.find("trx:") == 0);

   std::string acc_key = plugin.make_account_action_key(test_account, test_seq);
   BOOST_CHECK(acc_key.find("acc:testaccount:") == 0);
   BOOST_CHECK_EQUAL(acc_key, "acc:testaccount:00000000000000012345");

   std::string blk_key = plugin.make_block_transaction_key(test_block, test_id);
   BOOST_CHECK(blk_key.find("blk:0000067890:") == 0);
}

BOOST_AUTO_TEST_CASE(rocksdb_manager_error_handling) {
   rocksdb_manager manager;

   // Test operations on unopened database
   BOOST_CHECK(!manager.put("test_key", "test_value"));

   std::string value;
   BOOST_CHECK(!manager.get("test_key", value));

   // Test opening invalid path
   BOOST_CHECK(!manager.open(""));
   BOOST_CHECK(!manager.open("/dev/null/invalid"));
}

BOOST_AUTO_TEST_CASE(async_worker_error_handling) {
   async_worker worker;
   worker.start();

   std::atomic<bool> exception_caught{false};

   // Test task that throws exception
   auto future = worker.enqueue_task([&exception_caught]() {
      try {
         throw std::runtime_error("Test exception");
      } catch (...) {
         exception_caught = true;
         throw; // Re-throw to test exception handling
      }
   });

   // Exception should be propagated through future
   BOOST_CHECK_THROW(future.get(), std::runtime_error);
   BOOST_CHECK(exception_caught.load());

   worker.stop();
}

BOOST_AUTO_TEST_CASE(rollback_manager_edge_cases) {
   auto db = std::make_shared<rocksdb_manager>();
   std::string test_path = "/tmp/test_rollback_edge_" + std::to_string(std::time(nullptr));
   BOOST_REQUIRE(db->open(test_path));

   rollback_manager manager(db);

   // Test creating rollback point
   BOOST_CHECK(manager.create_rollback_point(100));

   // Test creating rollback point with different block number
   BOOST_CHECK(manager.create_rollback_point(101));

   // Test cleanup with zero retention
   manager.cleanup_old_rollback_points(0);

   // Cleanup always retains the newest point so rollback remains possible.
   BOOST_CHECK_EQUAL(manager.rollback_point_count(), 1u);
   auto latest = manager.get_latest_rollback_point();
   BOOST_REQUIRE(latest.has_value());
   BOOST_CHECK_EQUAL(*latest, 101u);

   db->close();
   std::filesystem::remove_all(test_path);
}

BOOST_AUTO_TEST_SUITE_END()
