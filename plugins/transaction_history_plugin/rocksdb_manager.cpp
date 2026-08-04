#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <fc/log/logger.hpp>
#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/statistics.h>
#include <rocksdb/version.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace eosio {

namespace {

bool extract_json_block_num(const std::string& value, uint32_t& block_num) {
   try {
      fc::variant data = fc::json::from_string(value);
      if (!data.is_object()) {
         return false;
      }

      const auto& obj = data.get_object();
      if (!obj.contains("block_num")) {
         return false;
      }

      block_num = obj["block_num"].as<uint32_t>();
      return true;
   } catch (...) {
      return false;
   }
}

void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
   const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
   if (fd >= 0) {
      if (::fsync(fd) != 0) {
         wlog("Failed to fsync directory ${path} after rollback rename", ("path", path.string()));
      }
      ::close(fd);
   }
#else
   (void)path;
#endif
}

void clone_checkpoint_for_rollback(const std::filesystem::path& source,
                                   const std::filesystem::path& destination) {
   std::filesystem::create_directories(destination);
   for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
      const auto relative = std::filesystem::relative(entry.path(), source);
      const auto target = destination / relative;
      if (entry.is_directory()) {
         std::filesystem::create_directories(target);
      } else if (entry.is_regular_file()) {
         std::filesystem::create_directories(target.parent_path());
         const auto extension = entry.path().extension();
         if (extension == ".sst" || extension == ".ldb") {
            // RocksDB table files are immutable. Preserve the checkpoint's
            // hard-link behavior instead of copying the complete database.
            std::filesystem::create_hard_link(entry.path(), target);
         } else {
            std::filesystem::copy_file(entry.path(), target,
                                       std::filesystem::copy_options::overwrite_existing);
         }
      } else {
         throw std::runtime_error("unsupported file type in RocksDB checkpoint: " +
                                  entry.path().string());
      }
   }
}

} // namespace

rocksdb_manager::rocksdb_manager() : is_open_(false) {
   // Configure RocksDB options for optimal performance
   options_.create_if_missing = true;
   options_.write_buffer_size = 64 * 1024 * 1024;  // 64MB
   options_.max_write_buffer_number = 3;
   options_.target_file_size_base = 64 * 1024 * 1024;  // 64MB
   const auto hardware_threads = std::max(1u, std::thread::hardware_concurrency());
   const int background_jobs = static_cast<int>(
      std::min(8u, std::max(1u, hardware_threads / 2)));
   options_.IncreaseParallelism(background_jobs);
   options_.max_background_jobs = background_jobs;
   options_.statistics = rocksdb::CreateDBStatistics();
   // Metrics are pulled by the plugin; periodic RocksDB LOG dumps add noise
   // and can create avoidable I/O on busy nodes.
   options_.stats_dump_period_sec = 0;
}

rocksdb_manager::~rocksdb_manager() {
   close();
}

bool rocksdb_manager::open(const std::string& db_path) {
   return open(db_path, true); // Default to compression enabled
}

bool rocksdb_manager::open(const std::string& db_path, bool enable_compression) {
   std::unique_lock<std::shared_mutex> lifecycle_lock(db_lifecycle_mutex_);
   return open_unlocked(db_path, enable_compression, true);
}

bool rocksdb_manager::open_unlocked(const std::string& db_path, bool enable_compression,
                                    bool recover_interrupted_swap) {
   if (is_open_) {
      return true;
   }

   db_path_ = db_path;

   // Configure compression based on parameter
   if (enable_compression) {
      options_.compression = rocksdb::kLZ4Compression;
      ilog("RocksDB compression enabled (LZ4)");
   } else {
      options_.compression = rocksdb::kNoCompression;
      ilog("RocksDB compression disabled");
   }

   // Recover an interrupted directory swap before create_if_missing has a
   // chance to silently create an empty history database.
   try {
      const auto database_path = std::filesystem::absolute(db_path).lexically_normal();
      const auto parent = database_path.parent_path();
      const auto basename = database_path.filename().string();
      const auto backup_path = parent / (basename + ".rollback_backup");
      const auto staging_path = parent / (basename + ".rollback_staging");
      const auto complete = [](const std::filesystem::path& path) {
         return std::filesystem::is_directory(path) &&
                std::filesystem::exists(path / "CURRENT");
      };

      if (recover_interrupted_swap && !complete(database_path) && complete(backup_path)) {
         std::error_code ignored;
         std::filesystem::remove_all(database_path, ignored);
         std::filesystem::rename(backup_path, database_path);
         sync_directory(parent);
         std::filesystem::remove_all(staging_path, ignored);
         wlog("Recovered transaction history database from interrupted rollback backup at ${path}",
              ("path", backup_path.string()));
      } else if (recover_interrupted_swap && !complete(database_path) && complete(staging_path)) {
         std::error_code ignored;
         std::filesystem::remove_all(database_path, ignored);
         std::filesystem::rename(staging_path, database_path);
         sync_directory(parent);
         wlog("Completed interrupted transaction history rollback from staging at ${path}",
              ("path", staging_path.string()));
      }
      std::filesystem::create_directories(db_path);
   } catch (const std::exception& e) {
      elog("Failed to create database directory ${path}: ${error}",
           ("path", db_path)("error", e.what()));
      return false;
   }

   // Check if database already exists to warn about compression changes
   bool db_exists = std::filesystem::exists(db_path + "/CURRENT");

   const auto open_database = [&]() {
#if ROCKSDB_MAJOR >= 11
      return rocksdb::DB::Open(options_, db_path, &db_);
#else
      rocksdb::DB* raw_db = nullptr;
      rocksdb::Status open_status = rocksdb::DB::Open(options_, db_path, &raw_db);
      if (open_status.ok()) {
         db_.reset(raw_db);
      }
      return open_status;
#endif
   };
   rocksdb::Status status = open_database();
   if (!status.ok()) {
      const auto database_path = std::filesystem::absolute(db_path).lexically_normal();
      const auto backup_path = database_path.parent_path() /
         (database_path.filename().string() + ".rollback_backup");
      if (recover_interrupted_swap && std::filesystem::exists(backup_path / "CURRENT")) {
         try {
            std::filesystem::remove_all(database_path);
            std::filesystem::rename(backup_path, database_path);
            sync_directory(database_path.parent_path());
            wlog("Restoring rollback backup after replacement database failed to open: ${error}",
                 ("error", status.ToString()));
            status = open_database();
         } catch (const std::exception& recovery_error) {
            elog("Failed to restore rollback backup after open error: ${error}",
                 ("error", recovery_error.what()));
         }
      }
   }
   if (!status.ok()) {
      elog("Failed to open RocksDB: ${error}", ("error", status.ToString()));
      return false;
   }

   is_open_ = true;
   {
      std::lock_guard<std::mutex> lock(stats_cache_mutex_);
      stats_cache_.clear();
   }

   // Store compression setting in a special key for future reference
   std::string compression_key = "_internal_compression_enabled";
   std::string current_setting = enable_compression ? "true" : "false";

   if (db_exists) {
      // Check previous compression setting
      std::string previous_setting;
      rocksdb::Status read_status = db_->Get(rocksdb::ReadOptions(), compression_key, &previous_setting);

      if (read_status.ok() && previous_setting != current_setting) {
         if (enable_compression && previous_setting == "false") {
            wlog("Transaction history database was previously created without compression, "
                 "but compression is now enabled. New data will be compressed, "
                 "but existing data remains uncompressed.");
         } else if (!enable_compression && previous_setting == "true") {
            wlog("Transaction history database was previously created with compression, "
                 "but compression is now disabled. Existing compressed data can still be read, "
                 "but new data will not be compressed.");
         }
      }
   }

   // Update the compression setting record
   rocksdb::Status write_status = db_->Put(rocksdb::WriteOptions(), compression_key, current_setting);
   if (!write_status.ok()) {
      wlog("Failed to record compression setting: ${error}", ("error", write_status.ToString()));
   }

   ilog("RocksDB opened successfully at: ${path}", ("path", db_path));

   // A complete live database plus a leftover backup means the replacement
   // was installed successfully and the process stopped before cleanup.
   const auto database_path = std::filesystem::absolute(db_path).lexically_normal();
   const auto backup_path = database_path.parent_path() /
      (database_path.filename().string() + ".rollback_backup");
   const auto staging_path = database_path.parent_path() /
      (database_path.filename().string() + ".rollback_staging");
   if (recover_interrupted_swap) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(backup_path, cleanup_error);
      cleanup_error.clear();
      std::filesystem::remove_all(staging_path, cleanup_error);
   }
   return true;
}

void rocksdb_manager::close() {
   std::unique_lock<std::shared_mutex> lifecycle_lock(db_lifecycle_mutex_);
   close_unlocked();
}

void rocksdb_manager::close_unlocked() {
   std::lock_guard<std::mutex> lock(stats_cache_mutex_);
   if (db_) {
      db_.reset();
      is_open_ = false;
      stats_cache_.clear();
   }
}

bool rocksdb_manager::put(const std::string& key, const std::string& value) {
   if (!db_) return false;

   // Retry logic for transient failures
   const int MAX_RETRIES = 3;
   const int RETRY_DELAY_MS = 100;

   for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
      rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);

      if (status.ok()) {
         return true;
      }

      // Check if this is a retryable error
      if (status.IsIOError() || status.IsBusy() || status.IsTimedOut()) {
         if (attempt < MAX_RETRIES - 1) {
            wlog("RocksDB put retry ${attempt}/${max} for key ${key}: ${error}",
                 ("attempt", attempt + 1)("max", MAX_RETRIES)("key", key)("error", status.ToString()));
            std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS * (attempt + 1)));
            continue;
         }
      }

      elog("RocksDB put failed for key ${key}: ${error}",
           ("key", key)("error", status.ToString()));
      return false;
   }

   return false;
}

bool rocksdb_manager::get(const std::string& key, std::string& value) {
   if (!db_) return false;

   // Add retry logic for transient read failures
   const int max_retries = 3;
   for (int retry = 0; retry < max_retries; ++retry) {
      rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &value);

      if (status.ok()) {
         return true;
      }

      if (status.IsNotFound()) {
         return false; // Key doesn't exist, no point in retrying
      }

      // For other errors, implement retry with exponential backoff
      if (retry < max_retries - 1) {
         wlog("RocksDB get error for key ${key} (attempt ${attempt}/${max}): ${error}",
              ("key", key)("attempt", retry + 1)("max", max_retries)("error", status.ToString()));
         std::this_thread::sleep_for(std::chrono::milliseconds(10 << retry)); // Exponential backoff
      } else {
         elog("RocksDB get failed for key ${key} after ${max} attempts: ${error}",
              ("key", key)("max", max_retries)("error", status.ToString()));
      }
   }

   return false;
}

std::string rocksdb_manager::get_compression_info() const {
   if (!db_) {
      return "Database not open";
   }

   std::string compression_setting;
   std::string compression_key = "_internal_compression_enabled";
   rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), compression_key, &compression_setting);

   std::string current_compression = (options_.compression == rocksdb::kLZ4Compression) ? "LZ4" : "None";

   if (status.ok()) {
      bool was_compressed = (compression_setting == "true");
      std::string previous_compression = was_compressed ? "LZ4" : "None";

      if (current_compression != previous_compression) {
         return "Current: " + current_compression + ", Previous: " + previous_compression + " (Mixed mode)";
      } else {
         return "Consistent: " + current_compression;
      }
   } else {
      return "Current: " + current_compression + " (No history available)";
   }
}

bool rocksdb_manager::initialize_database_state(uint32_t starting_block_num,
                                               uint32_t chain_head_block_num,
                                               bool is_snapshot_load,
                                               bool is_replay) {
   if (!db_) {
      elog("Cannot initialize database state: database not open");
      return false;
   }

   // Check current database state
   uint32_t db_last_block = get_last_block_number();

   if (is_snapshot_load) {
      if (db_last_block == 0) {
         // Case 1: Clean start from snapshot
         ilog("Initializing transaction history from snapshot at block ${block}",
              ("block", starting_block_num));
         return update_last_block_number(starting_block_num);
      } else {
         // Case 2: Existing database with snapshot load
         if (db_last_block <= chain_head_block_num) {
            // Database is behind or at snapshot level - this is expected
            if (db_last_block < starting_block_num) {
               // Gap exists, warn but continue
               wlog("Transaction history database last block ${db_block} is behind snapshot start ${snap_block}. "
                    "Some historical transactions may be missing.",
                    ("db_block", db_last_block)("snap_block", starting_block_num));
            }
            return true; // Continue from current state
         } else {
            // Database is ahead of snapshot - clear future data
            wlog("Transaction history database contains blocks beyond snapshot point. "
                 "Clearing data from block ${snap_block} onwards (was at block ${db_block})",
                 ("snap_block", starting_block_num)("db_block", db_last_block));

            if (!clear_from_block(starting_block_num)) {
               elog("Failed to clear future blocks from transaction history database");
               return false;
            }
            return update_last_block_number(starting_block_num);
         }
      }
   } else if (is_replay) {
      // Case 3: Replay operation
      uint32_t replay_start = std::min(starting_block_num, chain_head_block_num);

      if (db_last_block >= replay_start) {
         ilog("Replay detected: clearing transaction history from block ${replay_start} onwards "
              "(database was at block ${db_block})",
              ("replay_start", replay_start)("db_block", db_last_block));

         if (!clear_from_block(replay_start)) {
            elog("Failed to clear blocks for replay from transaction history database");
            return false;
         }
         return update_last_block_number(replay_start > 0 ? replay_start - 1 : 0);
      } else {
         // Database is already behind replay point, continue normally
         ilog("Replay detected: transaction history database is already behind replay point");
         return true;
      }
   } else {
      // Case 4: Normal startup
      if (db_last_block > chain_head_block_num) {
         // Database is ahead of chain - this indicates inconsistency
         wlog("Transaction history database (block ${db_block}) is ahead of chain head (block ${chain_block}). "
              "Clearing future data to maintain consistency.",
              ("db_block", db_last_block)("chain_block", chain_head_block_num));

         if (!clear_from_block(chain_head_block_num + 1)) {
            elog("Failed to clear future blocks from transaction history database");
            return false;
         }
         return update_last_block_number(chain_head_block_num);
      }

      // Normal case - database is at or behind chain head
      return true;
   }
}

uint32_t rocksdb_manager::get_last_block_number() const {
   if (!db_) return 0;

   std::string value;
   std::string key = "_internal_last_block_number";
   rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &value);

   if (status.ok()) {
      try {
         return std::stoul(value);
      } catch (const std::exception& e) {
         wlog("Failed to parse last block number from database: ${error}", ("error", e.what()));
         return 0;
      }
   }

   return 0; // Not found or error
}

bool rocksdb_manager::update_last_block_number(uint32_t block_num) {
   if (!db_) return false;

   std::string key = "_internal_last_block_number";
   std::string value = std::to_string(block_num);

   rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
   if (!status.ok()) {
      elog("Failed to update last block number: ${error}", ("error", status.ToString()));
      return false;
   }

   return true;
}

bool rocksdb_manager::clear_from_block(uint32_t from_block_num) {
   if (!db_) return false;

   try {
      std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
      rocksdb::WriteBatch batch;
      constexpr size_t max_batch_keys = 10000;
      constexpr size_t max_batch_bytes = 16 * 1024 * 1024;
      size_t batch_keys = 0, batch_bytes = 0, deleted = 0;
      size_t blocks_checked = 0, transactions_checked = 0, accounts_checked = 0;

      const auto flush = [&]() -> bool {
         if (batch_keys == 0) return true;
         const auto status = db_->Write(rocksdb::WriteOptions(), &batch);
         if (!status.ok()) {
            elog("Failed to clear blocks from database: ${error}", ("error", status.ToString()));
            return false;
         }
         deleted += batch_keys;
         batch.Clear();
         batch_keys = batch_bytes = 0;
         return true;
      };

      // The iterator keeps a stable RocksDB view while bounded batches are
      // committed, so no key list grows with database size.
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
         std::string key = it->key().ToString();

         // Skip internal keys
         if (key.find("_internal_") == 0) {
            continue;
         }

         bool should_delete = false;

         if (key.find("blk:") == 0) {
            // Block-based key: "blk:block_num:..."
            blocks_checked++;
            size_t first_colon = key.find(':', 4);
            if (first_colon != std::string::npos) {
               try {
                  uint32_t key_block_num = std::stoul(key.substr(4, first_colon - 4));
                  should_delete = (key_block_num >= from_block_num);
               } catch (...) {
                  // Skip malformed keys
               }
            }
         } else if (key.find("trx:") == 0) {
            // Transaction key - read the data to check block number
            transactions_checked++;
            std::string value = it->value().ToString();
            uint32_t block_num = 0;
            if (extract_json_block_num(value, block_num)) {
               should_delete = (block_num >= from_block_num);
            }
         } else if (key.find("acc:") == 0) {
            // Account action key - similar approach
            accounts_checked++;
            std::string value = it->value().ToString();
            uint32_t block_num = 0;
            if (extract_json_block_num(value, block_num)) {
               should_delete = (block_num >= from_block_num);
            }
         }

         if (should_delete) {
            batch.Delete(key);
            ++batch_keys;
            batch_bytes += key.size();
            if ((batch_keys >= max_batch_keys || batch_bytes >= max_batch_bytes) && !flush()) {
               return false;
            }
         }
      }

      if (!it->status().ok()) {
         elog("Failed while scanning database for block cleanup: ${error}",
              ("error", it->status().ToString()));
         return false;
      }

      if (!flush()) return false;
      if (deleted != 0) {
         ilog("Database cleanup completed: cleared ${count} entries from block ${from_block} onwards "
              "(scanned ${blocks} block keys, ${transactions} transaction keys, ${accounts} account keys)",
              ("count", deleted)("from_block", from_block_num)
              ("blocks", blocks_checked)("transactions", transactions_checked)("accounts", accounts_checked));
      } else {
         ilog("Database cleanup: no entries found to clear from block ${from_block} onwards "
              "(scanned ${blocks} block keys, ${transactions} transaction keys, ${accounts} account keys)",
              ("from_block", from_block_num)("blocks", blocks_checked)
              ("transactions", transactions_checked)("accounts", accounts_checked));
      }

      return true;

   } catch (const std::exception& e) {
      elog("Exception during database cleanup: ${error}", ("error", e.what()));
      return false;
   }
}

bool rocksdb_manager::clear_all_data() {
   if (!db_) return false;

   try {
      std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
      rocksdb::WriteBatch batch;
      constexpr size_t max_batch_keys = 10000;
      constexpr size_t max_batch_bytes = 16 * 1024 * 1024;
      size_t total_keys = 0, deleted = 0, batch_keys = 0, batch_bytes = 0;
      const auto flush = [&]() -> bool {
         if (batch_keys == 0) return true;
         const auto status = db_->Write(rocksdb::WriteOptions(), &batch);
         if (!status.ok()) {
            elog("Failed to clear data batch: ${error}", ("error", status.ToString()));
            return false;
         }
         deleted += batch_keys;
         batch.Clear();
         batch_keys = batch_bytes = 0;
         return true;
      };

      // Collect all non-internal keys for deletion
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
         std::string key = it->key().ToString();
         total_keys++;

         // Only the immutable storage-format setting survives a force clean.
         if (key != "_internal_compression_enabled") {
            batch.Delete(key);
            ++batch_keys;
            batch_bytes += key.size();
            if ((batch_keys >= max_batch_keys || batch_bytes >= max_batch_bytes) && !flush()) {
               return false;
            }
         }
      }

      if (!it->status().ok()) {
         elog("Failed while scanning database for force clean: ${error}",
              ("error", it->status().ToString()));
         return false;
      }

      if (!flush()) return false;
      if (deleted != 0) {
         ilog("Force clean completed: cleared ${count} transaction history entries (${total} total keys scanned)",
              ("count", deleted)("total", total_keys));
      } else {
         ilog("Force clean: no transaction data found to clear");
      }

      std::error_code checkpoint_error;
      std::filesystem::remove_all(
         std::filesystem::path(get_checkpoint_path(0)).parent_path(), checkpoint_error);
      if (checkpoint_error) {
         elog("Failed to remove rollback checkpoints during force clean: ${error}",
              ("error", checkpoint_error.message()));
         return false;
      }
      return update_last_block_number(0);

   } catch (const std::exception& e) {
      elog("Exception during force clean: ${error}", ("error", e.what()));
      return false;
   }
}

std::string rocksdb_manager::get_database_stats() const {
   std::lock_guard<std::mutex> cache_lock(stats_cache_mutex_);
   if (!db_) {
      return R"({"status": "closed", "message": "Database not open"})";
   }

   const auto now = std::chrono::steady_clock::now();
   if (!stats_cache_.empty() && now - stats_cache_time_ < std::chrono::seconds(1)) {
      return stats_cache_;
   }

   try {
      auto read_uint_property = [this](const char* property) -> uint64_t {
         std::string value;
         if (!db_->GetProperty(property, &value)) {
            return 0;
         }
         try {
            return std::stoull(value);
         } catch (...) {
            return 0;
         }
      };

      // RocksDB properties are constant-time metadata reads. Do not iterate the
      // user keyspace from an HTTP-facing statistics request.
      const uint64_t total_size = read_uint_property("rocksdb.total-sst-files-size");
      const uint64_t estimated_keys = read_uint_property("rocksdb.estimate-num-keys");
      const uint64_t live_versions = read_uint_property("rocksdb.num-live-versions");
      const uint64_t pending_compaction =
         read_uint_property("rocksdb.estimate-pending-compaction-bytes");
      const uint32_t last_block = get_last_block_number();
      const std::string compression_info = get_compression_info();

      fc::mutable_variant_object stats;
      stats["status"] = "open";
      stats["database_path"] = db_path_;
      stats["last_recorded_block"] = last_block;
      stats["estimated_total_keys"] = estimated_keys;
      stats["total_size_bytes"] = total_size;
      stats["total_size_mb"] = total_size / (1024.0 * 1024.0);
      stats["compression_info"] = compression_info;
      stats["rocksdb_metadata"] = fc::mutable_variant_object()
         ("num_live_versions", live_versions)
         ("estimated_pending_compaction_bytes", pending_compaction)
         ("key_count_is_estimate", true);
      stats["health"] = fc::mutable_variant_object()
         ("database_open", true)
         ("compression_consistent", compression_info.find("Mixed mode") == std::string::npos)
         ("has_recorded_blocks", last_block > 0);

      stats_cache_ = fc::json::to_string(fc::variant(stats), fc::time_point::maximum());
      stats_cache_time_ = now;
      return stats_cache_;

   } catch (const std::exception& e) {
      fc::mutable_variant_object error;
      error["status"] = "error";
      error["message"] = e.what();
      return fc::json::to_string(fc::variant(error), fc::time_point::maximum());
   }
}

bool rocksdb_manager::validate_and_repair_database() {
   if (!db_) {
      elog("Cannot validate database: database not open");
      return false;
   }

   try {
      ilog("Starting database validation and repair process...");

      bool repairs_made = false;
      size_t invalid_keys_found = 0;
      size_t orphaned_data_found = 0;

      std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
      rocksdb::WriteBatch repair_batch;
      constexpr size_t max_repair_batch_keys = 10000;
      constexpr size_t max_repair_batch_bytes = 16 * 1024 * 1024;
      // Keep validation consistent with the plugin's accepted write limit. A
      // corrupt database can otherwise force an unbounded allocation through
      // Slice::ToString() before the record is rejected.
      constexpr size_t max_history_record_bytes = 256 * 1024 * 1024;
      size_t repair_batch_keys = 0;
      size_t repair_batch_bytes = 0;
      const auto flush_repairs = [&]() -> bool {
         if (repair_batch_keys == 0) return true;
         const rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &repair_batch);
         if (!status.ok()) {
            elog("Failed to remove invalid keys: ${error}", ("error", status.ToString()));
            return false;
         }
         repairs_made = true;
         repair_batch.Clear();
         repair_batch_keys = 0;
         repair_batch_bytes = 0;
         return true;
      };

      uint32_t last_recorded_block = get_last_block_number();
      uint32_t highest_block_found = 0;

      // Scan all keys for validation
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
         std::string key = it->key().ToString();

         // Skip internal keys from validation
         if (key.find("_internal_") == 0) {
            continue;
         }

         bool key_is_valid = true;
         uint32_t key_block_num = 0;

         if (key.find("trx:") == 0) {
            // Validate transaction data
            const auto value = it->value();
            if (value.size() <= max_history_record_bytes &&
                extract_json_block_num(value.ToString(), key_block_num)) {
               highest_block_found = std::max(highest_block_found, key_block_num);
            } else {
               key_is_valid = false;
               invalid_keys_found++;
            }
         } else if (key.find("acc:") == 0) {
            // Validate account action data
            const auto value = it->value();
            if (value.size() <= max_history_record_bytes &&
                extract_json_block_num(value.ToString(), key_block_num)) {
               highest_block_found = std::max(highest_block_found, key_block_num);
            } else {
               key_is_valid = false;
               invalid_keys_found++;
            }
         } else if (key.find("blk:") == 0) {
            // Validate block-based keys
            size_t first_colon = key.find(':', 4);
            if (first_colon != std::string::npos) {
               try {
                  key_block_num = std::stoul(key.substr(4, first_colon - 4));
                  highest_block_found = std::max(highest_block_found, key_block_num);
               } catch (...) {
                  key_is_valid = false;
                  invalid_keys_found++;
               }
            } else {
               key_is_valid = false;
               invalid_keys_found++;
            }
         }

         // Check for orphaned data (blocks beyond last recorded)
         if (key_is_valid && last_recorded_block > 0 && key_block_num > last_recorded_block + 1000) {
            // Data is suspiciously far ahead - might be orphaned
            orphaned_data_found++;
            dlog("Found potentially orphaned data: key ${key} at block ${block}, last recorded ${last}",
                 ("key", key)("block", key_block_num)("last", last_recorded_block));
         }

         // Mark invalid keys for deletion
         if (!key_is_valid) {
            repair_batch.Delete(key);
            ++repair_batch_keys;
            repair_batch_bytes += key.size();
            if ((repair_batch_keys >= max_repair_batch_keys ||
                 repair_batch_bytes >= max_repair_batch_bytes) && !flush_repairs()) {
               return false;
            }
         }
      }

      if (!it->status().ok()) {
         elog("Failed while scanning database for validation: ${error}",
              ("error", it->status().ToString()));
         return false;
      }

      if (!flush_repairs()) return false;

      // Update last block number if we found a higher valid block
      if (highest_block_found > last_recorded_block) {
         ilog("Updating last recorded block from ${old} to ${new}",
              ("old", last_recorded_block)("new", highest_block_found));
         if (update_last_block_number(highest_block_found)) {
            repairs_made = true;
         }
      }

      ilog("Database validation completed: ${invalid} invalid keys, ${orphaned} potentially orphaned entries, repairs made: ${repairs}",
           ("invalid", invalid_keys_found)("orphaned", orphaned_data_found)("repairs", repairs_made));

      return true;

   } catch (const std::exception& e) {
      elog("Exception during database validation: ${error}", ("error", e.what()));
      return false;
   }
}

bool rocksdb_manager::compact_database() {
   if (!db_) {
      elog("Cannot compact database: database not open");
      return false;
   }

   try {
      ilog("Starting database compaction...");

      // Get size before compaction
      std::string size_before_str;
      size_t size_before = 0;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &size_before_str)) {
         try {
            size_before = std::stoull(size_before_str);
         } catch (...) {}
      }

      // Perform full compaction
      rocksdb::CompactRangeOptions options;
      options.allow_write_stall = true;
      options.exclusive_manual_compaction = false;

      rocksdb::Status status = db_->CompactRange(options, nullptr, nullptr);

      if (!status.ok()) {
         elog("Database compaction failed: ${error}", ("error", status.ToString()));
         return false;
      }

      // Get size after compaction
      std::string size_after_str;
      size_t size_after = 0;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &size_after_str)) {
         try {
            size_after = std::stoull(size_after_str);
         } catch (...) {}
      }

      double reduction_percent = 0.0;
      if (size_before > 0) {
         reduction_percent = ((static_cast<double>(size_before) - static_cast<double>(size_after)) /
                              static_cast<double>(size_before)) * 100.0;
      }

      ilog("Database compaction completed: size reduced from ${before} to ${after} bytes (${percent}% reduction)",
           ("before", size_before)("after", size_after)("percent", reduction_percent));

      return true;

   } catch (const std::exception& e) {
      elog("Exception during database compaction: ${error}", ("error", e.what()));
      return false;
   }
}

bool rocksdb_manager::check_and_repair_database_state(uint32_t chain_head_block_num,
                                                     bool is_snapshot_load,
                                                     bool is_replay) {
   if (!db_) {
      elog("Cannot check database state: database not open");
      return false;
   }

   uint32_t db_last_block = get_last_block_number();

   // Determine appropriate starting block based on scenario
   uint32_t starting_block_num = 1; // Default

   if (is_snapshot_load) {
      starting_block_num = chain_head_block_num;
   } else if (is_replay) {
      // For replay, we might want to start from an earlier point
      starting_block_num = std::max(1u, chain_head_block_num);
   } else {
      // Normal startup - continue from where we left off
      starting_block_num = db_last_block + 1;
   }

   return initialize_database_state(starting_block_num, chain_head_block_num,
                                  is_snapshot_load, is_replay);
}

bool rocksdb_manager::remove(const std::string& key) {
   if (!db_) return false;

   rocksdb::Status status = db_->Delete(rocksdb::WriteOptions(), key);
   if (!status.ok()) {
      elog("RocksDB delete failed for key ${key}: ${error}",
           ("key", key)("error", status.ToString()));
      return false;
   }
   return true;
}

rocksdb::Iterator* rocksdb_manager::new_iterator() const {
   if (!db_) return nullptr;

   return db_->NewIterator(rocksdb::ReadOptions());
}

bool rocksdb_manager::batch_write(const std::vector<std::pair<std::string, std::string>>& writes,
                                 const std::vector<std::string>& deletes,
                                 bool sync) {
   if (!db_) return false;

   rocksdb::WriteBatch batch;

   for (const auto& write : writes) {
      batch.Put(write.first, write.second);
   }

   for (const auto& key : deletes) {
      batch.Delete(key);
   }

   rocksdb::WriteOptions write_options;
   write_options.sync = sync;
   rocksdb::Status status = db_->Write(write_options, &batch);
   if (!status.ok()) {
      elog("RocksDB batch write failed: ${error}", ("error", status.ToString()));
      return false;
   }

   return true;
}

bool rocksdb_manager::create_checkpoint(uint32_t block_num) {
   if (!db_) return false;

   const std::string checkpoint_path = get_checkpoint_path(block_num);

   try {
      std::filesystem::create_directories(std::filesystem::path(checkpoint_path).parent_path());
   } catch (const std::exception& e) {
      elog("Failed to create checkpoint directory for block ${block}: ${error}",
           ("block", block_num)("error", e.what()));
      return false;
   }

   rocksdb::Checkpoint* raw_checkpoint = nullptr;
   rocksdb::Status status = rocksdb::Checkpoint::Create(db_.get(), &raw_checkpoint);
   if (!status.ok()) {
      elog("Failed to create checkpoint object: ${error}", ("error", status.ToString()));
      return false;
   }

   std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw_checkpoint);
   // WriteOptions keeps WAL enabled. Allow a bounded amount of WAL to be
   // captured by the checkpoint so every block does not force a memtable
   // flush; RocksDB still flushes once the live WAL exceeds this threshold.
   constexpr uint64_t checkpoint_wal_flush_threshold = 64ull * 1024 * 1024;
   status = checkpoint->CreateCheckpoint(checkpoint_path, checkpoint_wal_flush_threshold);

   if (!status.ok()) {
      elog("Failed to create checkpoint for block ${block}: ${error}",
           ("block", block_num)("error", status.ToString()));
      return false;
   }

   dlog("Created checkpoint for block ${block} at ${path}",
        ("block", block_num)("path", checkpoint_path));
   return true;
}

std::string rocksdb_manager::get_checkpoint_path(uint32_t block_num) const {
   const auto database_path = std::filesystem::absolute(db_path_).lexically_normal();
   const auto checkpoint_root = database_path.parent_path() /
      (database_path.filename().string() + "_checkpoints");
   return (checkpoint_root / ("checkpoint_" + std::to_string(block_num))).string();
}

bool rocksdb_manager::rollback_to_block(uint32_t block_num) {
   std::unique_lock<std::shared_mutex> lifecycle_lock(db_lifecycle_mutex_);
   if (!db_) return false;

   const std::filesystem::path checkpoint_path = get_checkpoint_path(block_num);
   const std::filesystem::path database_path =
      std::filesystem::absolute(db_path_).lexically_normal();
   const std::filesystem::path staging_path = database_path.parent_path() /
      (database_path.filename().string() + ".rollback_staging");
   const std::filesystem::path backup_path = database_path.parent_path() /
      (database_path.filename().string() + ".rollback_backup");
   const bool compression_enabled = options_.compression != rocksdb::kNoCompression;

   if (!std::filesystem::is_directory(checkpoint_path) ||
       !std::filesystem::exists(checkpoint_path / "CURRENT")) {
      elog("Checkpoint for block ${block} does not exist", ("block", block_num));
      return false;
   }

   // Prepare and validate a complete replacement while the live database is
   // still open. A failed copy must never put the current database at risk.
   try {
      if (std::filesystem::exists(backup_path)) {
         std::error_code stale_backup_error;
         std::filesystem::remove_all(backup_path, stale_backup_error);
         if (stale_backup_error) {
            elog("Cannot roll back block ${block}: stale backup cannot be removed at ${path}: ${error}",
                 ("block", block_num)("path", backup_path.string())
                 ("error", stale_backup_error.message()));
            return false;
         }
      }

      std::filesystem::remove_all(staging_path);
      clone_checkpoint_for_rollback(checkpoint_path, staging_path);
      if (!std::filesystem::exists(staging_path / "CURRENT")) {
         throw std::runtime_error("staged checkpoint is incomplete");
      }
   } catch (const std::exception& e) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(staging_path, cleanup_error);
      elog("Failed to prepare rollback to block ${block}: ${error}",
           ("block", block_num)("error", e.what()));
      return false;
   }

   // Keep API readers out for the complete close/swap/open sequence. Releasing
   // this lock after close would let a new query dereference a null db_ or
   // observe the filesystem between the two atomic renames.
   close_unlocked();
   bool original_moved = false;
   bool replacement_installed = false;

   try {
      // Keep the original database intact until the replacement is ready, then
      // switch directories using same-filesystem renames.
      std::filesystem::rename(database_path, backup_path);
      sync_directory(database_path.parent_path());
      original_moved = true;
      std::filesystem::rename(staging_path, database_path);
      sync_directory(database_path.parent_path());
      replacement_installed = true;

      // A failed replacement must be reported as a failed rollback. Normal
      // startup recovery is deliberately disabled here because silently
      // reopening the backup would otherwise make this operation return true.
      if (!open_unlocked(database_path.string(), compression_enabled, false)) {
         throw std::runtime_error("failed to open restored checkpoint");
      }

      std::error_code cleanup_error;
      std::filesystem::remove_all(backup_path, cleanup_error);
      if (cleanup_error) {
         wlog("Rollback succeeded but failed to remove backup ${path}: ${error}",
              ("path", backup_path.string())("error", cleanup_error.message()));
      }
      ilog("Successfully rolled back database to block ${block}", ("block", block_num));
      return true;
   } catch (const std::exception& e) {
      close_unlocked();

      std::error_code recovery_error;
      if (replacement_installed) {
         std::filesystem::remove_all(database_path, recovery_error);
         if (recovery_error) {
            elog("Failed to remove unusable rollback database ${path}: ${error}",
                 ("path", database_path.string())("error", recovery_error.message()));
         }
      }

      if (original_moved && !recovery_error) {
         std::filesystem::rename(backup_path, database_path, recovery_error);
         if (!recovery_error) sync_directory(database_path.parent_path());
      }

      if (original_moved && !recovery_error) {
         if (!open_unlocked(database_path.string(), compression_enabled, false)) {
            elog("Failed to reopen original database after rollback failure");
         }
      } else if (!original_moved) {
         // The directory swap never started, so the original database is still
         // in place and only needs to be reopened.
         if (!open_unlocked(database_path.string(), compression_enabled, false)) {
            elog("Failed to reopen database after rollback setup failure");
         }
      } else if (recovery_error) {
         elog("Failed to restore original database from ${path}: ${error}",
              ("path", backup_path.string())("error", recovery_error.message()));
      }

      std::error_code cleanup_error;
      std::filesystem::remove_all(staging_path, cleanup_error);
      elog("Rollback to block ${block} failed; original database recovery was attempted: ${error}",
           ("block", block_num)("error", e.what()));
      return false;
   }
}

std::string rocksdb_manager::get_performance_metrics() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object metrics;

   try {
      // Get RocksDB statistics
      std::string stats_str;
      if (db_->GetProperty("rocksdb.stats", &stats_str)) {
         metrics["rocksdb_internal_stats"] = stats_str;
      }

      // These are ticker counters, not DB properties. Reading them through
      // GetProperty silently produced no metrics on supported RocksDB builds.
      if (options_.statistics) {
         metrics["total_keys_read"] = options_.statistics->getTickerCount(rocksdb::NUMBER_KEYS_READ);
         metrics["total_keys_written"] = options_.statistics->getTickerCount(rocksdb::NUMBER_KEYS_WRITTEN);
         metrics["total_seeks"] = options_.statistics->getTickerCount(rocksdb::NUMBER_DB_SEEK);
         metrics["bytes_read"] = options_.statistics->getTickerCount(rocksdb::BYTES_READ);
         metrics["bytes_written"] = options_.statistics->getTickerCount(rocksdb::BYTES_WRITTEN);
      }

      // Get cache statistics
      std::string cache_usage, cache_pinned;
      if (db_->GetProperty("rocksdb.block-cache-usage", &cache_usage)) {
         metrics["cache_usage_bytes"] = std::stoull(cache_usage);
         metrics["cache_usage_mb"] = std::stoull(cache_usage) / (1024 * 1024);
      }
      if (db_->GetProperty("rocksdb.block-cache-pinned-usage", &cache_pinned)) {
         metrics["cache_pinned_bytes"] = std::stoull(cache_pinned);
      }

      // Get compaction statistics
      std::string compaction_pending, memtable_flush_pending;
      if (db_->GetProperty("rocksdb.compaction-pending", &compaction_pending)) {
         metrics["compaction_pending"] = (compaction_pending == "1");
      }
      if (db_->GetProperty("rocksdb.mem-table-flush-pending", &memtable_flush_pending)) {
         metrics["memtable_flush_pending"] = (memtable_flush_pending == "1");
      }

      // Get background error count
      std::string bg_errors;
      if (db_->GetProperty("rocksdb.background-errors", &bg_errors)) {
         metrics["background_errors"] = std::stoull(bg_errors);
      }

      // Performance health indicators
      uint64_t total_ops = 0;
      if (metrics.find("total_keys_read") != metrics.end()) total_ops += metrics["total_keys_read"].as_uint64();
      if (metrics.find("total_keys_written") != metrics.end()) total_ops += metrics["total_keys_written"].as_uint64();

      metrics["total_operations"] = total_ops;
      metrics["healthy"] = (metrics.find("background_errors") == metrics.end() ||
                           metrics["background_errors"].as_uint64() == 0);

   } catch (const std::exception& e) {
      metrics["error"] = e.what();
      metrics["healthy"] = false;
   }

   return fc::json::to_string(fc::variant(metrics), fc::time_point::maximum());
}

std::string rocksdb_manager::get_size_breakdown() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object size_info;

   try {
      // Get total size
      std::string total_size_str;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &total_size_str)) {
         uint64_t total_size = std::stoull(total_size_str);
         size_info["total_sst_size_bytes"] = total_size;
         size_info["total_sst_size_mb"] = total_size / (1024 * 1024);
      }

      // Get live data size
      std::string live_size_str;
      if (db_->GetProperty("rocksdb.live-sst-files-size", &live_size_str)) {
         uint64_t live_size = std::stoull(live_size_str);
         size_info["live_sst_size_bytes"] = live_size;
         size_info["live_sst_size_mb"] = live_size / (1024 * 1024);
      }

      // Get number of files
      std::string num_files_str;
      if (db_->GetProperty("rocksdb.num-files-at-level0", &num_files_str)) {
         size_info["level0_files"] = std::stoull(num_files_str);
      }

      // Sample and estimate data type distribution
      std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
      uint64_t transaction_entries = 0;
      uint64_t account_entries = 0;
      uint64_t block_entries = 0;
      uint64_t metadata_entries = 0;
      uint64_t sample_count = 0;
      const uint64_t max_samples = 10000; // Limit sampling for performance

      for (it->SeekToFirst(); it->Valid() && sample_count < max_samples; it->Next(), sample_count++) {
         std::string key = it->key().ToString();
         if (key.starts_with("trx:")) {
            transaction_entries++;
         } else if (key.starts_with("acc:")) {
            account_entries++;
         } else if (key.starts_with("blk:")) {
            block_entries++;
         } else {
            metadata_entries++;
         }
      }

      if (!it->status().ok()) {
         throw std::runtime_error("RocksDB size sampling failed: " + it->status().ToString());
      }

      // Estimate total distribution based on sample
      if (sample_count > 0) {
         size_info["estimated_transaction_entries"] = transaction_entries;
         size_info["estimated_account_entries"] = account_entries;
         size_info["estimated_block_entries"] = block_entries;
         size_info["estimated_metadata_entries"] = metadata_entries;
         size_info["sample_size"] = sample_count;

         if (sample_count == max_samples) {
            size_info["note"] = "Distribution is estimated from sample due to large dataset";
         }
      }

   } catch (const std::exception& e) {
      size_info["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(size_info), fc::time_point::maximum());
}

bool rocksdb_manager::health_check() const {
   if (!db_) {
      return false;
   }

   try {
      // Test basic write operation
      const std::string test_key = "_health_check_" + std::to_string(fc::time_point::now().time_since_epoch().count());
      const std::string test_value = "health_check_value";

      rocksdb::Status write_status = db_->Put(rocksdb::WriteOptions(), test_key, test_value);
      if (!write_status.ok()) {
         elog("Health check failed: write test failed - ${error}", ("error", write_status.ToString()));
         return false;
      }
      bool cleanup_required = true;
      auto cleanup = fc::scoped_exit<std::function<void()>>([&]() {
         if (cleanup_required) {
            db_->Delete(rocksdb::WriteOptions(), test_key);
         }
      });

      // Test basic read operation
      std::string read_value;
      rocksdb::Status read_status = db_->Get(rocksdb::ReadOptions(), test_key, &read_value);
      if (!read_status.ok()) {
         elog("Health check failed: read test failed - ${error}", ("error", read_status.ToString()));
         return false;
      }

      // Verify data integrity
      if (read_value != test_value) {
         elog("Health check failed: data integrity test failed - expected '${expected}', got '${actual}'",
              ("expected", test_value)("actual", read_value));
         return false;
      }

      // Test delete operation
      rocksdb::Status delete_status = db_->Delete(rocksdb::WriteOptions(), test_key);
      if (!delete_status.ok()) {
         elog("Health check failed: delete test failed - ${error}", ("error", delete_status.ToString()));
         return false;
      }

      // Verify deletion
      std::string verify_delete;
      rocksdb::Status verify_status = db_->Get(rocksdb::ReadOptions(), test_key, &verify_delete);
      if (!verify_status.IsNotFound()) {
         elog("Health check failed: delete verification failed");
         return false;
      }
      cleanup_required = false;

      // Check for background errors
      std::string bg_errors;
      if (db_->GetProperty("rocksdb.background-errors", &bg_errors)) {
         if (std::stoull(bg_errors) > 0) {
            elog("Health check failed: background errors detected - ${errors}", ("errors", bg_errors));
            return false;
         }
      }

      return true;

   } catch (const std::exception& e) {
      elog("Health check failed with exception: ${error}", ("error", e.what()));
      return false;
   } catch (...) {
      elog("Health check failed with unknown exception");
      return false;
   }
}

std::string rocksdb_manager::get_tuning_recommendations() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object recommendations;

   try {
      // Get current configuration
      std::string level0_files;
      db_->GetProperty("rocksdb.num-files-at-level0", &level0_files);

      uint64_t current_l0_files = 0;
      try {
         current_l0_files = std::stoull(level0_files);
      } catch (...) {}

      // System memory analysis
      std::ifstream meminfo("/proc/meminfo");
      size_t total_memory = 0, available_memory = 0;
      std::string line;
      while (std::getline(meminfo, line)) {
         if (line.find("MemTotal:") == 0) {
            total_memory = std::stoull(line.substr(9)) * 1024; // Convert KB to bytes
         } else if (line.find("MemAvailable:") == 0) {
            available_memory = std::stoull(line.substr(13)) * 1024;
         }
      }

      // Database size analysis
      std::string db_size_str;
      size_t db_size = 0;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &db_size_str)) {
         try { db_size = std::stoull(db_size_str); } catch (...) {}
      }

      // Generate recommendations
      fc::mutable_variant_object config_recommendations;
      fc::mutable_variant_object performance_recommendations;
      fc::mutable_variant_object maintenance_recommendations;

      // Memory configuration recommendations
      size_t recommended_cache = std::min(available_memory / 4, db_size / 2);
      recommended_cache = std::max(recommended_cache, static_cast<size_t>(64 * 1024 * 1024)); // Min 64MB

      config_recommendations["block_cache_size"] = fc::mutable_variant_object()
         ("current", "Unknown - check configuration")
         ("recommended_bytes", recommended_cache)
         ("recommended_mb", recommended_cache / (1024 * 1024))
         ("reason", "25% of available memory or 50% of database size, whichever is smaller");

      // Write buffer recommendations
      size_t recommended_write_buffer = std::min(available_memory / 16, static_cast<size_t>(128 * 1024 * 1024));
      config_recommendations["write_buffer_size"] = fc::mutable_variant_object()
         ("current_bytes", options_.write_buffer_size)
         ("current_mb", options_.write_buffer_size / (1024 * 1024))
         ("recommended_bytes", recommended_write_buffer)
         ("recommended_mb", recommended_write_buffer / (1024 * 1024))
         ("reason", "6.25% of available memory, max 128MB for optimal flush frequency");

      // L0 files analysis
      if (current_l0_files > 10) {
         performance_recommendations["level0_compaction"] = fc::mutable_variant_object()
            ("current_files", current_l0_files)
            ("status", "Warning: High L0 file count")
            ("recommendation", "Consider manual compaction or increasing background compaction threads")
            ("suggested_action", "Enable auto-compaction or increase max_background_compactions");
      }

      // Background jobs optimization
      size_t cpu_cores = std::thread::hardware_concurrency();
      size_t recommended_bg_jobs = std::max(static_cast<size_t>(1),
         std::min(cpu_cores / 2, static_cast<size_t>(6)));
      config_recommendations["background_jobs"] = fc::mutable_variant_object()
         ("current", options_.max_background_jobs)
         ("recommended", recommended_bg_jobs)
         ("cpu_cores", cpu_cores)
         ("reason", "Half of CPU cores, max 6 for balanced I/O performance");

      // Compression recommendations
      std::string compression_info = get_compression_info();
      if (compression_info.find("Mixed mode") != std::string::npos) {
         maintenance_recommendations["compression_consistency"] = fc::mutable_variant_object()
            ("status", "Warning: Mixed compression mode detected")
            ("recommendation", "Consider full database compaction to standardize compression")
            ("impact", "May improve read performance and reduce storage space");
      }

      // Database size vs memory recommendations
      if (db_size > available_memory * 2) {
         performance_recommendations["memory_pressure"] = fc::mutable_variant_object()
            ("status", "Warning: Database size is much larger than available memory")
            ("db_size_gb", db_size / (1024.0 * 1024.0 * 1024.0))
            ("available_memory_gb", available_memory / (1024.0 * 1024.0 * 1024.0))
            ("recommendation", "Consider increasing system memory or implementing data pruning")
            ("alternatives", fc::variants{
               "Enable block cache compression",
               "Implement historical data archiving",
               "Use read-only replicas for queries"
            });
      }

      recommendations["configuration"] = config_recommendations;
      recommendations["performance"] = performance_recommendations;
      recommendations["maintenance"] = maintenance_recommendations;
      recommendations["system_info"] = fc::mutable_variant_object()
         ("total_memory_gb", total_memory / (1024.0 * 1024.0 * 1024.0))
         ("available_memory_gb", available_memory / (1024.0 * 1024.0 * 1024.0))
         ("database_size_gb", db_size / (1024.0 * 1024.0 * 1024.0))
         ("cpu_cores", cpu_cores);

   } catch (const std::exception& e) {
      recommendations["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(recommendations), fc::time_point::maximum());
}

size_t rocksdb_manager::estimate_optimal_cache_size() const {
   if (!db_) return 0;

   try {
      // Get system memory
      std::ifstream meminfo("/proc/meminfo");
      size_t available_memory = 0;
      std::string line;
      while (std::getline(meminfo, line)) {
         if (line.find("MemAvailable:") == 0) {
            available_memory = std::stoull(line.substr(13)) * 1024; // Convert KB to bytes
            break;
         }
      }

      // Get database size
      std::string db_size_str;
      size_t db_size = 0;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &db_size_str)) {
         try { db_size = std::stoull(db_size_str); } catch (...) {}
      }

      // Estimation logic:
      // 1. Use 25% of available memory
      // 2. But not more than 50% of database size (if DB is small)
      // 3. Minimum 64MB, maximum 2GB for safety
      size_t cache_from_memory = available_memory / 4;
      size_t cache_from_db_size = db_size / 2;

      size_t optimal_cache = std::min(cache_from_memory, cache_from_db_size);

      // Apply bounds
      optimal_cache = std::max(optimal_cache, static_cast<size_t>(64 * 1024 * 1024));      // Min 64MB
      optimal_cache = std::min(optimal_cache, static_cast<size_t>(2ULL * 1024 * 1024 * 1024)); // Max 2GB

      return optimal_cache;

   } catch (...) {
      return 256 * 1024 * 1024; // Default 256MB
   }
}

std::string rocksdb_manager::analyze_key_distribution() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object analysis;

   try {
      std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));

      // Distribution counters
      std::map<std::string, uint64_t> prefix_counts;
      std::map<std::string, uint64_t> prefix_sizes;
      std::map<uint32_t, uint64_t> block_distribution; // Block number -> count

      uint64_t total_keys = 0;
      uint64_t total_size = 0;
      uint32_t min_block = UINT32_MAX, max_block = 0;

      const uint64_t SAMPLE_LIMIT = 50000; // Sample for performance
      uint64_t sampled = 0;

      for (it->SeekToFirst(); it->Valid() && sampled < SAMPLE_LIMIT; it->Next(), sampled++) {
         std::string key = it->key().ToString();
         std::string value = it->value().ToString();

         total_keys++;
         total_size += key.size() + value.size();

         // Analyze key prefixes
         size_t colon_pos = key.find(':');
         if (colon_pos != std::string::npos) {
            std::string prefix = key.substr(0, colon_pos + 1);
            prefix_counts[prefix]++;
            prefix_sizes[prefix] += key.size() + value.size();

            // Extract block numbers for temporal analysis
            if (prefix == "trx:" || prefix == "acc:") {
               uint32_t block_num = 0;
               if (extract_json_block_num(value, block_num)) {
                  block_distribution[block_num]++;
                  min_block = std::min(min_block, block_num);
                  max_block = std::max(max_block, block_num);
               }
            } else if (prefix == "blk:") {
               // Extract from key: "blk:block_num:..."
               size_t second_colon = key.find(':', 4);
               if (second_colon != std::string::npos) {
                  try {
                     uint32_t block_num = std::stoul(key.substr(4, second_colon - 4));
                     block_distribution[block_num]++;
                     min_block = std::min(min_block, block_num);
                     max_block = std::max(max_block, block_num);
                  } catch (...) {}
               }
            }
         }
      }

      if (!it->status().ok()) {
         throw std::runtime_error("RocksDB key distribution scan failed: " + it->status().ToString());
      }

      // Generate analysis results
      fc::mutable_variant_object prefix_analysis;
      for (const auto& entry : prefix_counts) {
         fc::mutable_variant_object prefix_info;
         prefix_info["count"] = entry.second;
         prefix_info["total_size_bytes"] = prefix_sizes[entry.first];
         prefix_info["total_size_mb"] = prefix_sizes[entry.first] / (1024.0 * 1024.0);
         prefix_info["avg_size_bytes"] = entry.second > 0 ? prefix_sizes[entry.first] / entry.second : 0;
         prefix_info["percentage"] = total_keys > 0 ? (entry.second * 100.0) / total_keys : 0.0;

         prefix_analysis[entry.first] = prefix_info;
      }

      // Block range analysis
      fc::mutable_variant_object temporal_analysis;
      if (min_block != UINT32_MAX && max_block > min_block) {
         temporal_analysis["block_range"] = fc::mutable_variant_object()
            ("min_block", min_block)
            ("max_block", max_block)
            ("span", max_block - min_block)
            ("total_blocks_with_data", block_distribution.size());

         // Calculate distribution density
         if (block_distribution.size() > 10) {
            std::vector<uint64_t> counts;
            for (const auto& entry : block_distribution) {
               counts.push_back(entry.second);
            }
            std::sort(counts.begin(), counts.end());

            size_t median_idx = counts.size() / 2;
            uint64_t median_count = counts[median_idx];
            uint64_t max_count = counts.back();

            temporal_analysis["density_stats"] = fc::mutable_variant_object()
               ("median_entries_per_block", median_count)
               ("max_entries_per_block", max_count)
               ("distribution_type", max_count > median_count * 3 ? "skewed" : "uniform");
         }
      }

      // Generate optimization recommendations
      fc::variants optimization_suggestions;

      // Check for unbalanced distribution
      if (prefix_counts.size() > 0) {
         auto max_prefix = std::max_element(prefix_counts.begin(), prefix_counts.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

         if (max_prefix->second > total_keys * 0.8) {
            optimization_suggestions.push_back(fc::mutable_variant_object()
               ("type", "distribution_warning")
               ("issue", "Single key type dominates database")
               ("dominant_prefix", max_prefix->first)
               ("percentage", (max_prefix->second * 100.0) / total_keys)
               ("suggestion", "Consider partitioning or separate storage for dominant key type"));
         }
      }

      // Check for sparse block distribution
      if (max_block > min_block && block_distribution.size() < (max_block - min_block) * 0.1) {
         optimization_suggestions.push_back(fc::mutable_variant_object()
            ("type", "temporal_sparsity")
            ("issue", "Sparse block number distribution detected")
            ("coverage_percentage", (block_distribution.size() * 100.0) / (max_block - min_block))
            ("suggestion", "Consider implementing block number compaction or archival for old blocks"));
      }

      analysis["key_distribution"] = prefix_analysis;
      analysis["temporal_analysis"] = temporal_analysis;
      analysis["summary"] = fc::mutable_variant_object()
         ("total_keys_sampled", total_keys)
         ("total_size_mb", total_size / (1024.0 * 1024.0))
         ("avg_key_size_bytes", total_keys > 0 ? total_size / total_keys : 0)
         ("unique_prefixes", prefix_counts.size())
         ("sample_limit_reached", sampled >= SAMPLE_LIMIT);

      analysis["optimization_suggestions"] = optimization_suggestions;

   } catch (const std::exception& e) {
      analysis["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(analysis), fc::time_point::maximum());
}

std::string rocksdb_manager::get_optimization_suggestions() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object suggestions;

   try {
      fc::variants optimization_list;

      // Check write buffer efficiency
      std::string memtable_size_str, writes_str;
      if (db_->GetProperty("rocksdb.cur-size-all-mem-tables", &memtable_size_str) &&
          db_->GetProperty("rocksdb.number.db.put", &writes_str)) {

         uint64_t memtable_size = 0, total_writes = 0;
         try {
            memtable_size = std::stoull(memtable_size_str);
            total_writes = std::stoull(writes_str);
         } catch (...) {}

         if (total_writes > 100000 && memtable_size > options_.write_buffer_size * 0.8) {
            optimization_list.push_back(fc::mutable_variant_object()
               ("category", "write_performance")
               ("priority", "high")
               ("issue", "Write buffer frequently full")
               ("suggestion", "Increase write_buffer_size or max_write_buffer_number")
               ("current_buffer_size_mb", options_.write_buffer_size / (1024 * 1024))
               ("recommended_action", "Consider doubling write buffer size"));
         }
      }

      // Check L0 file count
      std::string l0_files_str;
      if (db_->GetProperty("rocksdb.num-files-at-level0", &l0_files_str)) {
         uint64_t l0_files = 0;
         try { l0_files = std::stoull(l0_files_str); } catch (...) {}

         if (l0_files > 20) {
            optimization_list.push_back(fc::mutable_variant_object()
               ("category", "compaction")
               ("priority", "high")
               ("issue", "Excessive L0 files causing read amplification")
               ("current_l0_files", l0_files)
               ("suggestion", "Trigger manual compaction or increase background compaction threads")
               ("recommended_action", "Call compact_database() method"));
         } else if (l0_files > 10) {
            optimization_list.push_back(fc::mutable_variant_object()
               ("category", "compaction")
               ("priority", "medium")
               ("issue", "High L0 file count")
               ("current_l0_files", l0_files)
               ("suggestion", "Monitor compaction and consider increasing background jobs"));
         }
      }

      // Check cache hit ratio
      std::string cache_hits_str, cache_misses_str;
      if (db_->GetProperty("rocksdb.block-cache-hit", &cache_hits_str) &&
          db_->GetProperty("rocksdb.block-cache-miss", &cache_misses_str)) {

         uint64_t hits = 0, misses = 0;
         try {
            hits = std::stoull(cache_hits_str);
            misses = std::stoull(cache_misses_str);
         } catch (...) {}

         if (hits + misses > 1000) {
            double hit_ratio = (double)hits / (hits + misses);
            if (hit_ratio < 0.8) {
               optimization_list.push_back(fc::mutable_variant_object()
                  ("category", "cache_performance")
                  ("priority", "medium")
                  ("issue", "Low cache hit ratio")
                  ("current_hit_ratio", hit_ratio)
                  ("suggestion", "Increase block cache size")
                  ("recommended_action", "Consider increasing block cache to improve read performance"));
            }
         }
      }

      // Check compression efficiency
      std::string total_size_str, live_size_str;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &total_size_str) &&
          db_->GetProperty("rocksdb.live-sst-files-size", &live_size_str)) {

         uint64_t total_size = 0, live_size = 0;
         try {
            total_size = std::stoull(total_size_str);
            live_size = std::stoull(live_size_str);
         } catch (...) {}

         if (total_size > live_size * 1.5 && total_size > 100 * 1024 * 1024) {
            optimization_list.push_back(fc::mutable_variant_object()
               ("category", "storage_efficiency")
               ("priority", "medium")
               ("issue", "Significant space amplification detected")
               ("total_size_mb", total_size / (1024 * 1024))
               ("live_size_mb", live_size / (1024 * 1024))
               ("amplification_ratio", (double)total_size / live_size)
               ("suggestion", "Run compaction to reclaim space")
               ("recommended_action", "Schedule database compaction during low-traffic period"));
         }
      }

      // System resource analysis
      std::ifstream meminfo("/proc/meminfo");
      size_t available_memory = 0;
      std::string line;
      while (std::getline(meminfo, line)) {
         if (line.find("MemAvailable:") == 0) {
            available_memory = std::stoull(line.substr(13)) * 1024;
            break;
         }
      }

      if (available_memory > 0) {
         size_t db_size = 0;
         if (db_->GetProperty("rocksdb.total-sst-files-size", &total_size_str)) {
            try { db_size = std::stoull(total_size_str); } catch (...) {}
         }

         if (db_size > available_memory * 4) {
            optimization_list.push_back(fc::mutable_variant_object()
               ("category", "system_resources")
               ("priority", "high")
               ("issue", "Database size much larger than available memory")
               ("db_size_gb", db_size / (1024.0 * 1024.0 * 1024.0))
               ("available_memory_gb", available_memory / (1024.0 * 1024.0 * 1024.0))
               ("suggestion", "Consider data archival or adding more RAM")
               ("recommended_action", "Implement historical data pruning strategy"));
         }
      }

      suggestions["optimizations"] = optimization_list;
      suggestions["total_suggestions"] = optimization_list.size();
      suggestions["analysis_timestamp"] = fc::time_point::now().time_since_epoch().count();

   } catch (const std::exception& e) {
      suggestions["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(suggestions), fc::time_point::maximum());
}

std::string rocksdb_manager::analyze_fragmentation() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object fragmentation_analysis;

   try {
      // Get size information
      std::string total_size_str, live_size_str;
      uint64_t total_size = 0, live_size = 0;

      if (db_->GetProperty("rocksdb.total-sst-files-size", &total_size_str)) {
         try { total_size = std::stoull(total_size_str); } catch (...) {}
      }

      if (db_->GetProperty("rocksdb.live-sst-files-size", &live_size_str)) {
         try { live_size = std::stoull(live_size_str); } catch (...) {}
      }

      // Calculate fragmentation metrics
      double space_amplification = (live_size > 0) ? (double)total_size / live_size : 1.0;
      double fragmentation_percentage = (space_amplification - 1.0) * 100.0;

      fragmentation_analysis["total_size_bytes"] = total_size;
      fragmentation_analysis["live_size_bytes"] = live_size;
      fragmentation_analysis["wasted_space_bytes"] = total_size - live_size;
      fragmentation_analysis["space_amplification"] = space_amplification;
      fragmentation_analysis["fragmentation_percentage"] = fragmentation_percentage;

      // Fragmentation assessment
      std::string fragmentation_level;
      std::string recommendation;

      if (fragmentation_percentage < 10) {
         fragmentation_level = "low";
         recommendation = "No immediate action needed";
      } else if (fragmentation_percentage < 25) {
         fragmentation_level = "moderate";
         recommendation = "Consider compaction during low-traffic periods";
      } else if (fragmentation_percentage < 50) {
         fragmentation_level = "high";
         recommendation = "Schedule compaction soon to reclaim space";
      } else {
         fragmentation_level = "severe";
         recommendation = "Immediate compaction recommended";
      }

      fragmentation_analysis["fragmentation_level"] = fragmentation_level;
      fragmentation_analysis["recommendation"] = recommendation;

      // Level-specific analysis
      fc::mutable_variant_object level_info;
      for (int level = 0; level <= 6; ++level) {
         std::string level_size_key = "rocksdb.size-bytes-level" + std::to_string(level);
         std::string level_files_key = "rocksdb.num-files-level" + std::to_string(level);

         std::string level_size_str, level_files_str;
         if (db_->GetProperty(level_size_key, &level_size_str) &&
             db_->GetProperty(level_files_key, &level_files_str)) {

            uint64_t level_size = 0, level_files = 0;
            try {
               level_size = std::stoull(level_size_str);
               level_files = std::stoull(level_files_str);
            } catch (...) {}

            if (level_size > 0 || level_files > 0) {
               level_info["level_" + std::to_string(level)] = fc::mutable_variant_object()
                  ("size_bytes", level_size)
                  ("size_mb", level_size / (1024 * 1024))
                  ("num_files", level_files)
                  ("avg_file_size_mb", level_files > 0 ? (level_size / level_files) / (1024 * 1024) : 0);
            }
         }
      }

      fragmentation_analysis["level_breakdown"] = level_info;

      // Time-based recommendations
      uint64_t estimated_compaction_time = std::max(static_cast<uint64_t>(1), total_size / (100 * 1024 * 1024)); // Rough estimate: 100MB/second
      fragmentation_analysis["estimated_compaction_time_seconds"] = estimated_compaction_time;
      fragmentation_analysis["estimated_compaction_time_minutes"] = estimated_compaction_time / 60;

   } catch (const std::exception& e) {
      fragmentation_analysis["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(fragmentation_analysis), fc::time_point::maximum());
}

std::string rocksdb_manager::get_cache_analysis() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object cache_analysis;

   try {
      // Block cache statistics
      std::string cache_usage_str, cache_pinned_str;

      if (db_->GetProperty("rocksdb.block-cache-usage", &cache_usage_str)) {
         uint64_t cache_usage = std::stoull(cache_usage_str);
         cache_analysis["block_cache_usage_bytes"] = cache_usage;
         cache_analysis["block_cache_usage_mb"] = cache_usage / (1024 * 1024);
      }

      if (db_->GetProperty("rocksdb.block-cache-pinned-usage", &cache_pinned_str)) {
         uint64_t cache_pinned = std::stoull(cache_pinned_str);
         cache_analysis["block_cache_pinned_bytes"] = cache_pinned;
         cache_analysis["block_cache_pinned_mb"] = cache_pinned / (1024 * 1024);
      }

      // Cache hit/miss statistics
      uint64_t hits = 0, misses = 0;
      if (options_.statistics) {
         hits = options_.statistics->getTickerCount(rocksdb::BLOCK_CACHE_HIT);
         misses = options_.statistics->getTickerCount(rocksdb::BLOCK_CACHE_MISS);
      }

      cache_analysis["cache_hits"] = hits;
      cache_analysis["cache_misses"] = misses;

      if (hits + misses > 0) {
         double hit_ratio = (double)hits / (hits + misses);
         cache_analysis["hit_ratio"] = hit_ratio;
         cache_analysis["hit_ratio_percentage"] = hit_ratio * 100.0;

         // Performance assessment
         std::string performance_level;
         if (hit_ratio > 0.95) {
            performance_level = "excellent";
         } else if (hit_ratio > 0.85) {
            performance_level = "good";
         } else if (hit_ratio > 0.70) {
            performance_level = "fair";
         } else {
            performance_level = "poor";
         }
         cache_analysis["performance_level"] = performance_level;
      }

      // Cache efficiency recommendations
      fc::variants recommendations;

      uint64_t cache_usage = 0;
      try {
         if (!cache_usage_str.empty()) {
            cache_usage = std::stoull(cache_usage_str);
         }
      } catch (...) {}

      // Estimate optimal cache size
      size_t optimal_cache = estimate_optimal_cache_size();
      cache_analysis["optimal_cache_size_bytes"] = optimal_cache;
      cache_analysis["optimal_cache_size_mb"] = optimal_cache / (1024 * 1024);

      if (cache_usage > 0) {
         double cache_utilization = (double)cache_usage / optimal_cache;
         cache_analysis["cache_utilization"] = cache_utilization;

         if (cache_utilization < 0.5) {
            recommendations.push_back(fc::mutable_variant_object()
               ("type", "underutilized")
               ("message", "Cache is underutilized, consider reducing cache size")
               ("current_usage_mb", cache_usage / (1024 * 1024))
               ("suggestion", "Reduce cache size to free up memory for other processes"));
         } else if (cache_utilization > 0.9) {
            recommendations.push_back(fc::mutable_variant_object()
               ("type", "overutilized")
               ("message", "Cache is nearly full, consider increasing cache size")
               ("current_usage_mb", cache_usage / (1024 * 1024))
               ("suggestion", "Increase cache size to improve hit ratio"));
         }
      }

      // Hit ratio based recommendations
      if (hits + misses > 1000) {
         double hit_ratio = (double)hits / (hits + misses);
         if (hit_ratio < 0.8) {
            recommendations.push_back(fc::mutable_variant_object()
               ("type", "low_hit_ratio")
               ("message", "Low cache hit ratio detected")
               ("current_hit_ratio", hit_ratio)
               ("suggestion", "Consider increasing cache size or review access patterns"));
         }
      }

      cache_analysis["recommendations"] = recommendations;

      // System memory context
      std::ifstream meminfo("/proc/meminfo");
      size_t available_memory = 0;
      std::string line;
      while (std::getline(meminfo, line)) {
         if (line.find("MemAvailable:") == 0) {
            available_memory = std::stoull(line.substr(13)) * 1024;
            break;
         }
      }

      if (available_memory > 0) {
         cache_analysis["system_available_memory_gb"] = available_memory / (1024.0 * 1024.0 * 1024.0);
         cache_analysis["cache_memory_percentage"] = (cache_usage * 100.0) / available_memory;
      }

   } catch (const std::exception& e) {
      cache_analysis["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(cache_analysis), fc::time_point::maximum());
}

bool rocksdb_manager::auto_optimize(uint32_t max_duration_seconds) {
   if (!db_) {
      elog("Cannot auto-optimize: database not open");
      return false;
   }

   try {
      ilog("Starting automatic database optimization (max duration: ${duration}s)", ("duration", max_duration_seconds));

      auto start_time = std::chrono::steady_clock::now();
      bool optimization_performed = false;

      // Check if compaction is needed
      if (needs_compaction()) {
         ilog("Auto-optimization: Starting database compaction");
         if (compact_database()) {
            optimization_performed = true;
            ilog("Auto-optimization: Compaction completed successfully");
         } else {
            wlog("Auto-optimization: Compaction failed");
         }

         // Check if we've exceeded time limit
         auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
         if (elapsed >= max_duration_seconds) {
            ilog("Auto-optimization: Time limit reached after compaction");
            return optimization_performed;
         }
      }

      // Validate and repair if needed
      if (validate_and_repair_database()) {
         optimization_performed = true;
         ilog("Auto-optimization: Database validation and repair completed");
      }

      // Final health check
      if (health_check()) {
         ilog("Auto-optimization: Database health check passed");
      } else {
         wlog("Auto-optimization: Database health check failed");
      }

      auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
         std::chrono::steady_clock::now() - start_time).count();

      ilog("Auto-optimization completed in ${elapsed}s, optimizations performed: ${performed}",
           ("elapsed", total_elapsed)("performed", optimization_performed));

      return true;

   } catch (const std::exception& e) {
      elog("Exception during auto-optimization: ${error}", ("error", e.what()));
      return false;
   }
}

std::string rocksdb_manager::check_maintenance_needs() const {
   if (!db_) {
      return "{\"error\":\"Database not open\"}";
   }

   fc::mutable_variant_object maintenance_check;

   try {
      fc::variants urgent_tasks;
      fc::variants recommended_tasks;
      fc::variants optional_tasks;

      // Check fragmentation level
      std::string fragmentation_json = analyze_fragmentation();
      fc::variant fragmentation_data = fc::json::from_string(fragmentation_json);

      if (fragmentation_data.is_object()) {
         auto frag_obj = fragmentation_data.get_object();
         if (frag_obj.contains("fragmentation_percentage")) {
            double frag_percent = frag_obj["fragmentation_percentage"].as<double>();

            if (frag_percent > 50) {
               urgent_tasks.push_back(fc::mutable_variant_object()
                  ("task", "database_compaction")
                  ("reason", "Severe fragmentation detected")
                  ("fragmentation_percentage", frag_percent)
                  ("estimated_time_minutes", frag_obj.contains("estimated_compaction_time_minutes") ?
                     frag_obj["estimated_compaction_time_minutes"].as<uint64_t>() : 0));
            } else if (frag_percent > 25) {
               recommended_tasks.push_back(fc::mutable_variant_object()
                  ("task", "database_compaction")
                  ("reason", "High fragmentation detected")
                  ("fragmentation_percentage", frag_percent));
            }
         }
      }

      // Check L0 file count
      std::string l0_files_str;
      if (db_->GetProperty("rocksdb.num-files-at-level0", &l0_files_str)) {
         uint64_t l0_files = 0;
         try { l0_files = std::stoull(l0_files_str); } catch (...) {}

         if (l0_files > 30) {
            urgent_tasks.push_back(fc::mutable_variant_object()
               ("task", "manual_compaction")
               ("reason", "Critical L0 file count")
               ("current_l0_files", l0_files)
               ("impact", "Severe read performance degradation"));
         } else if (l0_files > 15) {
            recommended_tasks.push_back(fc::mutable_variant_object()
               ("task", "manual_compaction")
               ("reason", "High L0 file count")
               ("current_l0_files", l0_files));
         }
      }

      // Check background errors
      std::string bg_errors_str;
      if (db_->GetProperty("rocksdb.background-errors", &bg_errors_str)) {
         uint64_t bg_errors = 0;
         try { bg_errors = std::stoull(bg_errors_str); } catch (...) {}

         if (bg_errors > 0) {
            urgent_tasks.push_back(fc::mutable_variant_object()
               ("task", "error_investigation")
               ("reason", "Background errors detected")
               ("error_count", bg_errors)
               ("action", "Check logs and run database validation"));
         }
      }

      // Check cache performance
      std::string cache_analysis_json = get_cache_analysis();
      fc::variant cache_data = fc::json::from_string(cache_analysis_json);

      if (cache_data.is_object()) {
         auto cache_obj = cache_data.get_object();
         if (cache_obj.contains("hit_ratio")) {
            double hit_ratio = cache_obj["hit_ratio"].as<double>();

            if (hit_ratio < 0.7) {
               recommended_tasks.push_back(fc::mutable_variant_object()
                  ("task", "cache_optimization")
                  ("reason", "Poor cache hit ratio")
                  ("current_hit_ratio", hit_ratio)
                  ("suggestion", "Consider increasing cache size"));
            }
         }
      }

      // Check database size vs memory
      std::string db_size_str;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &db_size_str)) {
         uint64_t db_size = 0;
         try { db_size = std::stoull(db_size_str); } catch (...) {}

         std::ifstream meminfo("/proc/meminfo");
         size_t available_memory = 0;
         std::string line;
         while (std::getline(meminfo, line)) {
            if (line.find("MemAvailable:") == 0) {
               available_memory = std::stoull(line.substr(13)) * 1024;
               break;
            }
         }

         if (available_memory > 0 && db_size > available_memory * 8) {
            recommended_tasks.push_back(fc::mutable_variant_object()
               ("task", "data_archival")
               ("reason", "Database size much larger than available memory")
               ("db_size_gb", db_size / (1024.0 * 1024.0 * 1024.0))
               ("available_memory_gb", available_memory / (1024.0 * 1024.0 * 1024.0))
               ("suggestion", "Implement data pruning or archival strategy"));
         }
      }

      // Optional maintenance tasks
      optional_tasks.push_back(fc::mutable_variant_object()
         ("task", "database_validation")
         ("reason", "Routine health check")
         ("frequency", "weekly")
         ("impact", "Minimal"));

      optional_tasks.push_back(fc::mutable_variant_object()
         ("task", "performance_analysis")
         ("reason", "Monitor performance trends")
         ("frequency", "daily")
         ("impact", "None"));

      maintenance_check["urgent_tasks"] = urgent_tasks;
      maintenance_check["recommended_tasks"] = recommended_tasks;
      maintenance_check["optional_tasks"] = optional_tasks;
      maintenance_check["total_urgent"] = urgent_tasks.size();
      maintenance_check["total_recommended"] = recommended_tasks.size();
      maintenance_check["total_optional"] = optional_tasks.size();
      maintenance_check["next_check_recommended_hours"] = 24;
      maintenance_check["analysis_timestamp"] = fc::time_point::now().time_since_epoch().count();

   } catch (const std::exception& e) {
      maintenance_check["error"] = e.what();
   }

   return fc::json::to_string(fc::variant(maintenance_check), fc::time_point::maximum());
}

size_t rocksdb_manager::calculate_optimal_write_buffer_size() const {
   // Get available system memory
   std::ifstream meminfo("/proc/meminfo");
   size_t available_memory = 0;
   std::string line;
   while (std::getline(meminfo, line)) {
      if (line.find("MemAvailable:") == 0) {
         available_memory = std::stoull(line.substr(13)) * 1024;
         break;
      }
   }

   if (available_memory == 0) {
      return 64 * 1024 * 1024; // Default 64MB
   }

   // Use 1/32 of available memory, with bounds
   size_t optimal_size = available_memory / 32;
   optimal_size = std::max(optimal_size, static_cast<size_t>(16 * 1024 * 1024));  // Min 16MB
   optimal_size = std::min(optimal_size, static_cast<size_t>(256 * 1024 * 1024)); // Max 256MB

   return optimal_size;
}

bool rocksdb_manager::needs_compaction() const {
   if (!db_) return false;

   try {
      // Check L0 file count
      std::string l0_files_str;
      if (db_->GetProperty("rocksdb.num-files-at-level0", &l0_files_str)) {
         uint64_t l0_files = std::stoull(l0_files_str);
         if (l0_files > 10) {
            return true;
         }
      }

      // Check space amplification
      std::string total_size_str, live_size_str;
      if (db_->GetProperty("rocksdb.total-sst-files-size", &total_size_str) &&
          db_->GetProperty("rocksdb.live-sst-files-size", &live_size_str)) {

         uint64_t total_size = std::stoull(total_size_str);
         uint64_t live_size = std::stoull(live_size_str);

         if (live_size > 0 && total_size > live_size * 1.5) {
            return true;
         }
      }

      // Check if compaction is already pending
      std::string compaction_pending;
      if (db_->GetProperty("rocksdb.compaction-pending", &compaction_pending)) {
         return (compaction_pending == "1");
      }

   } catch (...) {
      return false;
   }

   return false;
}

} // namespace eosio
