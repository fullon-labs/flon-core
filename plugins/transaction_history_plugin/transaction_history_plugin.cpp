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
   bool compression_enabled_ = true;
   bool filter_on_star = true;

   // Constants for monitoring and limits
   static constexpr uint32_t MAX_RETAINED_BLOCKS = 1000;
   static constexpr uint32_t MAX_TRACE_SIZE = 10 * 1024 * 1024; // 10MB
   static constexpr uint32_t MAX_ACTIONS_PER_TX = 1000;

   // Statistics for monitoring
   std::atomic<uint64_t> transactions_processed_{0};
   std::atomic<uint64_t> transactions_failed_{0};
   std::atomic<uint64_t> total_processing_time_us_{0};
   fc::time_point startup_time_;

   // For monitoring and warnings
   std::atomic<uint32_t> last_warning_block_{0};
   const uint32_t warning_interval_ = 10000; // Warn every 10000 blocks
   const uint32_t max_safe_pending_blocks_ = 5000; // Warn if pending blocks exceed this

   void applied_transaction(const eosio::chain::transaction_trace_ptr& trace);
   void check_data_size_warnings(uint32_t current_block_num, uint32_t lib_block_num);

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

      if (options.count("transaction-history-compression")) {
         my->compression_enabled_ = options.at("transaction-history-compression").as<bool>();
      }

      // Validate configuration parameters
      EOS_ASSERT(!my->db_path_.empty(), eosio::chain::plugin_exception,
                 "transaction-history-dir cannot be empty");

      ilog("Transaction history monitoring: max trace size: ${trace_size} bytes, max retained blocks: ${blocks}, max actions per tx: ${actions}",
           ("trace_size", transaction_history_plugin_impl::MAX_TRACE_SIZE)
           ("blocks", transaction_history_plugin_impl::MAX_RETAINED_BLOCKS)
           ("actions", transaction_history_plugin_impl::MAX_ACTIONS_PER_TX));

      // Initialize components
      my->db_ = std::make_shared<rocksdb_manager>();
      my->worker_ = std::make_unique<async_worker>();
      my->rollback_mgr_ = std::make_unique<rollback_manager>(my->db_);

      if (!my->db_->open(my->db_path_)) {
         throw std::runtime_error("Failed to open transaction history database at: " + my->db_path_);
      }

      ilog("Transaction history plugin initialized with database at: ${path}",
           ("path", my->db_path_));

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

         // Check for data size warnings periodically
         if (result.block_num > last_warning_block_ + warning_interval_) {
            last_warning_block_ = result.block_num;
            check_data_size_warnings(result.block_num, result.last_irreversible_block);
         }

         // Convert action traces to variants with size monitoring
         size_t total_size = 0;

         for (const auto& action_trace : trace->action_traces) {
            fc::variant action_var;
            fc::to_variant(action_trace, action_var);

            size_t action_size = fc::raw::pack_size(action_var);
            total_size += action_size;

            result.traces.push_back(action_var);
         }

         // Warn if transaction trace size exceeds configured limit
         if (total_size > MAX_TRACE_SIZE) {
            wlog("Transaction ${id} trace size ${size} bytes exceeds limit ${limit}, but storing anyway",
                 ("id", trace->id)("size", total_size)("limit", MAX_TRACE_SIZE));
         }

         // Store transaction data with error checking
         if (!db_->put_object(trx_key, result)) {
            elog("Failed to store transaction ${id}", ("id", trace->id));
            return;
         }

         // Create account-level indexes for action queries with limit
         size_t indexed_actions = 0;
         for (const auto& action_trace : trace->action_traces) {
            if (action_trace.receipt && indexed_actions < MAX_ACTIONS_PER_TX) {
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

         // Warn if too many actions were skipped in indexing
         if (trace->action_traces.size() > MAX_ACTIONS_PER_TX) {
            wlog("Transaction ${id} has ${total} actions, only indexed first ${indexed} actions",
                 ("id", trace->id)("total", trace->action_traces.size())("indexed", MAX_ACTIONS_PER_TX));
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

void transaction_history_plugin_impl::check_data_size_warnings(uint32_t current_block_num, uint32_t lib_block_num) {
   // Calculate pending (reversible) blocks
   uint32_t pending_blocks = current_block_num > lib_block_num ? current_block_num - lib_block_num : 0;

   // Warn if there are too many pending blocks (could indicate slow finality)
   if (pending_blocks > max_safe_pending_blocks_) {
      wlog("Transaction history warning: ${pending} pending blocks (head: ${head}, LIB: ${lib}). "
           "Consider checking network finality. Storage usage may be high.",
           ("pending", pending_blocks)("head", current_block_num)("lib", lib_block_num));
   }

   // Warn about max_retained_blocks setting if it's too low
   if (MAX_RETAINED_BLOCKS < pending_blocks + 1000) {
      wlog("Transaction history warning: max-retained-blocks (${max}) is close to pending blocks (${pending}). "
           "This setting is only used for documentation and warnings, not for data cleanup.",
           ("max", MAX_RETAINED_BLOCKS)("pending", pending_blocks));
   }

   // Optional: Check rollback manager status
   if (rollback_mgr_) {
      auto latest_rollback = rollback_mgr_->get_latest_rollback_point();
      if (latest_rollback.has_value()) {
         uint32_t rollback_age = current_block_num > *latest_rollback ? current_block_num - *latest_rollback : 0;
         if (rollback_age > 1000) {
            dlog("Transaction history info: Latest rollback point is ${age} blocks old (block ${rollback})",
                 ("age", rollback_age)("rollback", *latest_rollback));
         }
      }
   }
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
