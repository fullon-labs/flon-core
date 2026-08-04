#include <eosio/transaction_history_plugin/rollback_manager.hpp>
#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <fc/log/logger.hpp>
#include <fc/io/raw.hpp>
#include <filesystem>
#include <limits>

namespace eosio {

rollback_manager::rollback_manager(std::shared_ptr<rocksdb_manager> db) : db_(db) {
   load_rollback_points();
}

rollback_manager::~rollback_manager() {
}

bool rollback_manager::create_rollback_point(uint32_t block_num) {
   if (rollback_points_.find(block_num) != rollback_points_.end()) {
      return true;
   }
   try {
      // A directory without loaded rollback metadata is stale (for example,
      // after interruption between checkpoint creation and metadata commit).
      std::error_code stale_error;
      std::filesystem::remove_all(db_->get_checkpoint_path(block_num), stale_error);
      if (stale_error) {
         elog("Failed to remove stale checkpoint for block ${block}: ${error}",
              ("block", block_num)("error", stale_error.message()));
         return false;
      }

      // Create checkpoint in RocksDB
      if (!db_->create_checkpoint(block_num)) {
         return false;
      }

      // Store rollback info
      rollback_info info;
      info.block_num = block_num;
      info.timestamp = fc::time_point::now();
      info.checkpoint_path = db_->get_checkpoint_path(block_num);

      rollback_points_[block_num] = info;

      if (!save_rollback_info(info)) {
         elog("Failed to save rollback info for block ${block}", ("block", block_num));
         rollback_points_.erase(block_num);
         refresh_summary();
         std::error_code cleanup_error;
         std::filesystem::remove_all(info.checkpoint_path, cleanup_error);
         return false;
      }

      refresh_summary();
      dlog("Created rollback point for block ${block}", ("block", block_num));
      return true;

   } catch (const std::exception& e) {
      elog("Exception creating rollback point for block ${block}: ${what}",
           ("block", block_num)("what", e.what()));
      return false;
   }
}

bool rollback_manager::rollback_to_block(uint32_t block_num) {
   auto it = rollback_points_.find(block_num);
   if (it == rollback_points_.end()) {
      elog("No rollback point found for block ${block}", ("block", block_num));
      return false;
   }

   try {
      // Perform database rollback
      if (!db_->rollback_to_block(block_num)) {
         return false;
      }

      // The checkpoint is created before its own rollback metadata is written,
      // so restore that metadata into the reopened database as part of rollback.
      if (!save_rollback_info(it->second)) {
         elog("Failed to restore rollback metadata for block ${block}", ("block", block_num));
         return false;
      }

      // Remove rollback points after the target block
      auto upper = rollback_points_.upper_bound(block_num);
      for (auto cleanup_it = upper; cleanup_it != rollback_points_.end(); ++cleanup_it) {
         // Remove checkpoint directory
         std::string checkpoint_path = db_->get_checkpoint_path(cleanup_it->first);
         try {
            std::filesystem::remove_all(checkpoint_path);
         } catch (const std::exception& e) {
            wlog("Failed to remove checkpoint directory ${path}: ${error}",
                 ("path", checkpoint_path)("error", e.what()));
         }

         // Remove rollback info from database
         std::string key = get_rollback_key(cleanup_it->first);
         db_->remove(key);
      }

      rollback_points_.erase(upper, rollback_points_.end());
      refresh_summary();

      ilog("Successfully rolled back to block ${block}", ("block", block_num));
      return true;

   } catch (const std::exception& e) {
      elog("Exception during rollback to block ${block}: ${what}",
           ("block", block_num)("what", e.what()));
      return false;
   }
}

void rollback_manager::cleanup_old_rollback_points(uint32_t keep_blocks,
                                                   uint64_t min_free_bytes) {
   const auto checkpoint_root = std::filesystem::path(db_->get_checkpoint_path(0)).parent_path();
   const auto available_bytes = [&]() {
      std::error_code error;
      const auto info = std::filesystem::space(checkpoint_root, error);
      return error ? std::numeric_limits<uint64_t>::max() : info.available;
   };

   size_t removed = 0;
   uint64_t free_bytes = available_bytes();
   while (rollback_points_.size() > 1 &&
          (rollback_points_.size() > keep_blocks ||
           (min_free_bytes != 0 && free_bytes < min_free_bytes))) {
      auto cleanup_it = rollback_points_.begin();
      // Remove checkpoint directory
      std::string checkpoint_path = db_->get_checkpoint_path(cleanup_it->first);
      std::error_code remove_error;
      std::filesystem::remove_all(checkpoint_path, remove_error);
      if (remove_error) {
         wlog("Failed to remove checkpoint directory ${path}: ${error}",
              ("path", checkpoint_path)("error", remove_error.message()));
         break;
      }

      // Remove rollback info from database
      std::string key = get_rollback_key(cleanup_it->first);
      db_->remove(key);
      rollback_points_.erase(cleanup_it);
      free_bytes = available_bytes();
      ++removed;
   }
   if (removed != 0) {
      refresh_summary();
      dlog("Cleaned up ${removed} rollback points; keeping ${count} points with ${bytes} bytes free",
           ("removed", removed)("count", rollback_points_.size())("bytes", free_bytes));
   }
}

void rollback_manager::cleanup_irreversible_rollback_points(uint32_t irreversible_block_num) {
   bool removed = false;
   auto cleanup_it = rollback_points_.begin();
   while (cleanup_it != rollback_points_.end() && cleanup_it->first < irreversible_block_num) {
      const std::string checkpoint_path = db_->get_checkpoint_path(cleanup_it->first);
      std::error_code remove_error;
      std::filesystem::remove_all(checkpoint_path, remove_error);
      if (remove_error) {
         wlog("Failed to remove irreversible checkpoint ${path}: ${error}",
              ("path", checkpoint_path)("error", remove_error.message()));
         break;
      }

      const std::string key = get_rollback_key(cleanup_it->first);
      if (!db_->remove(key)) {
         wlog("Failed to remove rollback metadata ${key}", ("key", key));
         break;
      }
      cleanup_it = rollback_points_.erase(cleanup_it);
      removed = true;
   }
   if (removed) {
      refresh_summary();
   }
}

std::optional<uint32_t> rollback_manager::get_latest_rollback_point() const {
   if (rollback_point_count_.load(std::memory_order_acquire) == 0) {
      return {};
   }
   const auto latest = latest_rollback_point_.load(std::memory_order_acquire);
   return latest;
}

size_t rollback_manager::rollback_point_count() const {
   return rollback_point_count_.load(std::memory_order_acquire);
}

bool rollback_manager::has_rollback_point(uint32_t block_num) const {
   return rollback_points_.find(block_num) != rollback_points_.end();
}

std::string rollback_manager::get_rollback_key(uint32_t block_num) const {
   return "rollback:" + std::to_string(block_num);
}

bool rollback_manager::save_rollback_info(const rollback_info& info) {
   try {
      std::string key = get_rollback_key(info.block_num);
      return db_->put_object(key, info);
   } catch (...) {
      return false;
   }
}

bool rollback_manager::load_rollback_points() {
   rollback_points_.clear();
   refresh_summary();
   if (!db_) {
      return false;
   }

   try {
      std::unique_ptr<rocksdb::Iterator> iterator(db_->new_iterator());
      if (!iterator) {
         return false;
      }

      constexpr const char* prefix = "rollback:";
      for (iterator->Seek(prefix); iterator->Valid(); iterator->Next()) {
         const std::string key = iterator->key().ToString();
         if (key.compare(0, std::char_traits<char>::length(prefix), prefix) != 0) {
            break;
         }

         rollback_info info;
         if (!db_->get_object(key, info)) {
            wlog("Ignoring invalid rollback metadata at key ${key}", ("key", key));
            continue;
         }

         const std::string checkpoint_path = db_->get_checkpoint_path(info.block_num);
         if (!std::filesystem::exists(checkpoint_path)) {
            wlog("Ignoring rollback metadata for missing checkpoint at block ${block}",
                 ("block", info.block_num));
            continue;
         }

         info.checkpoint_path = checkpoint_path;
         rollback_points_[info.block_num] = std::move(info);
      }
      refresh_summary();
      return true;
   } catch (const std::exception& e) {
      elog("Failed to load rollback points: ${error}", ("error", e.what()));
      rollback_points_.clear();
      refresh_summary();
      return false;
   }
}

void rollback_manager::refresh_summary() {
   rollback_point_count_.store(rollback_points_.size(), std::memory_order_release);
   latest_rollback_point_.store(
      rollback_points_.empty() ? 0 : rollback_points_.rbegin()->first,
      std::memory_order_release);
}

} // namespace eosio
