#pragma once
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>
#include <fc/variant.hpp>
#include <fc/io/raw.hpp>
#include <memory>
#include <string>

namespace eosio {

/**
 * @brief RocksDB Database Manager
 *
 * Provides a high-level interface for RocksDB operations including
 * basic CRUD operations, batch operations, and checkpoint management
 * for rollback functionality.
 */
class rocksdb_manager {
public:
   rocksdb_manager();
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
    * @brief Close the database
    */
   void close();

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

   /**
    * @brief Delete a key
    * @param key The key to delete
    * @return true if successful, false otherwise
    */
   bool remove(const std::string& key);

   /**
    * @brief Perform batch write operations
    * @param writes Vector of key-value pairs to write
    * @param deletes Vector of keys to delete
    * @return true if successful, false otherwise
    */
   bool batch_write(const std::vector<std::pair<std::string, std::string>>& writes,
                   const std::vector<std::string>& deletes = {});

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
    * @brief Get database path
    * @return Database path string
    */
   const std::string& get_db_path() const { return db_path_; }

   /**
    * @brief Store an object using FC serialization
    * @tparam T Object type
    * @param key The key to store under
    * @param obj The object to store
    * @return true if successful, false otherwise
    */
   template<typename T>
   bool put_object(const std::string& key, const T& obj) {
      try {
         auto packed = fc::raw::pack(obj);
         return put(key, std::string(packed.data(), packed.size()));
      } catch (...) {
         return false;
      }
   }

   /**
    * @brief Retrieve an object using FC deserialization
    * @tparam T Object type
    * @param key The key to lookup
    * @param obj Output parameter for the object
    * @return true if successful, false otherwise
    */
   template<typename T>
   bool get_object(const std::string& key, T& obj) {
      std::string value;
      if (!get(key, value)) return false;
      try {
         fc::datastream<const char*> ds(value.data(), value.size());
         fc::raw::unpack(ds, obj);
         return true;
      } catch (...) {
         return false;
      }
   }

private:
   std::unique_ptr<rocksdb::DB> db_;
   rocksdb::Options options_;
   std::string db_path_;
   bool is_open_;
};

} // namespace eosio
