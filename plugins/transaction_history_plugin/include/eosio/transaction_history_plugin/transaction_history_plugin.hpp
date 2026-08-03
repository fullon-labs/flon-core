#pragma once
#include <eosio/chain/application.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/types.hpp>
#include <eosio/chain/name.hpp>
#include <eosio/chain/application.hpp>
#include <boost/program_options.hpp>
#include <fc/variant.hpp>
#include <optional>

namespace eosio {

using eosio::chain::transaction_id_type;
using eosio::chain::name;
using eosio::chain::public_key_type;
using boost::program_options::options_description;
using boost::program_options::variables_map;

class transaction_history_plugin; // Forward declaration

namespace transaction_history_apis {

class read_only {
public:
   struct get_transaction_params {
      std::string id;
      std::optional<uint32_t> block_num_hint;
   };

   struct get_transaction_result {
      transaction_id_type id;
      fc::variant trx;
      eosio::chain::block_timestamp_type block_time;
      uint32_t block_num = 0;
      uint32_t last_irreversible_block = 0;
      std::vector<fc::variant> traces;
      fc::variant res_usage;
      fc::variant gas_traces;
   };

   struct get_actions_params {
      name account_name;
      std::optional<int32_t> pos;
      std::optional<int32_t> offset;
   };

   struct get_actions_result {
      std::vector<std::map<std::string, fc::variant>> actions;
      uint32_t last_irreversible_block;
      bool more = false;
      std::optional<bool> time_limit_exceeded_error;
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

   // Database management API structures
   struct get_database_stats_params {};

   struct get_database_stats_result {
      bool success;
      fc::variant stats;
      std::optional<std::string> error;
   };

   struct get_performance_metrics_params {};

   struct get_performance_metrics_result {
      bool success;
      fc::variant metrics;
      std::optional<std::string> error;
   };

   struct get_optimization_suggestions_params {};

   struct get_optimization_suggestions_result {
      bool success;
      fc::variant suggestions;
      std::optional<std::string> error;
   };

   struct get_cache_analysis_params {};

   struct get_cache_analysis_result {
      bool success;
      fc::variant analysis;
      std::optional<std::string> error;
   };

   struct get_maintenance_needs_params {};

   struct get_maintenance_needs_result {
      bool success;
      fc::variant maintenance_needs;
      std::optional<std::string> error;
   };

   struct trigger_auto_optimize_params {
      std::optional<uint32_t> max_duration_seconds;
   };

   struct trigger_auto_optimize_result {
      bool success;
      std::optional<std::string> message;
      std::optional<std::string> error;
   };

   // Methods
   get_transaction_result get_transaction(const get_transaction_params& params) const;
   get_actions_result get_actions(const get_actions_params& params) const;
   get_transaction_count_result get_transaction_count(const get_transaction_count_params& params) const;
   get_key_accounts_result get_key_accounts(const get_key_accounts_params& params) const;
   get_controlled_accounts_result get_controlled_accounts(const get_controlled_accounts_params& params) const;

   // Database management APIs
   get_database_stats_result get_database_stats(const get_database_stats_params& params) const;
   get_performance_metrics_result get_performance_metrics(const get_performance_metrics_params& params) const;
   get_optimization_suggestions_result get_optimization_suggestions(const get_optimization_suggestions_params& params) const;
   get_cache_analysis_result get_cache_analysis(const get_cache_analysis_params& params) const;
   get_maintenance_needs_result get_maintenance_needs(const get_maintenance_needs_params& params) const;
   trigger_auto_optimize_result trigger_auto_optimize(const trigger_auto_optimize_params& params) const;

private:
   const class transaction_history_plugin* history;

public:
   read_only(const transaction_history_plugin* h) : history(h) {}

}; // class read_only

} // namespace transaction_history_apis

class transaction_history_plugin_impl;

class transaction_history_plugin; // Forward declaration for typedef

using transaction_history_const_ptr = const transaction_history_plugin*;

class transaction_history_plugin : public appbase::plugin<transaction_history_plugin> {
   friend class transaction_history_apis::read_only;  // Allow read_only to access private members

public:
   APPBASE_PLUGIN_REQUIRES((eosio::chain_plugin))

   transaction_history_plugin();
   virtual ~transaction_history_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;
   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

   transaction_history_apis::read_only get_read_only_api() const {
      return transaction_history_apis::read_only(this);
   }

   std::string make_transaction_key(const transaction_id_type& id) const;
   std::string make_account_action_key(const eosio::chain::name& account, uint64_t seq) const;
   std::string make_block_transaction_key(uint32_t block_num, const transaction_id_type& id) const;

   // Internal access methods for read_only API
   uint32_t get_last_irreversible_block_num() const;
   std::shared_ptr<class rocksdb_manager> get_db_manager() const;

private:
   std::unique_ptr<transaction_history_plugin_impl> my;
};

} // namespace eosio

FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_params, (id)(block_num_hint))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_result, (id)(trx)(block_time)(block_num)(last_irreversible_block)(traces)(res_usage)(gas_traces))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_actions_params, (account_name)(pos)(offset))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_actions_result, (actions)(last_irreversible_block)(more)(time_limit_exceeded_error))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_count_params, (start_block)(end_block))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_transaction_count_result, (count)(start_block)(end_block))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_key_accounts_params, (public_key))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_key_accounts_result, (account_names))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_controlled_accounts_params, (controlling_account))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_controlled_accounts_result, (controlled_accounts))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_database_stats_params, )
FC_REFLECT(eosio::transaction_history_apis::read_only::get_database_stats_result, (success)(stats)(error))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_performance_metrics_params, )
FC_REFLECT(eosio::transaction_history_apis::read_only::get_performance_metrics_result, (success)(metrics)(error))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_optimization_suggestions_params, )
FC_REFLECT(eosio::transaction_history_apis::read_only::get_optimization_suggestions_result, (success)(suggestions)(error))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_cache_analysis_params, )
FC_REFLECT(eosio::transaction_history_apis::read_only::get_cache_analysis_result, (success)(analysis)(error))
FC_REFLECT(eosio::transaction_history_apis::read_only::get_maintenance_needs_params, )
FC_REFLECT(eosio::transaction_history_apis::read_only::get_maintenance_needs_result, (success)(maintenance_needs)(error))
FC_REFLECT(eosio::transaction_history_apis::read_only::trigger_auto_optimize_params, (max_duration_seconds))
FC_REFLECT(eosio::transaction_history_apis::read_only::trigger_auto_optimize_result, (success)(message)(error))
