#pragma once
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>
#include <fc/variant.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>
#include <memory>
#include <string>
#include <chrono>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace eosio {

struct history_undo_restore_entry {
   std::string key;
   std::string value;
};

struct history_block_undo_record {
   uint32_t block_num = 0;
   std::vector<history_undo_restore_entry> restore;
   std::vector<std::string> erase;
};

/**
 * @brief RocksDB Database Manager
 *
 * Provides a high-level interface for RocksDB operations including
 * basic CRUD operations, batch operations, and checkpoint management
 * for rollback functionality.
 */
class rocksdb_manager {
public:
   using database_read_lock = std::shared_lock<std::shared_mutex>;

   explicit rocksdb_manager(size_t block_cache_bytes = 256 * 1024 * 1024);
   ~rocksdb_manager();

   /**
    * @brief Open database at specified path
    * @param db_path Path to database directory
    * @return true if successful, false otherwise
    */
   bool open(const std::string& db_path);

   /**
    * @brief Open database at specified path with compression settings
    * @param db_path Path to database directory
    * @param enable_compression Whether to enable compression
    * @return true if successful, false otherwise
    */
   bool open(const std::string& db_path, bool enable_compression);

   /**
    * @brief Get database compression information
    * @return Information about compression usage in the database
    */
   std::string get_compression_info() const;

   /**
    * @brief Initialize database state management
    * @param starting_block_num Block number to start recording from
    * @param chain_head_block_num Current chain head block number
    * @param is_snapshot_load Whether this is loading from snapshot
    * @param is_replay Whether this is a replay operation
    * @return true if successful, false otherwise
    */
   bool initialize_database_state(uint32_t starting_block_num,
                                uint32_t chain_head_block_num,
                                bool is_snapshot_load = false,
                                bool is_replay = false);

   /**
    * @brief Get the last recorded block number in the database
    * @return Last block number, or 0 if none found
    */
   uint32_t get_last_block_number() const;

   /**
    * @brief Update the last recorded block number
    * @param block_num Block number to record
    * @return true if successful, false otherwise
    */
   bool update_last_block_number(uint32_t block_num);

   /**
    * @brief Clear database data from a specific block onwards
    * @param from_block_num Block number to clear from (inclusive)
    * @return true if successful, false otherwise
    */
   bool clear_from_block(uint32_t from_block_num);

   /**
    * @brief Clear all transaction history data (force clean)
    * @return true if successful, false otherwise
    */
   bool clear_all_data();

   /**
    * @brief Get database statistics and health information
    * @return JSON string with database statistics
    */
   std::string get_database_stats() const;

   /**
    * @brief Validate database integrity and fix minor issues
    * @return true if database is healthy or successfully repaired
    */
   bool validate_and_repair_database();

   /**
    * @brief Validate database integrity without changing live data
    * @return true when no repairable corruption or metadata lag is found
    */
   bool validate_database(const std::function<bool()>& should_cancel = {});

   /**
    * @brief Compact database to optimize performance and reclaim space
    * @return true if compaction succeeded
    */
   bool compact_database();

   /**
    * @brief Check database consistency and handle different startup scenarios
    * @param chain_head_block_num Current chain head block number
    * @param is_snapshot_load Whether this is loading from snapshot
    * @param is_replay Whether this is a replay operation
    * @return true if database is consistent or successfully repaired
    */
   bool check_and_repair_database_state(uint32_t chain_head_block_num,
                                       bool is_snapshot_load = false,
                                       bool is_replay = false);

   /**
    * @brief Get detailed performance metrics for the database
    * @return JSON string with performance metrics including operation counts, latencies, etc.
    */
   std::string get_performance_metrics() const;

   /**
    * @brief Get database size information broken down by data type
    * @return JSON string with size breakdown
    */
   std::string get_size_breakdown() const;

   /**
    * @brief Test database connectivity and basic operations
    * @return true if all basic operations work correctly
    */
   bool health_check() const;

   /**
    * @brief Get database configuration and tuning recommendations
    * @return JSON string with configuration analysis and recommendations
    */
   std::string get_tuning_recommendations() const;

   /**
    * @brief Estimate optimal cache size based on system memory and workload
    * @return Recommended cache size in bytes
    */
   size_t estimate_optimal_cache_size() const;

   /**
    * @brief Analyze key distribution and suggest optimization strategies
    * @return JSON string with distribution analysis and suggestions
    */
   /**
    * @brief Analyze key distribution patterns
    * @return JSON string with key distribution analysis
    */
   std::string analyze_key_distribution() const;

   /**
    * @brief Get database performance optimization suggestions
    * @return JSON string with optimization recommendations
    */
   std::string get_optimization_suggestions() const;

   /**
    * @brief Perform database fragmentation analysis
    * @return JSON string with fragmentation analysis results
    */
   std::string analyze_fragmentation() const;

   /**
    * @brief Get detailed cache statistics and recommendations
    * @return JSON string with cache analysis
    */
   std::string get_cache_analysis() const;

   /**
    * @brief Check if database needs maintenance
    * @return JSON string with maintenance recommendations
    */
   std::string check_maintenance_needs() const;

   /** Keep the live DB instance stable for a complete API query. */
   database_read_lock acquire_read_lock() const { return database_read_lock(db_lifecycle_mutex_); }

private:
   bool validate_database_impl(bool repair, const std::function<bool()>& should_cancel = {});
   bool open_unlocked(const std::string& db_path, bool enable_compression,
                      bool recover_interrupted_swap);
   void close_unlocked();

   std::unique_ptr<rocksdb::DB> db_;
   rocksdb::Options options_;
   std::string db_path_;
   bool is_open_;
   mutable std::mutex stats_cache_mutex_;
   mutable std::shared_mutex db_lifecycle_mutex_;
   mutable std::string stats_cache_;
   mutable std::chrono::steady_clock::time_point stats_cache_time_;

   /**
    * @brief Internal helper to calculate optimal write buffer size
    */
   size_t calculate_optimal_write_buffer_size() const;

   /**
    * @brief Internal helper to determine if compaction is needed
    */
   bool needs_compaction() const;

public:
   /**
    * @brief Close the database
    */
   void close();

   /**
    * @brief Store an object with the given key
    * @tparam T The type of object to store
    * @param key The key to store the object under
    * @param obj The object to store
    * @return true if successful, false otherwise
    */
   template<typename T>
   bool put_object(const std::string& key, const T& obj) {
      if (!db_) return false;
      try {
         fc::variant var;
         fc::to_variant(obj, var);
         std::string value = fc::json::to_string(var, fc::time_point::maximum());
         rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
         return status.ok();
      } catch (const std::exception& e) {
         wlog("Failed to store object: ${e}", ("e", e.what()));
         return false;
      }
   }

   /**
    * @brief Retrieve an object with the given key
    * @tparam T The type of object to retrieve
    * @param key The key of the object to retrieve
    * @param obj Reference to store the retrieved object
    * @return true if successful, false otherwise
    */
   template<typename T>
   bool get_object(const std::string& key, T& obj) {
      if (!db_) return false;
      try {
         std::string value;
         rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &value);
         if (!status.ok()) {
            return false;
         }
         fc::variant var = fc::json::from_string(value);
         fc::from_variant(var, obj);
         return true;
      } catch (const std::exception& e) {
         wlog("Failed to retrieve object: ${e}", ("e", e.what()));
         return false;
      }
   }

   /**
    * @brief Store a key-value pair
    * @param key The key to store
    * @param value The value to store
    * @return true if successful, false otherwise
    */
   bool put(const std::string& key, const std::string& value);

   /**
    * @brief Retrieve value for a key
    * @param key The key to lookup
    * @param value Output parameter for the value
    * @return true if key found, false otherwise
    */
   bool get(const std::string& key, std::string& value);

   /** Retrieve a group of keys using RocksDB's batched read path. */
   std::vector<rocksdb::Status> multi_get(const std::vector<std::string>& keys,
                                          std::vector<std::string>& values) const;

   /**
    * @brief Delete a key
    * @param key The key to delete
    * @return true if successful, false otherwise
    */
   bool remove(const std::string& key);

   /**
    * @brief Create a new iterator for database scanning
    * @return Raw iterator pointer (caller owns it)
    */
   rocksdb::Iterator* new_iterator() const;
   rocksdb::Iterator* new_iterator(const rocksdb::ReadOptions& options) const;

   /**
    * @brief Create a checkpoint for rollback
    * @param block_num Block number for checkpoint
    * @return true if successful, false otherwise
    */
   bool create_checkpoint(uint32_t block_num);

   /**
    * @brief Rollback database to a specific block
    * @param block_num Block number to rollback to
    * @return true if successful, false otherwise
    */
   bool rollback_to_block(uint32_t block_num);

   /**
    * Atomically write one accepted block together with the inverse mutations
    * needed to restore its parent. Public history keys are append-only; only
    * `_internal_` keys are read before the batch to preserve overwritten
    * values in the undo record.
    */
   bool batch_write_with_undo(
      uint32_t block_num,
      const std::vector<std::pair<std::string, std::string>>& writes,
      const std::vector<std::string>& deletes = {},
      uint64_t max_total_bytes = 0,
      uint64_t* total_bytes = nullptr,
      bool sync = false);

   /** Create an empty, durable undo anchor for an existing chain baseline. */
   bool create_undo_baseline(uint32_t block_num);

   /** Roll back all undo records newer than block_num, newest first. */
   bool rollback_with_undo(uint32_t block_num);

   bool has_undo_point(uint32_t block_num) const;
   bool remove_undo_point(uint32_t block_num);
   std::vector<uint32_t> list_undo_points() const;

   /**
    * @brief Get the path used to store a checkpoint
    * @param block_num Block number for checkpoint
    * @return Checkpoint path outside the live database directory
    */
   std::string get_checkpoint_path(uint32_t block_num) const;

   /**
    * @brief Get database path
    * @return Database path string
    */
   const std::string& get_db_path() const { return db_path_; }

   /**
    * @brief Perform batch write operations
    * @param writes Vector of key-value pairs to write
    * @param deletes Vector of keys to delete
    * @return true if successful, false otherwise
    */
   bool batch_write(const std::vector<std::pair<std::string, std::string>>& writes,
                   const std::vector<std::string>& deletes = {},
                   bool sync = false);

private:
   // Database instance and configuration
   bool compression_enabled_;
};

} // namespace eosio

FC_REFLECT(eosio::history_undo_restore_entry, (key)(value))
FC_REFLECT(eosio::history_block_undo_record, (block_num)(restore)(erase))
