#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <fc/log/logger.hpp>
#include <rocksdb/utilities/checkpoint.h>
#include <filesystem>
#include <thread>

namespace eosio {

rocksdb_manager::rocksdb_manager() : is_open_(false) {
   // Configure RocksDB options for optimal performance
   options_.create_if_missing = true;
   options_.write_buffer_size = 64 * 1024 * 1024;  // 64MB
   options_.max_write_buffer_number = 3;
   options_.target_file_size_base = 64 * 1024 * 1024;  // 64MB
   options_.compression = rocksdb::kLZ4Compression;
   options_.max_background_jobs = 4;
   options_.IncreaseParallelism(std::thread::hardware_concurrency());
}

rocksdb_manager::~rocksdb_manager() {
   close();
}

bool rocksdb_manager::open(const std::string& db_path) {
   if (is_open_) {
      return true;
   }

   db_path_ = db_path;

   // Create directory if it doesn't exist
   try {
      std::filesystem::create_directories(db_path);
   } catch (const std::exception& e) {
      elog("Failed to create database directory ${path}: ${error}",
           ("path", db_path)("error", e.what()));
      return false;
   }

   rocksdb::DB* raw_db;
   rocksdb::Status status = rocksdb::DB::Open(options_, db_path, &raw_db);
   if (!status.ok()) {
      elog("Failed to open RocksDB: ${error}", ("error", status.ToString()));
      return false;
   }

   db_.reset(raw_db);
   is_open_ = true;

   ilog("RocksDB opened successfully at: ${path}", ("path", db_path));
   return true;
}

void rocksdb_manager::close() {
   if (db_) {
      db_.reset();
      is_open_ = false;
   }
}

bool rocksdb_manager::put(const std::string& key, const std::string& value) {
   if (!db_) return false;

   rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
   if (!status.ok()) {
      elog("RocksDB put failed for key ${key}: ${error}",
           ("key", key)("error", status.ToString()));
      return false;
   }
   return true;
}

bool rocksdb_manager::get(const std::string& key, std::string& value) {
   if (!db_) return false;

   rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &value);
   if (status.IsNotFound()) {
      return false;
   }
   if (!status.ok()) {
      elog("RocksDB get failed for key ${key}: ${error}",
           ("key", key)("error", status.ToString()));
      return false;
   }
   return true;
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

bool rocksdb_manager::batch_write(const std::vector<std::pair<std::string, std::string>>& writes,
                                 const std::vector<std::string>& deletes) {
   if (!db_) return false;

   rocksdb::WriteBatch batch;

   for (const auto& write : writes) {
      batch.Put(write.first, write.second);
   }

   for (const auto& key : deletes) {
      batch.Delete(key);
   }

   rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
   if (!status.ok()) {
      elog("RocksDB batch write failed: ${error}", ("error", status.ToString()));
      return false;
   }

   return true;
}

bool rocksdb_manager::create_checkpoint(uint32_t block_num) {
   if (!db_) return false;

   std::string checkpoint_path = db_path_ + "/checkpoint_" + std::to_string(block_num);

   rocksdb::Checkpoint* checkpoint;
   rocksdb::Status status = rocksdb::Checkpoint::Create(db_.get(), &checkpoint);
   if (!status.ok()) {
      elog("Failed to create checkpoint object: ${error}", ("error", status.ToString()));
      return false;
   }

   status = checkpoint->CreateCheckpoint(checkpoint_path);
   delete checkpoint;

   if (!status.ok()) {
      elog("Failed to create checkpoint for block ${block}: ${error}",
           ("block", block_num)("error", status.ToString()));
      return false;
   }

   dlog("Created checkpoint for block ${block} at ${path}",
        ("block", block_num)("path", checkpoint_path));
   return true;
}

bool rocksdb_manager::rollback_to_block(uint32_t block_num) {
   if (!db_) return false;

   std::string checkpoint_path = db_path_ + "/checkpoint_" + std::to_string(block_num);

   if (!std::filesystem::exists(checkpoint_path)) {
      elog("Checkpoint for block ${block} does not exist", ("block", block_num));
      return false;
   }

   // Close current database
   close();

   try {
      // Remove current database directory
      std::filesystem::remove_all(db_path_);

      // Copy checkpoint to main database path
      std::filesystem::copy(checkpoint_path, db_path_,
                           std::filesystem::copy_options::recursive);

      // Reopen database
      if (!open(db_path_)) {
         elog("Failed to reopen database after rollback");
         return false;
      }

      ilog("Successfully rolled back database to block ${block}", ("block", block_num));
      return true;

   } catch (const std::exception& e) {
      elog("Exception during rollback to block ${block}: ${error}",
           ("block", block_num)("error", e.what()));
      return false;
   }
}

} // namespace eosio
