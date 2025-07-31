#include <eosio/transaction_history_plugin/transaction_history_plugin.hpp>
#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <eosio/transaction_history_plugin/async_worker.hpp>
#include <eosio/transaction_history_plugin/rollback_manager.hpp>

#include <eosio/chain/controller.hpp>
#include <eosio/chain/trace.hpp>
#include <fc/io/json.hpp>
#include <fc/crypto/sha256.hpp>

#include <boost/signals2/connection.hpp>
#include <boost/program_options.hpp>

namespace eosio {
namespace bpo = boost::program_options;

static auto _transaction_history_plugin = application::register_plugin<transaction_history_plugin>();

class transaction_history_plugin_impl {
public:
   eosio::chain_plugin* chain_plug = nullptr;
   std::shared_ptr<rocksdb_manager> db_;
   std::unique_ptr<async_worker> worker_;
   std::unique_ptr<rollback_manager> rollback_mgr_;

   boost::signals2::scoped_connection applied_transaction_connection_;

   std::string db_path_;
   uint32_t max_retained_blocks_ = 1000;
   uint32_t max_trace_size_ = 10 * 1024 * 1024; // 10MB
   uint32_t max_actions_per_tx_ = 1000;
   bool compression_enabled_ = true;
   bool filter_on_star = true;

   // Statistics for monitoring
   std::atomic<uint64_t> transactions_processed_{0};
   std::atomic<uint64_t> transactions_failed_{0};
   std::atomic<uint64_t> total_processing_time_us_{0};
   fc::time_point startup_time_;

   void applied_transaction(const eosio::chain::transaction_trace_ptr& trace);

   std::string make_transaction_key(const eosio::chain::transaction_id_type& id) const;
   std::string make_account_action_key(const eosio::chain::name& account, uint64_t seq) const;
   std::string make_block_transaction_key(uint32_t block_num, const eosio::chain::transaction_id_type& id) const;
};

transaction_history_plugin::transaction_history_plugin() : my(new transaction_history_plugin_impl()) {
}

transaction_history_plugin::~transaction_history_plugin() {
}

void transaction_history_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()
      ("transaction-history-dir", bpo::value<std::string>()->default_value("transaction_history"),
       "The location of the transaction history database")
      ("transaction-history-max-retained-blocks", bpo::value<uint32_t>()->default_value(1000),
       "The maximum number of blocks to retain in transaction history")
      ("transaction-history-max-trace-size", bpo::value<uint32_t>()->default_value(10485760),
       "Maximum size in bytes for a single transaction trace (default: 10MB)")
      ("transaction-history-max-actions-per-tx", bpo::value<uint32_t>()->default_value(1000),
       "Maximum number of actions to index per transaction")
      ("transaction-history-compression", bpo::value<bool>()->default_value(true),
       "Enable compression for stored transaction data")
      ("transaction-history-filter-on", bpo::value<std::vector<std::string>>()->composing(),
       "Track actions which match account:action:actor. Actor may be blank to include all actors.")
      ("transaction-history-filter-out", bpo::value<std::vector<std::string>>()->composing(),
       "Do not track actions which match account:action:actor. Actor may be blank to exclude all actors.");
}

void transaction_history_plugin::plugin_initialize(const variables_map& options) {
   try {
      my->chain_plug = appbase::app().find_plugin<eosio::chain_plugin>();
      EOS_ASSERT(my->chain_plug, eosio::chain::missing_chain_plugin_exception, "");

      my->db_path_ = options.at("transaction-history-dir").as<std::string>();
      my->max_retained_blocks_ = options.at("transaction-history-max-retained-blocks").as<uint32_t>();

      if (options.count("transaction-history-max-trace-size")) {
         my->max_trace_size_ = options.at("transaction-history-max-trace-size").as<uint32_t>();
      }
      if (options.count("transaction-history-max-actions-per-tx")) {
         my->max_actions_per_tx_ = options.at("transaction-history-max-actions-per-tx").as<uint32_t>();
      }
      if (options.count("transaction-history-compression")) {
         my->compression_enabled_ = options.at("transaction-history-compression").as<bool>();
      }

      // Validate configuration parameters
      EOS_ASSERT(!my->db_path_.empty(), eosio::chain::plugin_exception,
                 "transaction-history-dir cannot be empty");
      EOS_ASSERT(my->max_retained_blocks_ > 0, eosio::chain::plugin_exception,
                 "transaction-history-max-retained-blocks must be greater than 0");
      EOS_ASSERT(my->max_retained_blocks_ <= 100000, eosio::chain::plugin_exception,
                 "transaction-history-max-retained-blocks cannot exceed 100000 for memory safety");

      // Initialize components
      my->db_ = std::make_shared<rocksdb_manager>();
      my->worker_ = std::make_unique<async_worker>();
      my->rollback_mgr_ = std::make_unique<rollback_manager>(my->db_);

      if (!my->db_->open(my->db_path_)) {
         throw std::runtime_error("Failed to open transaction history database at: " + my->db_path_);
      }

      ilog("Transaction history plugin initialized with database at: ${path}, max_retained_blocks: ${blocks}",
           ("path", my->db_path_)("blocks", my->max_retained_blocks_));

   } FC_LOG_AND_RETHROW()
}

void transaction_history_plugin::plugin_startup() {
   ilog("Starting transaction_history_plugin");

   my->startup_time_ = fc::time_point::now();

   auto& chain = my->chain_plug->chain();
   my->applied_transaction_connection_ = chain.applied_transaction().connect(
      [&](std::tuple<const eosio::chain::transaction_trace_ptr&, const eosio::chain::packed_transaction_ptr&> t) {
         my->applied_transaction(std::get<0>(t));
      });

   ilog("Transaction history plugin started successfully");
}

void transaction_history_plugin::plugin_shutdown() {
   ilog("Shutting down transaction_history_plugin");

   // Print statistics
   auto uptime = fc::time_point::now() - my->startup_time_;
   auto uptime_seconds = uptime.count() / 1000000;
   auto avg_processing_time = my->transactions_processed_ > 0 ?
      my->total_processing_time_us_ / my->transactions_processed_ : 0;

   ilog("Plugin statistics - Uptime: ${uptime}s, Transactions processed: ${processed}, Failed: ${failed}, "
        "Average processing time: ${avg_time}μs",
        ("uptime", uptime_seconds)("processed", my->transactions_processed_.load())
        ("failed", my->transactions_failed_.load())("avg_time", avg_processing_time));

   // Disconnect from chain signals
   my->applied_transaction_connection_.disconnect();

   // Stop worker threads and wait for completion
   if (my->worker_) {
      size_t pending = my->worker_->pending_tasks();
      if (pending > 0) {
         ilog("Waiting for ${count} pending tasks to complete", ("count", pending));
      }
      my->worker_->stop();
   }

   // Close database
   if (my->db_) {
      my->db_->close();
   }

   ilog("Transaction history plugin shutdown complete");
}

void transaction_history_plugin_impl::applied_transaction(const eosio::chain::transaction_trace_ptr& trace) {
   if (!trace || !trace->receipt) return;

   // Only process successful transactions to avoid storing failed ones
   if (trace->receipt->status != eosio::chain::transaction_receipt_header::executed &&
       trace->receipt->status != eosio::chain::transaction_receipt_header::soft_fail) {
      return;
   }

   // Process transaction asynchronously to avoid blocking main chain
   worker_->enqueue_task([this, trace]() {
      auto start_time = fc::time_point::now();

      try {
         fc::variant trace_var;
         fc::to_variant(*trace, trace_var);

         std::string trx_key = make_transaction_key(trace->id);

         transaction_history_apis::read_only::get_transaction_result result;
         result.id = trace->id;
         result.trx = trace_var;
         result.block_time = fc::time_point_sec(chain_plug->chain().pending_block_time());
         result.block_num = chain_plug->chain().head().block_num();
         result.last_irreversible_block = chain_plug->chain().last_irreversible_block_num();

         // Convert action traces to variants with size limit check
         size_t total_size = 0;
         const size_t max_trace_size = max_trace_size_; // Use configurable limit

         for (const auto& action_trace : trace->action_traces) {
            fc::variant action_var;
            fc::to_variant(action_trace, action_var);

            size_t action_size = fc::raw::pack_size(action_var);
            total_size += action_size;

            if (total_size > max_trace_size) {
               wlog("Transaction ${id} trace size ${size} bytes exceeds limit, truncating",
                    ("id", trace->id)("size", total_size));
               break;
            }

            result.traces.push_back(action_var);
         }

         // Store transaction data with error checking
         if (!db_->put_object(trx_key, result)) {
            elog("Failed to store transaction ${id}", ("id", trace->id));
            return;
         }

         // Create account-level indexes for action queries
         size_t indexed_actions = 0;
         for (const auto& action_trace : trace->action_traces) {
            if (action_trace.receipt && indexed_actions < max_actions_per_tx_) { // Use configurable limit
               std::string account_key = make_account_action_key(
                  action_trace.receipt->receiver,
                  action_trace.receipt->global_sequence
               );

               // Store minimal action info for account queries
               std::map<std::string, fc::variant> action_info;
               action_info["trx_id"] = trace->id;
               action_info["block_num"] = result.block_num;
               action_info["global_sequence"] = action_trace.receipt->global_sequence;
               action_info["account"] = action_trace.receipt->receiver;
               action_info["action_name"] = action_trace.act.name;

               if (!db_->put_object(account_key, action_info)) {
                  wlog("Failed to store account action index for ${account}:${seq}",
                       ("account", action_trace.receipt->receiver)("seq", action_trace.receipt->global_sequence));
               }
               indexed_actions++;
            }
         }

         auto processing_time = fc::time_point::now() - start_time;
         transactions_processed_++;
         total_processing_time_us_ += processing_time.count();

         if (processing_time.count() > 100000) { // Log if processing takes > 100ms
            dlog("Transaction ${id} processing took ${time}μs (${actions} actions, ${size} bytes)",
                 ("id", trace->id)("time", processing_time.count())("actions", indexed_actions)("size", total_size));
         }

      } catch (const std::exception& e) {
         transactions_failed_++;
         elog("Error processing transaction ${id}: ${what}",
              ("id", trace->id)("what", e.what()));
      } catch (...) {
         transactions_failed_++;
         elog("Unknown error processing transaction ${id}", ("id", trace->id));
      }
   });
}

std::string transaction_history_plugin_impl::make_transaction_key(const transaction_id_type& id) const {
   return "trx:" + id.str();
}

std::string transaction_history_plugin_impl::make_account_action_key(const name& account, uint64_t seq) const {
   return "acc:" + account.to_string() + ":" + std::to_string(seq);
}

std::string transaction_history_plugin_impl::make_block_transaction_key(uint32_t block_num, const transaction_id_type& id) const {
   return "blk:" + std::to_string(block_num) + ":" + id.str();
}

// read_only implementation
namespace transaction_history_apis {

read_only::get_transaction_result read_only::get_transaction(const get_transaction_params& params) const {
   std::string key = history->make_transaction_key(params.id);
   get_transaction_result result;

   if (!history->db_->get_object(key, result)) {
      EOS_THROW(chain::tx_not_found, "Transaction ${id} not found in history", ("id", params.id));
   }

   // Update current irreversible block
   result.last_irreversible_block = history->chain_plug->chain().last_irreversible_block_num();

   return result;
}

read_only::get_actions_result read_only::get_actions(const get_actions_params& params) const {
   get_actions_result result;
   result.last_irreversible_block = history->chain_plug->chain().last_irreversible_block_num();
   result.more = false;

   // TODO: Implement account action querying
   // This would involve iterating through account-specific keys in RocksDB
   // and reconstructing action data from stored transaction traces

   return result;
}

read_only::get_transaction_count_result read_only::get_transaction_count(const get_transaction_count_params& params) const {
   get_transaction_count_result result;
   result.count = 0;
   result.start_block = params.start_block.value_or(1);
   result.end_block = params.end_block.value_or(history->chain_plug->chain().head().block_num());

   // TODO: Implement transaction counting logic
   // This would involve iterating through block-specific transaction keys

   return result;
}

read_only::get_key_accounts_result read_only::get_key_accounts(const get_key_accounts_params& params) const {
   get_key_accounts_result result;

   // TODO: Implement key accounts lookup
   // This would require maintaining an index of public keys to accounts

   return result;
}

read_only::get_controlled_accounts_result read_only::get_controlled_accounts(const get_controlled_accounts_params& params) const {
   get_controlled_accounts_result result;

   // TODO: Implement controlled accounts lookup
   // This would require maintaining an index of account control relationships

   return result;
}

} // namespace transaction_history_apis

} // namespace eosio
