#pragma once
#include <eosio/chain/types.hpp>
#include <fc/time.hpp>
#include <fc/reflect/reflect.hpp>
#include <string>
#include <map>
#include <memory>

namespace eosio {

class rocksdb_manager;

/**
 * @brief Information about a rollback checkpoint
 */
struct rollback_info {
   uint32_t block_num;           ///< Block number of the checkpoint
   fc::time_point timestamp;     ///< When the checkpoint was created
   std::string checkpoint_path;  ///< Path to the checkpoint directory
};

/**
 * @brief Rollback Manager
 *
 * Manages rollback checkpoints for the transaction history database.
 * Uses RocksDB's checkpoint feature to create consistent snapshots
 * that can be used for rollback operations.
 */
class rollback_manager {
public:
   explicit rollback_manager(std::shared_ptr<rocksdb_manager> db);
   ~rollback_manager();

   /**
    * @brief Create a rollback checkpoint for a specific block
    * @param block_num Block number to create checkpoint for
    * @return true if successful, false otherwise
    */
   bool create_rollback_point(uint32_t block_num);

   /**
    * @brief Rollback database to a specific block
    * @param block_num Block number to rollback to
    * @return true if successful, false otherwise
    */
   bool rollback_to_block(uint32_t block_num);

   /**
    * @brief Clean up old rollback points
    * @param keep_blocks Number of recent rollback points to keep
    */
   void cleanup_old_rollback_points(uint32_t keep_blocks = 1000);

   /**
    * @brief Get the most recent rollback point
    * @return Block number of latest rollback point, or empty if none exist
    */
   std::optional<uint32_t> get_latest_rollback_point() const;

private:
   std::shared_ptr<rocksdb_manager> db_;
   std::map<uint32_t, rollback_info> rollback_points_;

   std::string get_rollback_key(uint32_t block_num) const;
   bool save_rollback_info(const rollback_info& info);
   bool load_rollback_points();
};

} // namespace eosio

FC_REFLECT(eosio::rollback_info, (block_num)(timestamp)(checkpoint_path))
