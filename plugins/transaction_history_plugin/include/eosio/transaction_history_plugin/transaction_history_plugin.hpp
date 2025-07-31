#pragma once
#include <eosio/chain/application.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/trace.hpp>
#include <fc/io/json.hpp>
#include <boost/program_options/variables_map.hpp>
#include <memory>

namespace eosio {

using namespace appbase;
using namespace eosio::chain;
using boost::program_options::variables_map;
using boost::program_options::options_description;

typedef std::shared_ptr<class transaction_history_plugin_impl> transaction_history_ptr;
typedef std::shared_ptr<const class transaction_history_plugin_impl> transaction_history_const_ptr;

namespace transaction_history_apis {

class read_only {
   transaction_history_const_ptr history;

public:
   read_only(transaction_history_const_ptr&& history)
      : history(history) {}

   struct get_transaction_params {
      transaction_id_type id;
      std::optional<uint32_t> block_num_hint;
   };

   struct get_transaction_result {
      transaction_id_type id;
      fc::variant trx;
      fc::time_point_sec block_time;
      uint32_t block_num;
      uint32_t last_irreversible_block;
      std::vector<fc::variant> traces;
   };

   struct get_actions_params {
      name account_name;
      std::optional<int32_t> pos;
      std::optional<int32_t> offset;
   };

   struct get_actions_result {
      std::vector<fc::variant> actions;
      uint32_t last_irreversible_block;
      bool more;
   };

   struct get_transaction_count_params {
      std::optional<uint32_t> start_block;
      std::optional<uint32_t> end_block;
   };

   struct get_transaction_count_result {
      uint64_t count;
      uint32_t start_block;
      uint32_t end_block;
   };

   struct get_key_accounts_params {
      public_key_type public_key;
   };

   struct get_key_accounts_result {
      std::vector<name> account_names;
   };

   struct get_controlled_accounts_params {
      name controlling_account;
   };

   struct get_controlled_accounts_result {
      std::vector<name> controlled_accounts;
   };

   get_transaction_result get_transaction(const get_transaction_params& params) const;
   get_actions_result get_actions(const get_actions_params& params) const;
   get_transaction_count_result get_transaction_count(const get_transaction_count_params& params) const;
   get_key_accounts_result get_key_accounts(const get_key_accounts_params& params) const;
   get_controlled_accounts_result get_controlled_accounts(const get_controlled_accounts_params& params) const;
};

} // namespace transaction_history_apis

/**
 * @brief Transaction History Plugin
 *
 * This plugin provides comprehensive transaction history recording functionality
 * using RocksDB as the storage backend. It processes transactions asynchronously
 * to avoid blocking the main chain processing thread.
 *
 * Key features:
 * - Asynchronous processing using worker threads
 * - RocksDB storage for high performance and reliability
 * - Rollback support through checkpoints
 * - Configurable retention policies and size limits
 * - Account-level action indexing for efficient queries
 * - Transaction trace compression to save storage space
 * - Real-time statistics and performance monitoring
 * - Configurable filters for selective transaction recording
 *
 * Configuration options:
 * - transaction-history-dir: Database storage location
 * - transaction-history-max-retained-blocks: Block retention limit
 * - transaction-history-max-trace-size: Maximum trace size per transaction
 * - transaction-history-max-actions-per-tx: Maximum actions indexed per transaction
 * - transaction-history-compression: Enable/disable compression
 * - transaction-history-filter-on: Include specific account:action:actor patterns
 * - transaction-history-filter-out: Exclude specific account:action:actor patterns
 *
 * API endpoints:
 * - get_transaction: Retrieve transaction by ID with optional block hint
 * - get_actions: Get actions for a specific account with pagination
 * - get_transaction_count: Count transactions in a block range
 * - get_key_accounts: Find accounts associated with a public key
 * - get_controlled_accounts: Find accounts controlled by another account
 */
class transaction_history_plugin : public appbase::plugin<transaction_history_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((eosio::chain_plugin))

   transaction_history_plugin();
   virtual ~transaction_history_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg);
   virtual void plugin_initialize(const variables_map& options);
   virtual void plugin_startup();
   virtual void plugin_shutdown();

   transaction_history_apis::read_only get_read_only_api() const {
      return transaction_history_apis::read_only(transaction_history_const_ptr(my));
   }

private:
   transaction_history_ptr my;
};

} // namespace eosio

FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_params, (id)(block_num_hint))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_result, (id)(trx)(block_time)(block_num)(last_irreversible_block)(traces))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_actions_params, (account_name)(pos)(offset))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_actions_result, (actions)(last_irreversible_block)(more))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_count_params, (start_block)(end_block))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_count_result, (count)(start_block)(end_block))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_key_accounts_params, (public_key))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_key_accounts_result, (account_names))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_controlled_accounts_params, (controlling_account))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_controlled_accounts_result, (controlled_accounts))