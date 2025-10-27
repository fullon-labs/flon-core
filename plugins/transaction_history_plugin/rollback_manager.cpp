#include <eosio/transaction_history_plugin/rollback_manager.hpp>
#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <fc/log/logger.hpp>
#include <fc/io/raw.hpp>
#include <filesystem>

namespace eosio {

rollback_manager::rollback_manager(std::shared_ptr<rocksdb_manager> db) : db_(db) {
   load_rollback_points();
}

rollback_manager::~rollback_manager() {
}

bool rollback_manager::create_rollback_point(uint32_t block_num) {
   try {
      // Create checkpoint in RocksDB
      if (!db_->create_checkpoint(block_num)) {
         return false;
      }

      // Store rollback info
      rollback_info info;
      info.block_num = block_num;
      info.timestamp = fc::time_point::now();
      info.checkpoint_path = "checkpoint_" + std::to_string(block_num);

      rollback_points_[block_num] = info;

      if (!save_rollback_info(info)) {
         elog("Failed to save rollback info for block ${block}", ("block", block_num));
         return false;
      }

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

      // Remove rollback points after the target block
      auto upper = rollback_points_.upper_bound(block_num);
      for (auto cleanup_it = upper; cleanup_it != rollback_points_.end(); ++cleanup_it) {
         // Remove checkpoint directory
         std::string checkpoint_path = db_->get_db_path() + "/" + cleanup_it->second.checkpoint_path;
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

      ilog("Successfully rolled back to block ${block}", ("block", block_num));
      return true;

   } catch (const std::exception& e) {
      elog("Exception during rollback to block ${block}: ${what}",
           ("block", block_num)("what", e.what()));
      return false;
   }
}

void rollback_manager::cleanup_old_rollback_points(uint32_t keep_blocks) {
   if (rollback_points_.size() <= keep_blocks) {
      return;
   }

   auto it = rollback_points_.begin();
   std::advance(it, rollback_points_.size() - keep_blocks);

   for (auto cleanup_it = rollback_points_.begin(); cleanup_it != it; ++cleanup_it) {
      // Remove checkpoint directory
      std::string checkpoint_path = db_->get_db_path() + "/" + cleanup_it->second.checkpoint_path;
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

   rollback_points_.erase(rollback_points_.begin(), it);

   dlog("Cleaned up old rollback points, keeping ${count} recent points", ("count", keep_blocks));
}

std::optional<uint32_t> rollback_manager::get_latest_rollback_point() const {
   if (rollback_points_.empty()) {
      return {};
   }

   return rollback_points_.rbegin()->first;
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
   // In a production implementation, this would iterate through all rollback keys
   // For now, we'll start with an empty set and build as we create new checkpoints
   rollback_points_.clear();
   return true;
}

} // namespace eosio
