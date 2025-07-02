#pragma once

// #include <eosio/sign_transaction_plugin/account_query_db.hpp>
#include <eosio/chain_plugin/trx_retry_db.hpp>
#include <eosio/signature_provider_plugin/signature_provider_plugin.hpp>
#include <eosio/producer_plugin/producer_plugin.hpp>

#include <eosio/chain/application.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/plugin_interface.hpp>
#include <eosio/chain/types.hpp>

#include <boost/container/flat_set.hpp>

#include <fc/time.hpp>

namespace fc { class variant; }

namespace eosio { namespace chain { class abi_resolver; } }

namespace eosio { namespace sign_transaction {

   using chain::controller;
   using std::unique_ptr;
   using std::pair;
   using namespace appbase;
   using chain::name;
   using chain::uint128_t;
   using chain::public_key_type;
   using chain::transaction;
   using chain::transaction_id_type;
   using boost::container::flat_set;
   // using chain::asset;
   // using chain::symbol;
   using chain::authority;
   // using chain::account_name;
   using chain::action_name;
   using chain::abi_def;
   using chain::abi_serializer;
   using chain::abi_serializer_cache_builder;
   using chain::abi_resolver;
   using chain::packed_transaction;
   using eosio::chain_apis::trx_retry_db;
   using signature_provider_type = signature_provider_plugin::signature_provider_type;

   class sign_transaction_plugin_impl;

namespace chain_apis {

class read_write;

class api_base: public  eosio::chain_apis::api_base {
protected:
   template<class API, class Result>
   static void send_transaction_gen(API& api, send_transaction_params_t params, chain::plugin_interface::next_function<Result> next);
};

class read_write : public api_base {

   sign_transaction_plugin_impl& my;
   controller& chain;
   std::optional<trx_retry_db>& trx_retry;
   const fc::microseconds abi_serializer_max_time;
   const fc::microseconds http_max_response_time;
   const bool api_accept_transactions;
   friend class api_base;

public:
   read_write(sign_transaction_plugin_impl& my, controller& chain, std::optional<trx_retry_db>& trx_retry,
              const fc::microseconds& abi_serializer_max_time, const fc::microseconds& http_max_response_time,
              bool api_accept_transactions);
   void validate() const;

   // return deadline for call
   fc::time_point start() const {
      validate();
      return http_max_response_time == fc::microseconds::maximum() ? fc::time_point::maximum()
                                                                   : fc::time_point::now() + http_max_response_time;
   }

   using push_transaction_params = eosio::chain_apis::read_write::push_transaction_params;
   using push_transaction_results = eosio::chain_apis::read_write::push_transaction_results;
   // struct push_transaction_results {
   //    chain::transaction_id_type  transaction_id;
   //    fc::variant                 processed; // "processed" is expected JSON for trxs in client
   // };
   void push_transaction(const push_transaction_params& params, chain::plugin_interface::next_function<push_transaction_results> next);


   using push_transactions_params  = eosio::chain_apis::read_write::push_transactions_params;
   using push_transactions_results = eosio::chain_apis::read_write::push_transactions_results;
   void push_transactions(const push_transactions_params& params, chain::plugin_interface::next_function<push_transactions_results> next);

   using send_transaction_params = eosio::chain_apis::read_write::send_transaction_params;
   using send_transaction_results = eosio::chain_apis::read_write::send_transaction_results;
   void send_transaction(send_transaction_params params, chain::plugin_interface::next_function<send_transaction_results> next);

   using send_transaction2_params = eosio::chain_apis::read_write::send_transaction2_params;
   void send_transaction2(send_transaction2_params params, chain::plugin_interface::next_function<send_transaction_results> next);

};

} // namespace chain_apis

class sign_transaction_plugin : public plugin<sign_transaction_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin)(producer_plugin)
                           (signature_provider_plugin))

   sign_transaction_plugin();
   virtual ~sign_transaction_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();
   void handle_sighup() override;

   chain_apis::read_write get_read_write_api(const fc::microseconds& http_max_response_time);
   // chain_apis::read_only get_read_only_api(const fc::microseconds& http_max_response_time) const;

   bool accept_block( const chain::signed_block_ptr& block, const chain::block_id_type& id, const std::optional<chain::block_handle>& obt );
   void accept_transaction(const chain::packed_transaction_ptr& trx, chain::plugin_interface::next_function<chain::transaction_trace_ptr> next);

   // Only call this after plugin_initialize()!
   controller& chain();
   // Only call this after plugin_initialize()!
   const controller& chain() const;

   chain::chain_id_type get_chain_id() const;
   fc::microseconds get_abi_serializer_max_time() const;
   bool api_accept_transactions() const;
   // set true by other plugins if any plugin allows transactions
   bool accept_transactions() const;
   void enable_accept_transactions();
   // true if vote processing is enabled
   bool accept_votes() const;

   static void handle_guard_exception(const chain::guard_exception& e);

   // return variant of trace for logging, trace is modified to minimize log output
   fc::variant get_log_trx_trace(const chain::transaction_trace_ptr& trx_trace) const;
   // return variant of trx for logging, trace is modified to minimize log output
   fc::variant get_log_trx(const transaction& trx) const;

private:

   unique_ptr<class sign_transaction_plugin_impl> my;
};

}} // namespace eosio::sign_transaction


// FC_REFLECT( eosio::chain_apis::read_write::push_transaction_results, (transaction_id)(processed) )
// FC_REFLECT( eosio::chain_apis::read_write::send_transaction2_params, (return_failure_trace)(retry_trx)(retry_trx_num_blocks)(transaction) )
