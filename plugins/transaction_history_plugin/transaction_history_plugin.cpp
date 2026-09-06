/*
 * Transaction History Plugin Implementation
 *
 * This plugin provides comprehensive transaction history tracking with intelligent
 * database state management for various startup scenarios:
 *
 * 1. Snapshot Loading:
 *    - Detects when chain is loaded from snapshot
 *    - Automatically clears future blocks if database is ahead of snapshot
 *    - Warns about potential gaps in historical data
 *
 * 2. Replay Operations:
 *    - Detects replay scenarios based on chain state analysis
 *    - Clears conflicting data that would be replayed
 *    - Maintains data consistency during replay process
 *
 * 3. Normal Operations:
 *    - Continues from last recorded state
 *    - Validates database consistency against chain state
 *    - Provides warnings for potential inconsistencies
 *
 * Features:
 *    - Configurable compression (LZ4/None) with mixed-mode support
 *    - Automatic database repair with user-controllable options
 *    - Force clean capability for complete database reset
 *    - Comprehensive health monitoring and statistics
 *    - Intelligent key-based cleanup with block number validation
 *    - Performance monitoring and periodic health checks
 *
 * Configuration Options:
 *    --transaction-history-dir: Database directory location
 *    --transaction-history-compression: Enable/disable LZ4 compression
 *    --transaction-history-auto-repair: Auto-repair database inconsistencies
 *    --transaction-history-force-clean: Force complete database cleanup
 */

#include <eosio/transaction_history_plugin/transaction_history_plugin.hpp>
#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <eosio/transaction_history_plugin/async_worker.hpp>
#include <eosio/transaction_history_plugin/rollback_manager.hpp>

#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/trace.hpp>
#include <eosio/chain/permission_object.hpp>
#include <fc/io/json.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/scoped_exit.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/program_options.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace eosio {
namespace bpo = boost::program_options;
using boost::program_options::options_description;
using boost::program_options::variables_map;

static auto _transaction_history_plugin = appbase::application::register_plugin<transaction_history_plugin>();

namespace {

size_t estimate_retained_trace_size(const eosio::chain::transaction_trace& trace,
                                    const eosio::chain::packed_transaction_ptr& packed) {
   // Exceptions contain nested dynamically allocated log data. They are rare
   // on stored successful traces, so retain the exact serialized fallback for
   // those cases while keeping the common path allocation-free.
   const bool has_nested_exception = trace.except || trace.failed_dtrx_trace ||
      std::any_of(trace.action_traces.begin(), trace.action_traces.end(),
                  [](const auto& action_trace) { return action_trace.except.has_value(); });
   if (has_nested_exception) {
      size_t total = fc::raw::pack_size(trace);
      if (packed) {
         const size_t packed_size = packed->get_estimated_size();
         if (packed_size > std::numeric_limits<size_t>::max() - total) {
            throw std::overflow_error("retained transaction size overflow");
         }
         total += packed_size;
      }
      return total;
   }

   size_t total = sizeof(trace) + 1024; // shared ownership and allocator overhead
   const auto add = [&total](size_t amount) {
      if (amount > std::numeric_limits<size_t>::max() - total) {
         throw std::overflow_error("retained transaction size overflow");
      }
      total += amount;
   };
   const auto add_elements = [&add](size_t count, size_t element_size) {
      if (element_size != 0 && count > std::numeric_limits<size_t>::max() / element_size) {
         throw std::overflow_error("retained transaction size overflow");
      }
      add(count * element_size);
   };

   add_elements(trace.action_traces.capacity(), sizeof(eosio::chain::action_trace));
   add_elements(trace.gas_traces.capacity(), sizeof(eosio::chain::account_gas_trace));
   for (const auto& action_trace : trace.action_traces) {
      add_elements(action_trace.act.authorization.capacity(), sizeof(eosio::chain::permission_level));
      add(action_trace.act.data.capacity());
      add(action_trace.console.capacity());
      add(action_trace.return_value.capacity());
      add_elements(action_trace.account_ram_deltas.capacity(), sizeof(eosio::chain::account_delta));
      if (action_trace.receipt) {
         add_elements(action_trace.receipt->auth_sequence.capacity(),
                      sizeof(decltype(action_trace.receipt->auth_sequence)::value_type));
      }
   }
   if (packed) add(packed->get_estimated_size());
   return total;
}

void update_atomic_max(std::atomic<uint64_t>& target, uint64_t value) {
   uint64_t current = target.load(std::memory_order_relaxed);
   while (current < value &&
          !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

} // namespace

class transaction_history_plugin_impl {
   friend class transaction_history_apis::read_only;  // Allow read_only to access private members

public:
   eosio::chain_plugin* chain_plug = nullptr;
   std::shared_ptr<rocksdb_manager> db_;
   std::unique_ptr<async_worker> worker_;
   std::unique_ptr<async_worker> maintenance_worker_;
   std::unique_ptr<rollback_manager> rollback_mgr_;

   boost::signals2::scoped_connection applied_transaction_connection_;
   boost::signals2::scoped_connection accepted_block_connection_;
   boost::signals2::scoped_connection block_start_connection_;

   struct filter_entry {
      eosio::chain::name receiver;
      eosio::chain::name action;
      eosio::chain::name actor;

      auto key() const { return std::tie(receiver, action, actor); }
      bool operator<(const filter_entry& other) const { return key() < other.key(); }
   };

   std::string db_path_;
   bool compression_enabled_ = true;
   bool auto_repair_enabled_ = true;
   bool force_clean_enabled_ = false;
   bool auto_compact_enabled_ = false;
   bool validate_on_startup_enabled_ = false;
   uint32_t maintenance_interval_ = 100000;
   bool detailed_monitoring_enabled_ = false;
   bool auto_tuning_enabled_ = false;
   uint32_t analysis_interval_ = 500000;
   bool filter_on_star = true;
   std::set<filter_entry> filter_on_;
   std::set<filter_entry> filter_out_;

   // Constants for monitoring and limits
   uint32_t max_retained_blocks_ = 1000;
   uint32_t max_trace_size_ = 10 * 1024 * 1024;
   uint32_t max_actions_per_tx_ = 1000;
   uint32_t max_account_indexes_per_tx_ = 4096;
   uint64_t max_write_batch_bytes_ = 64ull * 1024 * 1024;
   uint64_t max_api_response_bytes_ = 16ull * 1024 * 1024;
   uint64_t min_checkpoint_free_bytes_ = 5ull * 1024 * 1024 * 1024;
   uint64_t max_queue_tasks_ = async_worker::max_pending_tasks;
   uint64_t max_queue_bytes_ = async_worker::max_pending_bytes;
   uint64_t block_cache_bytes_ = 256ull * 1024 * 1024;
   static constexpr uint32_t MAX_API_RESULTS = 1000;
   static constexpr uint64_t MAX_COUNT_SCAN_KEYS = 1000000;
   static constexpr int64_t API_SCAN_TIME_US = 20000;

   // Statistics for monitoring
   std::atomic<uint64_t> transactions_processed_{0};
   std::atomic<uint64_t> transactions_failed_{0};
   std::atomic<uint64_t> total_processing_time_us_{0};
   std::atomic<bool> history_healthy_{true};
   std::atomic<uint32_t> history_gap_block_{0};
   std::atomic<bool> health_task_pending_{false};
   std::atomic<bool> maintenance_task_pending_{false};
   std::atomic<bool> analysis_task_pending_{false};
   std::atomic<uint64_t> maintenance_generation_{0};
   std::atomic<uint64_t> maintenance_tasks_coalesced_{0};
   std::atomic<uint64_t> maintenance_tasks_skipped_{0};
   std::atomic<uint64_t> accepted_block_batches_{0};
   std::atomic<uint64_t> accepted_block_batch_bytes_{0};
   std::atomic<uint64_t> accepted_block_batch_time_us_{0};
   std::atomic<uint64_t> accepted_block_batch_max_us_{0};
   std::atomic<uint64_t> checkpoints_created_{0};
   std::atomic<uint64_t> checkpoint_time_us_{0};
   std::atomic<uint64_t> checkpoint_max_us_{0};
   std::atomic<uint64_t> checkpoint_cleanup_time_us_{0};
   std::atomic<uint64_t> checkpoint_cleanup_max_us_{0};
   std::atomic<uint64_t> history_reference_misses_{0};
   std::atomic<uint64_t> history_read_errors_{0};
   fc::time_point startup_time_;

   // For monitoring and warnings
   std::atomic<uint32_t> last_warning_block_{0};
   std::atomic<uint32_t> last_health_check_block_{0};
   std::atomic<uint32_t> last_maintenance_block_{0};
   std::atomic<uint32_t> last_analysis_block_{0};
   std::mutex last_updated_block_mutex_;
   uint32_t last_updated_block_ = 0;
   // Accessed only by the ordered history worker. Transaction records are
   // staged until accepted_block so one RocksDB WriteBatch commits the whole
   // block immediately before its rollback point is registered.
   uint32_t pending_block_num_ = 0;
   uint64_t pending_block_write_bytes_ = 0;
   uint64_t pending_sequence_bytes_ = 0;
   uint64_t pending_transactions_ = 0;
   uint64_t pending_processing_time_us_ = 0;
   std::vector<std::pair<std::string, std::string>> pending_block_writes_;
   std::map<eosio::chain::name, uint64_t> pending_account_sequences_;
   const uint32_t warning_interval_ = 10000; // Warn every 10000 blocks
   const uint32_t health_check_interval_ = 50000; // Health check every 50000 blocks
   const uint32_t max_safe_pending_blocks_ = 5000; // Warn if pending blocks exceed this

   void applied_transaction(const eosio::chain::transaction_trace_ptr& trace,
                            const eosio::chain::packed_transaction_ptr& packed);
   void ensure_chain_parent(uint32_t parent_block_num, const std::string& parent_block_id);
   void record_history_gap(uint32_t block_num, const std::string& reason);
   bool batch_write_with_retry(const std::vector<std::pair<std::string, std::string>>& writes,
                               const std::vector<std::string>& deletes = {},
                               bool sync = false);
   bool block_batch_write_with_retry(
      uint32_t block_num,
      const std::vector<std::pair<std::string, std::string>>& writes,
      uint64_t& total_bytes);
   void check_data_size_warnings(uint32_t current_block_num, uint32_t lib_block_num);
   void schedule_periodic_maintenance(uint32_t block_num, uint32_t lib_block_num);
   bool filter_action(const eosio::chain::action_trace& action_trace) const;
   uint64_t load_account_sequence(const eosio::chain::name& account) const;
   bool commit_accepted_block(uint32_t block_num, const std::string& block_id);
   void clear_pending_block();

   std::string make_transaction_key(const eosio::chain::transaction_id_type& id) const;
   std::string make_account_action_key(const eosio::chain::name& account, uint64_t seq) const;
   std::string make_action_key(uint64_t global_sequence) const;
   std::string make_block_transaction_key(uint32_t block_num, const eosio::chain::transaction_id_type& id) const;
};

namespace {

std::string fixed_width_number(uint64_t value, size_t width) {
   std::ostringstream out;
   out << std::setw(width) << std::setfill('0') << value;
   return out.str();
}

template<typename T>
std::string object_to_json(const T& object) {
   fc::variant value;
   fc::to_variant(object, value);
   return fc::json::to_string(value, fc::time_point::maximum());
}

} // namespace

transaction_history_plugin::transaction_history_plugin() : my(new transaction_history_plugin_impl()) {
}

transaction_history_plugin::~transaction_history_plugin() {
}

void transaction_history_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()
      ("transaction-history-dir", bpo::value<std::filesystem::path>()->default_value("transaction_history"),
       "The location of the transaction history database (absolute path or relative to application data dir)")
      ("transaction-history-compression", bpo::value<bool>()->default_value(true),
       "Enable compression for stored transaction data")
      ("transaction-history-auto-repair", bpo::value<bool>()->default_value(true),
       "Automatically repair database inconsistencies on startup (e.g., clear future blocks when loading from snapshot)")
      ("transaction-history-force-clean", bpo::value<bool>()->default_value(false),
       "Force clean transaction history database on startup (removes all existing data)")
      ("transaction-history-auto-compact", bpo::value<bool>()->default_value(false),
       "Run one full database compaction during controlled startup")
      ("transaction-history-validate-on-startup", bpo::value<bool>()->default_value(false),
       "Validate and repair database integrity on startup")
      ("transaction-history-maintenance-interval", bpo::value<uint32_t>()->default_value(100000),
       "Accepted-block interval for cancellable read-only database validation")
      ("transaction-history-detailed-monitoring", bpo::value<bool>()->default_value(false),
       "Enable detailed performance monitoring and statistics logging (may impact performance)")
      ("transaction-history-auto-tuning", bpo::value<bool>()->default_value(false),
       "Enable automatic performance tuning recommendations and analysis")
      ("transaction-history-analysis-interval", bpo::value<uint32_t>()->default_value(500000),
       "Interval in blocks for comprehensive database analysis (distribution, optimization suggestions)")
      ("transaction-history-max-retained-blocks", bpo::value<uint32_t>()->default_value(1000),
       "Maximum number of rollback points to retain")
      ("transaction-history-max-trace-size", bpo::value<uint32_t>()->default_value(10 * 1024 * 1024),
       "Maximum retained transaction trace size in bytes")
      ("transaction-history-max-actions-per-tx", bpo::value<uint32_t>()->default_value(1000),
       "Maximum number of action traces indexed per transaction")
      ("transaction-history-max-account-indexes-per-tx", bpo::value<uint32_t>()->default_value(4096),
       "Maximum receiver/authorization account index entries per transaction")
      ("transaction-history-max-write-batch-size", bpo::value<uint64_t>()->default_value(64ull * 1024 * 1024),
       "Maximum serialized bytes in one atomic transaction-history write batch")
      ("transaction-history-max-api-response-size", bpo::value<uint64_t>()->default_value(16ull * 1024 * 1024),
       "Maximum serialized action bytes returned by one history API request")
      ("transaction-history-min-checkpoint-free-space", bpo::value<uint64_t>()->default_value(5ull * 1024 * 1024 * 1024),
       "Minimum filesystem free bytes preserved while retaining rollback data")
      ("transaction-history-max-queue-tasks", bpo::value<uint64_t>()->default_value(async_worker::max_pending_tasks),
       "Maximum number of pending transaction-history tasks before chain processing applies backpressure")
      ("transaction-history-max-queue-bytes", bpo::value<uint64_t>()->default_value(async_worker::max_pending_bytes),
       "Maximum estimated bytes retained by pending transaction-history tasks before chain processing applies backpressure")
      ("transaction-history-block-cache-size", bpo::value<uint64_t>()->default_value(256ull * 1024 * 1024),
       "RocksDB block cache capacity in bytes")
      ("transaction-history-filter-on", bpo::value<std::vector<std::string>>()->composing(),
       "Track actions which match account:action:actor. Actor may be blank to include all actors.")
      ("transaction-history-filter-out", bpo::value<std::vector<std::string>>()->composing(),
       "Do not track actions which match account:action:actor. Actor may be blank to exclude all actors.")
      ("filter-on,f", bpo::value<std::vector<std::string>>()->composing(),
       "Compatibility alias for transaction-history-filter-on")
      ("filter-out,F", bpo::value<std::vector<std::string>>()->composing(),
       "Compatibility alias for transaction-history-filter-out");
}

void transaction_history_plugin::plugin_initialize(const variables_map& options) {
   try {
      my->chain_plug = appbase::app().find_plugin<eosio::chain_plugin>();
      EOS_ASSERT(my->chain_plug, eosio::chain::missing_chain_plugin_exception, "");

      // Handle transaction-history-dir with proper path logic
      auto dir_option = options.at("transaction-history-dir").as<std::filesystem::path>();
      std::filesystem::path transaction_history_dir;
      if (dir_option.is_relative())
         transaction_history_dir = appbase::app().data_dir() / dir_option;
      else
         transaction_history_dir = dir_option;

      // Create directories if they don't exist
      std::filesystem::create_directories(transaction_history_dir);

      // Store the absolute path as string for use in other components
      my->db_path_ = transaction_history_dir.string();

      if (options.count("transaction-history-compression")) {
         my->compression_enabled_ = options.at("transaction-history-compression").as<bool>();
      }

      if (options.count("transaction-history-auto-repair")) {
         my->auto_repair_enabled_ = options.at("transaction-history-auto-repair").as<bool>();
      }

      if (options.count("transaction-history-force-clean")) {
         my->force_clean_enabled_ = options.at("transaction-history-force-clean").as<bool>();
      }

      if (options.count("transaction-history-auto-compact")) {
         my->auto_compact_enabled_ = options.at("transaction-history-auto-compact").as<bool>();
      }

      if (options.count("transaction-history-validate-on-startup")) {
         my->validate_on_startup_enabled_ = options.at("transaction-history-validate-on-startup").as<bool>();
      }

      if (options.count("transaction-history-maintenance-interval")) {
         my->maintenance_interval_ = options.at("transaction-history-maintenance-interval").as<uint32_t>();
      }

      if (options.count("transaction-history-detailed-monitoring")) {
         my->detailed_monitoring_enabled_ = options.at("transaction-history-detailed-monitoring").as<bool>();
      }

      if (options.count("transaction-history-auto-tuning")) {
         my->auto_tuning_enabled_ = options.at("transaction-history-auto-tuning").as<bool>();
      }

      if (options.count("transaction-history-analysis-interval")) {
         my->analysis_interval_ = options.at("transaction-history-analysis-interval").as<uint32_t>();
      }
      my->max_retained_blocks_ = options.at("transaction-history-max-retained-blocks").as<uint32_t>();
      my->max_trace_size_ = options.at("transaction-history-max-trace-size").as<uint32_t>();
      my->max_actions_per_tx_ = options.at("transaction-history-max-actions-per-tx").as<uint32_t>();
      my->max_account_indexes_per_tx_ = options.at("transaction-history-max-account-indexes-per-tx").as<uint32_t>();
      my->max_write_batch_bytes_ = options.at("transaction-history-max-write-batch-size").as<uint64_t>();
      my->max_api_response_bytes_ = options.at("transaction-history-max-api-response-size").as<uint64_t>();
      my->min_checkpoint_free_bytes_ = options.at("transaction-history-min-checkpoint-free-space").as<uint64_t>();
      my->max_queue_tasks_ = options.at("transaction-history-max-queue-tasks").as<uint64_t>();
      my->max_queue_bytes_ = options.at("transaction-history-max-queue-bytes").as<uint64_t>();
      my->block_cache_bytes_ = options.at("transaction-history-block-cache-size").as<uint64_t>();
      EOS_ASSERT(my->max_retained_blocks_ > 0 && my->max_trace_size_ > 0 &&
                 my->max_actions_per_tx_ > 0 && my->max_account_indexes_per_tx_ > 0 &&
                 my->max_write_batch_bytes_ > 0 && my->max_api_response_bytes_ > 0 &&
                 my->max_queue_tasks_ > 0 && my->max_queue_bytes_ > 0 &&
                 my->block_cache_bytes_ > 0,
                 eosio::chain::plugin_exception, "transaction history limits must be greater than zero");
      EOS_ASSERT(my->max_queue_tasks_ <= std::numeric_limits<size_t>::max() &&
                 my->max_queue_bytes_ <= std::numeric_limits<size_t>::max() &&
                 my->block_cache_bytes_ <= std::numeric_limits<size_t>::max(),
                 eosio::chain::plugin_exception,
                 "transaction history queue or cache limits exceed this platform's addressable size");
      EOS_ASSERT(my->max_trace_size_ <= my->max_queue_bytes_ &&
                 my->max_write_batch_bytes_ <= my->max_queue_bytes_ &&
                 my->max_api_response_bytes_ <= my->max_queue_bytes_,
                 eosio::chain::plugin_exception,
                 "transaction history trace, write batch, and API response limits cannot exceed the queue memory limit of ${max} bytes",
                 ("max", my->max_queue_bytes_));

      auto parse_filters = [](const std::vector<std::string>& values,
                              std::set<transaction_history_plugin_impl::filter_entry>& output,
                              bool allow_star, bool& star) {
         for (const auto& value : values) {
            if (allow_star && (value == "*" || value == "\"*\"")) {
               star = true;
               continue;
            }

            std::vector<std::string> fields;
            boost::split(fields, value, boost::is_any_of(":"));
            EOS_ASSERT(fields.size() == 3, fc::invalid_arg_exception,
                       "Invalid transaction history filter ${filter}; expected receiver:action:actor",
                       ("filter", value));

            // Accept both the documented empty wildcard and the commonly used
            // explicit '*' spelling for action and actor.
            if (fields[1] == "*") fields[1].clear();
            if (fields[2] == "*") fields[2].clear();

            transaction_history_plugin_impl::filter_entry entry{
               eosio::chain::name(fields[0]), eosio::chain::name(fields[1]), eosio::chain::name(fields[2])
            };
            EOS_ASSERT(entry.receiver.to_uint64_t() != 0, fc::invalid_arg_exception,
                       "Invalid transaction history filter ${filter}: receiver is required",
                       ("filter", value));
            output.insert(entry);
         }
      };

      if (options.count("transaction-history-filter-on") || options.count("filter-on")) {
         my->filter_on_star = false;
         if (options.count("transaction-history-filter-on")) {
            parse_filters(options.at("transaction-history-filter-on").as<std::vector<std::string>>(),
                          my->filter_on_, true, my->filter_on_star);
         }
         if (options.count("filter-on")) {
            parse_filters(options.at("filter-on").as<std::vector<std::string>>(),
                          my->filter_on_, true, my->filter_on_star);
         }
      }
      if (options.count("transaction-history-filter-out") || options.count("filter-out")) {
         bool ignored_star = false;
         if (options.count("transaction-history-filter-out")) {
            parse_filters(options.at("transaction-history-filter-out").as<std::vector<std::string>>(),
                          my->filter_out_, false, ignored_star);
         }
         if (options.count("filter-out")) {
            parse_filters(options.at("filter-out").as<std::vector<std::string>>(),
                          my->filter_out_, false, ignored_star);
         }
      }

      // Validate configuration parameters
      EOS_ASSERT(!my->db_path_.empty(), eosio::chain::plugin_exception,
                 "transaction-history-dir cannot be empty");

      ilog("Transaction history monitoring: max trace size: ${trace_size} bytes, max retained blocks: ${blocks}, max actions per tx: ${actions}, queue: ${queue_tasks} tasks/${queue_bytes} bytes, compression: ${compression}",
           ("trace_size", my->max_trace_size_)
           ("blocks", my->max_retained_blocks_)
           ("actions", my->max_actions_per_tx_)
           ("queue_tasks", my->max_queue_tasks_)
           ("queue_bytes", my->max_queue_bytes_)
           ("compression", my->compression_enabled_ ? "enabled" : "disabled"));

      // Initialize components
      my->db_ = std::make_shared<rocksdb_manager>(static_cast<size_t>(my->block_cache_bytes_));
      my->worker_ = std::make_unique<async_worker>(
         static_cast<size_t>(my->max_queue_tasks_), static_cast<size_t>(my->max_queue_bytes_));
      // Maintenance is deliberately isolated from the ordered history writer.
      // Its tasks do not retain transaction traces, so a small bounded queue is
      // enough to coalesce periodic work without delaying accepted blocks.
      my->maintenance_worker_ = std::make_unique<async_worker>(8, 1);

      if (!my->db_->open(my->db_path_, my->compression_enabled_)) {
         throw std::runtime_error("Failed to open transaction history database at: " + my->db_path_);
      }

      // Handle force clean option
      if (my->force_clean_enabled_) {
         ilog("Force clean enabled: clearing all transaction history data");
         EOS_ASSERT(my->db_->clear_all_data(), chain::plugin_exception,
                    "Failed to perform requested transaction history force clean");
         ilog("Force clean completed successfully");
      }

      // Validate database integrity if requested
      if (my->validate_on_startup_enabled_) {
         ilog("Validating database integrity on startup...");
         EOS_ASSERT(my->db_->validate_and_repair_database(), chain::plugin_exception,
                    "Transaction history validation or repair failed during startup");
      }

      // Perform compaction if requested
      if (my->auto_compact_enabled_) {
         ilog("Performing database compaction on startup...");
         if (!my->db_->compact_database()) {
            wlog("Database compaction failed, but continuing startup");
         }
      }

      // Log compression information
      std::string compression_info = my->db_->get_compression_info();
      ilog("Database compression status: ${info}", ("info", compression_info));

      ilog("Transaction history plugin initialized with database at: ${path}",
           ("path", my->db_path_));

   } FC_LOG_AND_RETHROW()
}

void transaction_history_plugin::plugin_startup() {
   ilog("Starting transaction_history_plugin");

   my->startup_time_ = fc::time_point::now();

   auto& chain = my->chain_plug->chain();

   uint32_t chain_head_block = chain.head().block_num();
   uint32_t earliest_available = chain.earliest_available_block_num();
   uint32_t lib_num = chain.last_irreversible_block_num();
   // Startup mode is explicit application state. Inferring snapshot/replay
   // from the retained block range misclassifies ordinary pruned nodes and can
   // route a healthy database through destructive repair logic.
   const auto& app_options = app().get_options();
   const bool is_snapshot_load = app_options.count("snapshot") != 0;
   const bool is_replay =
      (app_options.count("replay-blockchain") != 0 &&
       app_options.at("replay-blockchain").as<bool>()) ||
      (app_options.count("hard-replay-blockchain") != 0 &&
       app_options.at("hard-replay-blockchain").as<bool>());

   // Get current database state for logging
   uint32_t db_last_block = my->db_->get_last_block_number();

   ilog("Transaction history startup analysis - Chain head: ${head}, LIB: ${lib}, "
        "Earliest: ${earliest}, DB last: ${db}, Snapshot: ${snapshot}, Replay: ${replay}",
        ("head", chain_head_block)("lib", lib_num)("earliest", earliest_available)
        ("db", db_last_block)("snapshot", is_snapshot_load)("replay", is_replay));

   // Initialize database state based on startup conditions
   if (my->auto_repair_enabled_) {
      EOS_ASSERT(my->db_->check_and_repair_database_state(
                    chain_head_block, is_snapshot_load, is_replay),
                 chain::plugin_exception,
                 "Failed to initialize transaction history database state");
   } else {
      // Just check but don't repair - provide detailed warnings
      if (is_snapshot_load && db_last_block > chain_head_block) {
         wlog("Database contains blocks beyond snapshot point (DB: ${db}, Chain: ${chain}). "
              "Auto-repair is disabled. Consider enabling --transaction-history-auto-repair or manually cleaning the database.",
              ("db", db_last_block)("chain", chain_head_block));
      } else if (is_replay && db_last_block > chain_head_block) {
         wlog("Database may contain blocks that will be replayed (DB: ${db}, Chain: ${chain}). "
              "Auto-repair is disabled. Data inconsistencies may occur. Consider enabling --transaction-history-auto-repair.",
              ("db", db_last_block)("chain", chain_head_block));
      } else if (!is_snapshot_load && !is_replay && db_last_block > chain_head_block) {
         wlog("Database (block ${db}) is ahead of chain head (block ${chain}). "
              "This may indicate a previous unclean shutdown. Consider enabling --transaction-history-auto-repair.",
              ("db", db_last_block)("chain", chain_head_block));
      }
   }

   // A height alone cannot prove that the history database belongs to the
   // active branch. Verify the persisted accepted-block identity before any
   // new transaction is recorded. Snapshot/replay can make old branch
   // checkpoints actively dangerous, so discard unverified history rather
   // than allowing the first block_start event to restore it.
   const std::string chain_head_id = chain.head().id().str();
   std::string persisted_gap_block;
   if (my->db_->get("_internal_history_gap_block", persisted_gap_block)) {
      if (my->auto_repair_enabled_) {
         wlog("Transaction history contains a persisted gap at block ${block}; clearing incomplete history",
              ("block", persisted_gap_block));
         EOS_ASSERT(my->db_->clear_all_data(), chain::plugin_exception,
                    "Failed to clear incomplete transaction history");
         EOS_ASSERT(my->db_->batch_write({
              {"_internal_last_block_number", std::to_string(chain_head_block)},
              {"_internal_last_accepted_block_num", std::to_string(chain_head_block)},
              {"_internal_last_accepted_block_id", chain_head_id}
           }, {}), chain::plugin_exception,
           "Failed to establish transaction history baseline after repairing a gap");
         db_last_block = chain_head_block;
      } else {
         my->history_healthy_ = false;
         try {
            my->history_gap_block_ = static_cast<uint32_t>(std::stoul(persisted_gap_block));
         } catch (...) {
            my->history_gap_block_ = chain_head_block;
         }
         elog("Transaction history contains a persisted gap at block ${block}; recording disabled",
              ("block", persisted_gap_block));
      }
   }
   std::string stored_accepted_num;
   std::string stored_accepted_id;
   bool has_chain_identity = my->db_->get("_internal_last_accepted_block_num", stored_accepted_num) &&
                             my->db_->get("_internal_last_accepted_block_id", stored_accepted_id);
   std::optional<uint32_t> stored_accepted_block_num;
   bool database_was_empty = db_last_block == 0;
   if (database_was_empty) {
      std::unique_ptr<rocksdb::Iterator> iterator(my->db_->new_iterator());
      EOS_ASSERT(iterator, chain::plugin_exception, "Transaction history database is not open");
      for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
         const auto key = iterator->key();
         if (key != "_internal_compression_enabled" &&
             key != "_internal_last_block_number") {
            database_was_empty = false;
            break;
         }
      }
      EOS_ASSERT(iterator->status().ok(), chain::plugin_exception,
                 "Failed to scan transaction history during startup: ${error}",
                 ("error", iterator->status().ToString()));
   }
   // A database with records but no branch identity predates the safety
   // marker and cannot be proven compatible. Only a database that was empty
   // before startup repair may establish a new baseline without clearing.
   bool chain_identity_matches = !has_chain_identity && database_was_empty;
   if (has_chain_identity) {
      try {
         const uint32_t stored_num = std::stoul(stored_accepted_num);
         stored_accepted_block_num = stored_num;
         if (stored_num == chain_head_block) {
            chain_identity_matches = stored_accepted_id == chain_head_id;
         } else if (stored_num <= chain_head_block && stored_num >= earliest_available) {
            const auto active_id = chain.chain_block_id_for_num(stored_num);
            chain_identity_matches = active_id && active_id->str() == stored_accepted_id;
         } else if (stored_num < earliest_available && !is_snapshot_load && !is_replay) {
            // A normal pruned-node restart cannot verify this old branch
            // prefix. Treat it as provisionally matching so the explicit
            // startup-gap branch below can rebuild or disable recording.
            chain_identity_matches = true;
         } else {
            chain_identity_matches = false;
         }
      } catch (...) {
         chain_identity_matches = false;
      }
   }

   if (!chain_identity_matches) {
      if (my->auto_repair_enabled_) {
         wlog("Transaction history belongs to an unverified chain branch; clearing history and rollback data");
         EOS_ASSERT(my->db_->clear_all_data(), chain::plugin_exception,
                    "Failed to clear transaction history from an incompatible chain branch");
         EOS_ASSERT(my->db_->batch_write({
              {"_internal_last_block_number", std::to_string(chain_head_block)},
              {"_internal_last_accepted_block_num", std::to_string(chain_head_block)},
              {"_internal_last_accepted_block_id", chain_head_id}
           }, {}), chain::plugin_exception,
           "Failed to establish transaction history chain baseline");
      } else {
         my->history_healthy_ = false;
         elog("Transaction history chain identity does not match the active chain; recording disabled");
      }
   } else if (stored_accepted_block_num && *stored_accepted_block_num < chain_head_block) {
      // Matching an older block proves the branch prefix, but it does not fill
      // the blocks between that point and the already-open chain head. Those
      // callbacks will not be replayed during a normal startup.
      const uint32_t first_missing_block = *stored_accepted_block_num + 1;
      if (my->auto_repair_enabled_) {
         wlog("Transaction history stops at block ${stored} while chain head is ${head}; "
              "clearing incomplete history and establishing a new baseline",
              ("stored", *stored_accepted_block_num)("head", chain_head_block));
         EOS_ASSERT(my->db_->clear_all_data(), chain::plugin_exception,
                    "Failed to clear incomplete transaction history during startup");
         EOS_ASSERT(my->db_->batch_write({
              {"_internal_last_block_number", std::to_string(chain_head_block)},
              {"_internal_last_accepted_block_num", std::to_string(chain_head_block)},
              {"_internal_last_accepted_block_id", chain_head_id}
           }, {}), chain::plugin_exception,
           "Failed to establish transaction history baseline after startup gap repair");
         db_last_block = chain_head_block;
      } else {
         my->record_history_gap(first_missing_block,
            "transaction history is behind the chain head at startup");
      }
   } else if (!has_chain_identity) {
      EOS_ASSERT(my->db_->batch_write({
           {"_internal_last_block_number", std::to_string(chain_head_block)},
           {"_internal_last_accepted_block_num", std::to_string(chain_head_block)},
           {"_internal_last_accepted_block_id", chain_head_id}
        }, {}), chain::plugin_exception,
        "Failed to initialize transaction history chain baseline");
   } else if (db_last_block != chain_head_block && my->auto_repair_enabled_) {
      // Accepted-block identity is authoritative; repair only the lagging
      // summary cursor when the exact chain head was already committed.
      EOS_ASSERT(my->db_->update_last_block_number(chain_head_block), chain::plugin_exception,
                 "Failed to align transaction history block cursor with accepted chain head");
      db_last_block = chain_head_block;
   }

   // Load rollback metadata only after startup repair has finalized the active
   // branch and removed any incompatible rollback data.
   my->rollback_mgr_ = std::make_unique<rollback_manager>(my->db_);

   // A newly established or repaired baseline must itself be restorable. This
   // closes the window in which the first live block can fork before any
   // accepted-block callback has created an undo point for its parent.
   if (my->history_healthy_.load() &&
       !my->rollback_mgr_->has_rollback_point(chain_head_block) &&
       !my->rollback_mgr_->create_rollback_point(chain_head_block)) {
      my->record_history_gap(chain_head_block, "failed to create startup rollback point");
   }

   // Log final database statistics
   std::string db_stats = my->db_->get_database_stats();
   dlog("Transaction history database statistics: ${stats}", ("stats", db_stats));

   // Initialize the monotonic database height after any startup repair or cleanup.
   my->last_updated_block_ = my->db_->get_last_block_number();

   // A bounded, ordered writer ensures each accepted-block batch follows all
   // staged transaction work for that block.
   my->worker_->start();
   my->maintenance_worker_->start();

   // block_start is emitted before transactions for the next block. Queueing
   // the parent check here ensures a fork rollback runs before any replacement
   // branch transaction reaches RocksDB.
   my->block_start_connection_ = chain.block_start().connect(
      [&](uint32_t) {
         if (!my->history_healthy_.load()) {
            return;
         }
         const uint32_t parent_block_num = chain.head().block_num();
         const std::string parent_block_id = chain.head().id().str();
         if (!my->worker_->enqueue_task_with_backpressure([impl = my.get(), parent_block_num, parent_block_id]() {
                try {
                   impl->ensure_chain_parent(parent_block_num, parent_block_id);
                } catch (const std::exception& e) {
                   impl->record_history_gap(parent_block_num,
                      std::string("fork-parent task exception: ") + e.what());
                } catch (...) {
                   impl->record_history_gap(parent_block_num, "unknown fork-parent task exception");
                }
             })) {
            if (!app().is_quiting()) {
               my->record_history_gap(parent_block_num, "history worker stopped before fork-parent check");
            }
         }
      });

   my->applied_transaction_connection_ = chain.applied_transaction().connect(
      [&](std::tuple<const eosio::chain::transaction_trace_ptr&, const eosio::chain::packed_transaction_ptr&> t) {
         my->applied_transaction(std::get<0>(t), std::get<1>(t));
      });

   my->accepted_block_connection_ = chain.accepted_block().connect(
      [&](const eosio::chain::block_signal_params& event) {
         if (!my->history_healthy_.load()) {
            return;
         }
         const uint32_t block_num = std::get<0>(event)->block_num();
         const std::string block_id = std::get<1>(event).str();
         const uint32_t irreversible_block_num = chain.last_irreversible_block_num();
         if (!my->worker_->enqueue_task_with_backpressure([impl = my.get(), block_num, block_id, irreversible_block_num]() {
            try {
               if (!impl->history_healthy_.load()) {
                  return;
               }

               if (!impl->commit_accepted_block(block_num, block_id)) {
                  impl->record_history_gap(block_num, "failed to atomically persist accepted block history");
                  return;
               }

               const auto checkpoint_start = std::chrono::steady_clock::now();
               if (!impl->rollback_mgr_->create_rollback_point(block_num)) {
                  impl->record_history_gap(block_num, "failed to register accepted-block undo point");
                  return;
               }
               impl->checkpoints_created_++;
               const uint64_t rollback_point_us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - checkpoint_start).count();
               impl->checkpoint_time_us_ += rollback_point_us;
               update_atomic_max(impl->checkpoint_max_us_, rollback_point_us);
               // Reclaim rollback data once per block after registering the
               // new atomic undo record. Never sacrifice the reversible fork
               // window for a count or free-space target.
               auto cleanup_start = std::chrono::steady_clock::now();
               impl->rollback_mgr_->cleanup_irreversible_rollback_points(irreversible_block_num);
               impl->rollback_mgr_->cleanup_old_rollback_points(
                  impl->max_retained_blocks_, impl->min_checkpoint_free_bytes_,
                  irreversible_block_num);
               const uint64_t cleanup_us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - cleanup_start).count();
               impl->checkpoint_cleanup_time_us_ += cleanup_us;
               update_atomic_max(impl->checkpoint_cleanup_max_us_, cleanup_us);
               impl->schedule_periodic_maintenance(block_num, irreversible_block_num);
            } catch (const std::exception& e) {
               impl->record_history_gap(block_num,
                  std::string("accepted-block task exception: ") + e.what());
            } catch (...) {
               impl->record_history_gap(block_num, "unknown accepted-block task exception");
            }
             })) {
            if (!app().is_quiting()) {
               my->record_history_gap(block_num, "history worker stopped before accepted-block commit");
            }
         }
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
   my->block_start_connection_.disconnect();
   my->applied_transaction_connection_.disconnect();
   my->accepted_block_connection_.disconnect();

   // Stop worker threads and wait for completion
   if (my->worker_) {
      size_t pending = my->worker_->pending_tasks();
      if (pending > 0) {
         ilog("Waiting for ${count} pending tasks to complete", ("count", pending));
      }
      my->worker_->stop();
   }

   if (my->maintenance_worker_) {
      // Optional scans and analyses must not make a production restart wait for
      // an entire queued maintenance backlog. A running operation completes so
      // the DB is never closed underneath it.
      my->maintenance_generation_++;
      my->maintenance_worker_->stop(false);
   }

   // Close database
   if (my->db_) {
      my->db_->close();
   }

   ilog("Transaction history plugin shutdown complete");
}

void transaction_history_plugin_impl::ensure_chain_parent(
   uint32_t parent_block_num, const std::string& parent_block_id) {
   if (!history_healthy_.load()) {
      return;
   }

   // A new block_start without an intervening accepted_block means the prior
   // speculative block was aborted. Its staged history was never visible in
   // RocksDB and can be discarded without rollback.
   if (pending_block_num_ != 0) {
      dlog("Discarding unaccepted transaction history staged for block ${block}",
           ("block", pending_block_num_));
      clear_pending_block();
   }

   std::string stored_num_text;
   std::string stored_id;
   if (!db_->get("_internal_last_accepted_block_num", stored_num_text) ||
       !db_->get("_internal_last_accepted_block_id", stored_id)) {
      // Existing databases predate block identity tracking. Establish a safe
      // baseline; subsequent live forks will be detected before transactions.
      if (!db_->batch_write({
             {"_internal_last_accepted_block_num", std::to_string(parent_block_num)},
             {"_internal_last_accepted_block_id", parent_block_id}
          }, {})) {
         record_history_gap(parent_block_num, "failed to initialize chain identity");
      }
      return;
   }

   uint32_t stored_num = 0;
   try {
      stored_num = std::stoul(stored_num_text);
   } catch (...) {
      record_history_gap(parent_block_num, "invalid accepted block number in history database");
      return;
   }

   if (stored_num == parent_block_num && stored_id == parent_block_id) {
      return;
   }

   if (stored_num < parent_block_num) {
      record_history_gap(stored_num + 1,
                         "chain advanced beyond the persisted history baseline");
      return;
   }

   wlog("Chain fork detected by transaction history: database block ${db_block}, "
        "new parent block ${parent_block}; rolling back history before branch transactions",
        ("db_block", stored_num)("parent_block", parent_block_num));

   // Cooperative cancellation lets a long validation release the lifecycle
   // read lock promptly so fork rollback can acquire exclusive access.
   maintenance_generation_++;
   if (!rollback_mgr_->rollback_to_block(parent_block_num)) {
      record_history_gap(parent_block_num, "no usable rollback point for fork parent");
      return;
   }
   clear_pending_block();

   std::string restored_id;
   if (!db_->get("_internal_last_accepted_block_id", restored_id) ||
       restored_id != parent_block_id) {
      record_history_gap(parent_block_num, "fork rollback point belongs to a different branch");
      return;
   }

   std::lock_guard<std::mutex> lock(last_updated_block_mutex_);
   last_updated_block_ = db_->get_last_block_number();
}

void transaction_history_plugin_impl::applied_transaction(
   const eosio::chain::transaction_trace_ptr& trace,
   const eosio::chain::packed_transaction_ptr& packed) {
   if (!trace || !trace->receipt) return;
   if (!history_healthy_.load()) return;

   // Only process successful transactions to avoid storing failed ones
   if (trace->receipt->status != eosio::chain::transaction_receipt_header::executed &&
       trace->receipt->status != eosio::chain::transaction_receipt_header::soft_fail) {
      return;
   }

   // Capture immutable block metadata on the chain thread. Reading the controller
   // later from a worker can associate the transaction with a newer block.
   const uint32_t block_num = trace->block_num;
   const eosio::chain::block_timestamp_type block_time = trace->block_time;
   const uint32_t last_irreversible_block = chain_plug->chain().last_irreversible_block_num();

   size_t retained_trace_bytes = 0;
   try {
      retained_trace_bytes = estimate_retained_trace_size(*trace, packed);
   } catch (const std::exception& e) {
      transactions_failed_++;
      elog("Failed to measure transaction ${id} trace size: ${error}",
           ("id", trace->id)("error", e.what()));
      record_history_gap(block_num, "failed to measure retained transaction history size");
      return;
   }

   if (retained_trace_bytes > max_trace_size_) {
      transactions_failed_++;
      wlog("Dropping transaction ${id} history trace of ${size} bytes; limit is ${limit}",
           ("id", trace->id)("size", retained_trace_bytes)("limit", max_trace_size_));
      record_history_gap(block_num, "transaction history trace exceeds configured size limit");
      return;
   }

   // Required history events apply bounded backpressure instead of being
   // dropped. During replay this deliberately limits chain processing to the
   // rate at which the ordered RocksDB writer can preserve complete history.
   if (!worker_->enqueue_task_with_backpressure_and_size(retained_trace_bytes,
      [this, trace, packed, block_num, block_time, last_irreversible_block, retained_trace_bytes]() {
      auto start_time = fc::time_point::now();

      try {
         if (!history_healthy_.load()) {
            return;
         }
         std::vector<const eosio::chain::action_trace*> filtered_actions;
         filtered_actions.reserve(trace->action_traces.size());
         for (const auto& action_trace : trace->action_traces) {
            if (filter_action(action_trace)) {
               filtered_actions.push_back(&action_trace);
            }
         }

         if (filtered_actions.empty()) {
            return;
         }

         std::string trx_key = make_transaction_key(trace->id);

         transaction_history_apis::read_only::get_transaction_result result;
         result.id = trace->id;
         fc::mutable_variant_object transaction_value("receipt", *trace->receipt);
         const bool filtering_active = !filter_on_star || !filter_out_.empty();
         if (packed && !filtering_active) {
            transaction_value("trx", packed->get_signed_transaction());
         } else if (filtering_active) {
            transaction_value("filtered", true);
         }
         result.trx = std::move(transaction_value);
         result.block_time = block_time;
         result.block_num = block_num;
         result.last_irreversible_block = last_irreversible_block;
         fc::to_variant(trace->res_usage, result.res_usage);
         fc::to_variant(trace->gas_traces, result.gas_traces);

         std::vector<std::pair<std::string, std::string>> writes;
         size_t write_bytes = 0;
         const auto append_write = [&writes, &write_bytes, this](std::string key, std::string value) {
            const uint64_t candidate = write_bytes + key.size() + value.size();
            if (candidate > max_write_batch_bytes_) return false;
            write_bytes = candidate;
            writes.emplace_back(std::move(key), std::move(value));
            return true;
         };

         // Store every receipted action once under its global sequence. The
         // transaction envelope and account indexes both retain only a stable
         // reference, while receipt-less traces remain embedded for backward-
         // compatible visibility of exceptional execution results.
         const size_t total_size = retained_trace_bytes;

         for (const auto* action_trace : filtered_actions) {
            fc::variant action_var;
            fc::to_variant(*action_trace, action_var);
            if (!action_trace->receipt) {
               result.traces.push_back(std::move(action_var));
               continue;
            }

            const std::string action_key =
               make_action_key(action_trace->receipt->global_sequence);
            std::map<std::string, fc::variant> action_info;
            action_info["trx_id"] = trace->id;
            action_info["block_num"] = result.block_num;
            action_info["block_time"] = result.block_time;
            action_info["global_sequence"] = action_trace->receipt->global_sequence;
            action_info["global_action_seq"] = action_trace->receipt->global_sequence;
            action_info["account"] = action_trace->receipt->receiver;
            action_info["action_name"] = action_trace->act.name;
            action_info["action_trace"] = std::move(action_var);
            if (!append_write(action_key, object_to_json(action_info))) {
               transactions_failed_++;
               record_history_gap(result.block_num,
                                  "normalized action exceeds configured batch limit");
               return;
            }

            std::map<std::string, fc::variant> action_ref;
            action_ref["action_ref"] = action_key;
            result.traces.emplace_back(std::move(action_ref));
         }

         // Warn if transaction trace size exceeds configured limit
         if (total_size > max_trace_size_) {
            wlog("Transaction ${id} trace size ${size} bytes exceeds limit ${limit}, but storing anyway",
                 ("id", trace->id)("size", total_size)("limit", max_trace_size_));
         }

         const std::string transaction_json = object_to_json(result);
         const std::string block_key = make_block_transaction_key(result.block_num, trace->id);
         const uint64_t base_bytes = trx_key.size() + transaction_json.size() +
                                     block_key.size() + trx_key.size();
         if (base_bytes > max_write_batch_bytes_ ||
             !append_write(trx_key, transaction_json)) {
            transactions_failed_++;
            wlog("Dropping transaction ${id} history because its record exceeds the write batch byte limit",
                 ("id", trace->id));
            record_history_gap(result.block_num, "transaction history write exceeds configured batch limit");
            return;
         }
         EOS_ASSERT(append_write(block_key, trx_key), chain::plugin_exception,
                    "transaction history base write exceeded the validated byte budget");

         // Create account-level indexes for action queries with limit
         size_t indexed_actions = 0;
         std::map<eosio::chain::name, uint64_t> next_sequences;
         size_t account_indexes = 0;
         bool index_budget_exhausted = false;
         for (const auto* action_trace : filtered_actions) {
            if (action_trace->receipt && indexed_actions < max_actions_per_tx_ && !index_budget_exhausted) {
               const std::string action_key =
                  make_action_key(action_trace->receipt->global_sequence);

               std::set<eosio::chain::name> indexed_accounts{action_trace->receipt->receiver};
               for (const auto& authorization : action_trace->act.authorization) {
                  indexed_accounts.insert(authorization.actor);
               }

               for (const auto& account : indexed_accounts) {
                  if (account_indexes >= max_account_indexes_per_tx_) {
                     index_budget_exhausted = true;
                     break;
                  }
                  const bool new_account = next_sequences.find(account) == next_sequences.end();
                  auto pending_sequence = pending_account_sequences_.find(account);
                  const uint64_t account_sequence = new_account
                     ? (pending_sequence == pending_account_sequences_.end()
                           ? load_account_sequence(account)
                           : pending_sequence->second)
                     : next_sequences.at(account);
                  std::map<std::string, fc::variant> action_info;
                  action_info["trx_id"] = trace->id;
                  action_info["block_num"] = result.block_num;
                  action_info["block_time"] = result.block_time;
                  action_info["global_sequence"] = action_trace->receipt->global_sequence;
                  action_info["global_action_seq"] = action_trace->receipt->global_sequence;
                  action_info["account"] = action_trace->receipt->receiver;
                  action_info["action_name"] = action_trace->act.name;
                  action_info["account_action_seq"] = account_sequence;
                  action_info["action_ref"] = action_key;
                  const std::string account_key = make_account_action_key(account, account_sequence);
                  const std::string account_json = object_to_json(action_info);
                  const uint64_t index_bytes = account_key.size() + account_json.size();
                  if (index_bytes > max_write_batch_bytes_ ||
                      write_bytes > max_write_batch_bytes_ - index_bytes) {
                     index_budget_exhausted = true;
                     break;
                  }

                  auto [sequence_it, inserted] = next_sequences.try_emplace(account, account_sequence);
                  if (inserted) {
                     sequence_it->second = account_sequence;
                  }
                  ++sequence_it->second;
                  EOS_ASSERT(append_write(account_key, account_json), chain::plugin_exception,
                             "transaction history index exceeded the reserved byte budget");
                  ++account_indexes;
               }
               indexed_actions++;
            }
         }

         if (pending_block_num_ != 0 && pending_block_num_ != result.block_num) {
            transactions_failed_++;
            record_history_gap(result.block_num, "accepted-block event missing before next transaction");
            return;
         }

         uint64_t updated_sequence_bytes = pending_sequence_bytes_;
         for (const auto& [account, next_sequence] : next_sequences) {
            const std::string key = "_internal_account_sequence:" + account.to_string();
            const std::string value = std::to_string(next_sequence);
            auto existing = pending_account_sequences_.find(account);
            if (existing != pending_account_sequences_.end()) {
               const uint64_t old_bytes = key.size() + std::to_string(existing->second).size();
               EOS_ASSERT(updated_sequence_bytes >= old_bytes, chain::plugin_exception,
                          "transaction history sequence byte accounting underflow");
               updated_sequence_bytes -= old_bytes;
            }
            updated_sequence_bytes += key.size() + value.size();
         }
         constexpr uint64_t accepted_block_metadata_reserve = 512;
         const auto exceeds_block_budget = [this](uint64_t used, uint64_t addition) {
            return used > max_write_batch_bytes_ || addition > max_write_batch_bytes_ - used;
         };
         uint64_t staged_bytes = pending_block_write_bytes_;
         bool over_budget = exceeds_block_budget(staged_bytes, updated_sequence_bytes);
         if (!over_budget) staged_bytes += updated_sequence_bytes;
         over_budget = over_budget || exceeds_block_budget(staged_bytes, write_bytes);
         if (!over_budget) staged_bytes += write_bytes;
         over_budget = over_budget ||
            exceeds_block_budget(staged_bytes, accepted_block_metadata_reserve);
         if (over_budget) {
            transactions_failed_++;
            record_history_gap(result.block_num, "accepted block history exceeds configured batch limit");
            return;
         }

         pending_block_num_ = result.block_num;
         pending_block_write_bytes_ += write_bytes;
         pending_sequence_bytes_ = updated_sequence_bytes;
         pending_block_writes_.insert(pending_block_writes_.end(),
                                      std::make_move_iterator(writes.begin()),
                                      std::make_move_iterator(writes.end()));
         for (const auto& [account, next_sequence] : next_sequences) {
            pending_account_sequences_[account] = next_sequence;
         }

         // Warn if too many actions were skipped in indexing
         if (filtered_actions.size() > max_actions_per_tx_ || index_budget_exhausted) {
            wlog("Transaction ${id} has ${total} actions, only indexed first ${indexed} actions",
                 ("id", trace->id)("total", filtered_actions.size())("indexed", indexed_actions));
         }

         auto processing_time = fc::time_point::now() - start_time;
         pending_transactions_++;
         pending_processing_time_us_ += processing_time.count();

         if (processing_time.count() > 100000) { // Log if processing takes > 100ms
            dlog("Transaction ${id} processing took ${time}μs (${actions} actions, ${size} bytes)",
                 ("id", trace->id)("time", processing_time.count())("actions", indexed_actions)("size", total_size));
         }

      } catch (const std::exception& e) {
         transactions_failed_++;
         record_history_gap(block_num, std::string("transaction history processing exception: ") + e.what());
         elog("Error processing transaction ${id}: ${what}",
              ("id", trace->id)("what", e.what()));
      } catch (...) {
         transactions_failed_++;
         record_history_gap(block_num, "unknown transaction history processing exception");
         elog("Unknown error processing transaction ${id}", ("id", trace->id));
      }
      })) {
      if (!app().is_quiting()) {
         transactions_failed_++;
         record_history_gap(block_num, "history worker stopped or transaction exceeds queue byte limit");
      }
   }
}

void transaction_history_plugin_impl::record_history_gap(uint32_t block_num,
                                                          const std::string& reason) {
   bool expected = true;
   if (!history_healthy_.compare_exchange_strong(expected, false)) {
      return;
   }

   history_gap_block_ = block_num;
   const std::string bounded_reason = reason.substr(0, 512);
   auto database_lock = db_->acquire_read_lock();
   if (!batch_write_with_retry({
          {"_internal_history_gap_block", std::to_string(block_num)},
          {"_internal_history_gap_reason", bounded_reason}
       }, {}, true)) {
      elog("Failed to persist transaction history gap marker at block ${block}",
           ("block", block_num));
      // Continuing the node would make a later restart treat an incomplete
      // history database as healthy. Stop cleanly so operators cannot miss the
      // undurable safety marker.
      app().quit();
   }
   elog("Transaction history became incomplete at block ${block}: ${reason}; recording disabled",
        ("block", block_num)("reason", bounded_reason));
}

bool transaction_history_plugin_impl::commit_accepted_block(
   uint32_t block_num, const std::string& block_id) {
   if (pending_block_num_ != 0 && pending_block_num_ != block_num) {
      clear_pending_block();
      return false;
   }

   auto writes = std::move(pending_block_writes_);
   writes.reserve(writes.size() + pending_account_sequences_.size() + 4);
   for (const auto& [account, next_sequence] : pending_account_sequences_) {
      writes.emplace_back("_internal_account_sequence:" + account.to_string(),
                          std::to_string(next_sequence));
   }
   writes.emplace_back("_internal_last_block_number", std::to_string(block_num));
   writes.emplace_back("_internal_last_accepted_block_num", std::to_string(block_num));
   writes.emplace_back("_internal_last_accepted_block_id", block_id);
   writes.emplace_back("_internal_schema_version", "2");

   uint64_t batch_bytes = 0;
   const auto batch_start = std::chrono::steady_clock::now();
   const bool committed = block_batch_write_with_retry(block_num, writes, batch_bytes);
   if (committed) {
      const uint64_t batch_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
         std::chrono::steady_clock::now() - batch_start).count();
      accepted_block_batches_++;
      accepted_block_batch_bytes_ += batch_bytes;
      accepted_block_batch_time_us_ += batch_time_us;
      update_atomic_max(accepted_block_batch_max_us_, batch_time_us);
      transactions_processed_ += pending_transactions_;
      total_processing_time_us_ += pending_processing_time_us_;
      std::lock_guard<std::mutex> lock(last_updated_block_mutex_);
      last_updated_block_ = std::max(last_updated_block_, block_num);
   }
   clear_pending_block();
   return committed;
}

void transaction_history_plugin_impl::clear_pending_block() {
   pending_block_num_ = 0;
   pending_block_write_bytes_ = 0;
   pending_sequence_bytes_ = 0;
   pending_transactions_ = 0;
   pending_processing_time_us_ = 0;
   pending_block_writes_.clear();
   pending_account_sequences_.clear();
}

bool transaction_history_plugin_impl::batch_write_with_retry(
   const std::vector<std::pair<std::string, std::string>>& writes,
   const std::vector<std::string>& deletes,
   bool sync) {
   constexpr uint32_t max_attempts = 3;
   for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
      if (db_->batch_write(writes, deletes, sync)) {
         return true;
      }
      if (attempt != max_attempts) {
         std::this_thread::sleep_for(std::chrono::milliseconds(5u << (attempt - 1)));
      }
   }
   return false;
}

bool transaction_history_plugin_impl::block_batch_write_with_retry(
   uint32_t block_num,
   const std::vector<std::pair<std::string, std::string>>& writes,
   uint64_t& total_bytes) {
   constexpr uint32_t max_attempts = 3;
   for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
      if (db_->batch_write_with_undo(block_num, writes, {}, max_write_batch_bytes_,
                                     &total_bytes)) {
         return true;
      }
      if (db_->has_undo_point(block_num)) {
         // A successful atomic WriteBatch must not be repeated merely because
         // a lower layer returned an ambiguous status.
         std::string accepted_num;
         if (db_->get("_internal_last_accepted_block_num", accepted_num) &&
             accepted_num == std::to_string(block_num)) {
            return true;
         }
         return false;
      }
      if (attempt != max_attempts) {
         std::this_thread::sleep_for(std::chrono::milliseconds(5u << (attempt - 1)));
      }
   }
   return false;
}

bool transaction_history_plugin_impl::filter_action(const eosio::chain::action_trace& action_trace) const {
   const auto matches = [&action_trace](const std::set<filter_entry>& filters) {
      // Failed/exceptional inline actions may not have a receipt. Their code
      // account is the only stable receiver-like identity available and keeps
      // them visible under wildcard and contract-level filters.
      const auto receiver = action_trace.receipt
         ? action_trace.receipt->receiver
         : action_trace.act.account;
      const auto action = action_trace.act.name;
      if (filters.count({receiver, {}, {}}) || filters.count({receiver, action, {}})) {
         return true;
      }
      for (const auto& authorization : action_trace.act.authorization) {
         if (filters.count({receiver, {}, authorization.actor}) ||
             filters.count({receiver, action, authorization.actor})) {
            return true;
         }
      }
      return false;
   };

   if (!filter_on_star && !matches(filter_on_)) {
      return false;
   }
   return !matches(filter_out_);
}

void transaction_history_plugin_impl::schedule_periodic_maintenance(
   uint32_t block_num, uint32_t lib_block_num) {
   const auto interval_elapsed = [block_num](uint32_t last, uint32_t interval) {
      return interval != 0 && block_num > last && block_num - last >= interval;
   };

   if (interval_elapsed(last_warning_block_.load(), warning_interval_)) {
      last_warning_block_ = block_num;
      check_data_size_warnings(block_num, lib_block_num);
   }

   if (interval_elapsed(last_health_check_block_.load(), health_check_interval_)) {
      last_health_check_block_ = block_num;
      const uint64_t generation = maintenance_generation_.load();
      bool expected = false;
      if (!health_task_pending_.compare_exchange_strong(expected, true)) {
         maintenance_tasks_coalesced_++;
      } else if (!maintenance_worker_->try_enqueue_task([this, block_num, generation]() {
         auto reset_pending = fc::scoped_exit<std::function<void()>>(
            [this]() { health_task_pending_ = false; });
         if (maintenance_generation_.load() != generation) return;
         auto database_lock = db_->acquire_read_lock();
         if (maintenance_generation_.load() != generation) return;
         const bool health_ok = db_->health_check();
         if (maintenance_generation_.load() != generation) return;

         if (health_ok) {
            ilog("Transaction history database health check PASSED at block ${block}",
                 ("block", block_num));
            if (detailed_monitoring_enabled_) {
               dlog("Database statistics: ${stats}", ("stats", db_->get_database_stats()));
               dlog("Performance metrics: ${performance}",
                    ("performance", db_->get_performance_metrics()));
               dlog("Size breakdown: ${size}", ("size", db_->get_size_breakdown()));
            }
         } else {
            wlog("Transaction history database health check FAILED at block ${block}; "
                 "validate and repair during a controlled restart", ("block", block_num));
         }
      })) {
         health_task_pending_ = false;
         maintenance_tasks_skipped_++;
      }
   }

   if (interval_elapsed(last_maintenance_block_.load(), maintenance_interval_)) {
      last_maintenance_block_ = block_num;
      const uint64_t generation = maintenance_generation_.load();
      bool expected = false;
      if (!maintenance_task_pending_.compare_exchange_strong(expected, true)) {
         maintenance_tasks_coalesced_++;
      } else if (!maintenance_worker_->try_enqueue_task([this, block_num, generation]() {
         auto reset_pending = fc::scoped_exit<std::function<void()>>(
            [this]() { maintenance_task_pending_ = false; });
         const auto cancelled = [this, generation]() {
            return maintenance_generation_.load() != generation || !history_healthy_.load();
         };
         if (cancelled()) return;
         auto database_lock = db_->acquire_read_lock();
         if (cancelled()) return;
         ilog("Starting cancellable database validation at block ${block}", ("block", block_num));
         const bool valid = db_->validate_database(cancelled);
         if (cancelled()) {
            dlog("Cancelled transaction history validation for fork/shutdown at block ${block}",
                 ("block", block_num));
         } else if (valid) {
            ilog("Scheduled database validation completed successfully");
         } else {
            wlog("Scheduled database validation found issues; repair during a controlled restart");
         }
         if (auto_compact_enabled_) {
            dlog("Skipping live full-range compaction; RocksDB background compaction remains active");
         }
      })) {
         maintenance_task_pending_ = false;
         maintenance_tasks_skipped_++;
      }
   }

   if (auto_tuning_enabled_ && interval_elapsed(last_analysis_block_.load(), analysis_interval_)) {
      last_analysis_block_ = block_num;
      const uint64_t generation = maintenance_generation_.load();
      bool expected = false;
      if (!analysis_task_pending_.compare_exchange_strong(expected, true)) {
         maintenance_tasks_coalesced_++;
      } else if (!maintenance_worker_->try_enqueue_task([this, block_num, generation]() {
         auto reset_pending = fc::scoped_exit<std::function<void()>>(
            [this]() { analysis_task_pending_ = false; });
         if (maintenance_generation_.load() != generation) return;
         auto database_lock = db_->acquire_read_lock();
         if (maintenance_generation_.load() != generation) return;
         try {
            const std::string recommendations = db_->get_tuning_recommendations();
            if (maintenance_generation_.load() != generation) return;
            const std::string distribution = db_->analyze_key_distribution();
            if (maintenance_generation_.load() != generation) return;
            ilog("Database tuning recommendations at block ${block}: ${recommendations}",
                 ("block", block_num)("recommendations", recommendations));
            dlog("Database key distribution: ${distribution}", ("distribution", distribution));
         } catch (const std::exception& e) {
            elog("Error during database analysis: ${error}", ("error", e.what()));
         }
      })) {
         analysis_task_pending_ = false;
         maintenance_tasks_skipped_++;
      }
   }
}

uint64_t transaction_history_plugin_impl::load_account_sequence(const eosio::chain::name& account) const {
   const std::string counter_key = "_internal_account_sequence:" + account.to_string();
   std::string stored;
   if (db_->get(counter_key, stored)) {
      try {
         return std::stoull(stored);
      } catch (...) {
         wlog("Resetting invalid account history sequence for ${account}", ("account", account));
      }
   }
   return 0;
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

   // The configured count only limits irreversible/stale rollback points. The
   // cleanup code always preserves the reversible window, so warn only when
   // the configured target is actually below that window.
   if (max_retained_blocks_ < pending_blocks) {
      wlog("Transaction history warning: max-retained-blocks (${max}) is close to pending blocks (${pending}). "
           "Increase rollback retention to preserve the complete reversible fork window.",
           ("max", max_retained_blocks_)("pending", pending_blocks));
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
   return "acc:" + account.to_string() + ":" + fixed_width_number(seq, 20);
}

std::string transaction_history_plugin_impl::make_action_key(uint64_t global_sequence) const {
   return "act:" + fixed_width_number(global_sequence, 20);
}

std::string transaction_history_plugin_impl::make_block_transaction_key(uint32_t block_num, const transaction_id_type& id) const {
   return "blk:" + fixed_width_number(block_num, 10) + ":" + id.str();
}

// transaction_history_plugin public methods implementation
std::string transaction_history_plugin::make_transaction_key(const transaction_id_type& id) const {
   return my->make_transaction_key(id);
}

std::string transaction_history_plugin::make_account_action_key(const eosio::chain::name& account, uint64_t seq) const {
   return my->make_account_action_key(account, seq);
}

std::string transaction_history_plugin::make_block_transaction_key(uint32_t block_num, const transaction_id_type& id) const {
   return my->make_block_transaction_key(block_num, id);
}

uint32_t transaction_history_plugin::get_last_irreversible_block_num() const {
   if (my && my->chain_plug) {
      return my->chain_plug->chain().last_irreversible_block_num();
   }
   return 0;
}

std::shared_ptr<rocksdb_manager> transaction_history_plugin::get_db_manager() const {
   if (my) {
      return my->db_;
   }
   return nullptr;
}

// read_only implementation
namespace transaction_history_apis {

read_only::get_transaction_result read_only::get_transaction(const get_transaction_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   EOS_ASSERT(params.id.size() >= 8 && params.id.size() <= 64 &&
              std::all_of(params.id.begin(), params.id.end(), [](unsigned char c) {
                 return std::isxdigit(c) != 0;
              }), chain::transaction_id_type_exception,
              "Invalid transaction ID prefix: ${id}", ("id", params.id));
   std::string normalized = boost::algorithm::to_lower_copy(params.id);
   get_transaction_result result;
   bool found = false;
   const bool filtering_active = !history->my->filter_on_star || !history->my->filter_out_.empty();

   if (normalized.size() == 64) {
      found = history->my->db_->get_object("trx:" + normalized, result);
   } else {
      std::unique_ptr<rocksdb::Iterator> iterator(history->my->db_->new_iterator());
      EOS_ASSERT(iterator, chain::plugin_exception, "Transaction history database is not open");
      const std::string prefix = "trx:" + normalized;
      iterator->Seek(prefix);
      if (iterator->Valid() && iterator->key().starts_with(prefix)) {
         const std::string stored_value = iterator->value().ToString();
         iterator->Next();
         EOS_ASSERT(!(iterator->Valid() && iterator->key().starts_with(prefix)),
                    chain::transaction_id_type_exception,
                    "Transaction ID prefix ${id} is ambiguous", ("id", params.id));
         try {
            fc::from_variant(fc::json::from_string(stored_value), result);
            found = true;
         } catch (...) {
            found = false;
         }
      }
      EOS_ASSERT(iterator->status().ok(), chain::plugin_exception,
                 "Failed to scan transaction history: ${error}",
                 ("error", iterator->status().ToString()));
   }

   auto& controller = history->my->chain_plug->chain();
   const auto abi_yield = eosio::chain::abi_serializer::create_yield_function(
      history->my->chain_plug->get_abi_serializer_max_time());

   if (!found && params.block_num_hint && !filtering_active) {
      auto block = controller.fetch_block_by_number(*params.block_num_hint);
      if (block) {
         for (const auto& receipt : block->transactions) {
            transaction_id_type id;
            if (std::holds_alternative<eosio::chain::packed_transaction>(receipt.trx)) {
               id = std::get<eosio::chain::packed_transaction>(receipt.trx).id();
            } else {
               id = std::get<transaction_id_type>(receipt.trx);
            }
            if (id.str().compare(0, normalized.size(), normalized) != 0) continue;

            EOS_ASSERT(!found, chain::transaction_id_type_exception,
                       "Transaction ID prefix ${id} is ambiguous in block ${block_num}",
                       ("id", params.id)("block_num", *params.block_num_hint));

            result.id = id;
            result.block_num = *params.block_num_hint;
            result.block_time = block->timestamp;
            fc::mutable_variant_object transaction_value("receipt", receipt);
            if (std::holds_alternative<eosio::chain::packed_transaction>(receipt.trx)) {
               transaction_value(
                  "trx", controller.to_variant_with_abi(
                     std::get<eosio::chain::packed_transaction>(receipt.trx).get_signed_transaction(),
                     abi_yield));
            }
            result.trx = std::move(transaction_value);
            found = true;
         }
      }
   }

   EOS_ASSERT(found, chain::tx_not_found,
              "Transaction ${id} not found in history${hint}",
              ("id", params.id)("hint", params.block_num_hint ? " or hinted block" : ""));

   // Rebuild the legacy receipt/transaction representation with ABI-decoded
   // action data when the stored record was not redacted by filtering.
   if (filtering_active) {
      fc::mutable_variant_object filtered_transaction("filtered", true);
      if (result.trx.is_object() && result.trx.get_object().contains("receipt")) {
         filtered_transaction("receipt", result.trx.get_object()["receipt"]);
      }
      result.trx = std::move(filtered_transaction);
   }
   const bool transaction_visible = !filtering_active && result.trx.is_object() &&
      result.trx.get_object().contains("trx");
   if (transaction_visible) {
      if (auto block = controller.fetch_block_by_number(result.block_num)) {
         for (const auto& receipt : block->transactions) {
            if (!std::holds_alternative<eosio::chain::packed_transaction>(receipt.trx)) continue;
            const auto& packed = std::get<eosio::chain::packed_transaction>(receipt.trx);
            if (packed.id() != result.id) continue;
            fc::mutable_variant_object transaction_value("receipt", receipt);
            transaction_value("trx", controller.to_variant_with_abi(
               packed.get_signed_transaction(), abi_yield));
            result.trx = std::move(transaction_value);
            break;
         }
      }

   }
   std::vector<size_t> action_ref_indexes;
   std::vector<std::string> action_ref_keys;
   for (size_t index = 0; index < result.traces.size(); ++index) {
      const auto& action_value = result.traces[index];
      if (action_value.is_object() && action_value.get_object().contains("action_ref")) {
         action_ref_indexes.push_back(index);
         action_ref_keys.push_back(action_value.get_object()["action_ref"].as_string());
      }
   }
   if (!action_ref_keys.empty()) {
      // All normalized actions for one transaction are already covered by the
      // transaction trace-size limit, so a larger batch remains memory-bounded.
      constexpr size_t max_multi_get_keys = 64;
      for (size_t batch_begin = 0; batch_begin < action_ref_keys.size();
           batch_begin += max_multi_get_keys) {
         const size_t batch_end = std::min(action_ref_keys.size(), batch_begin + max_multi_get_keys);
         const std::vector<std::string> batch_keys(
            action_ref_keys.begin() + batch_begin, action_ref_keys.begin() + batch_end);
         std::vector<std::string> normalized_values;
         const auto statuses = history->my->db_->multi_get(batch_keys, normalized_values);
         EOS_ASSERT(statuses.size() == batch_keys.size() &&
                    normalized_values.size() == batch_keys.size(),
                    chain::plugin_exception, "RocksDB returned an incomplete normalized-action batch");
         for (size_t batch_index = 0; batch_index < batch_keys.size(); ++batch_index) {
            const size_t index = batch_begin + batch_index;
            EOS_ASSERT(statuses[batch_index].ok(), chain::plugin_exception,
                    "Normalized action ${ref} referenced by transaction ${id} is missing: ${error}",
                    ("ref", action_ref_keys[index])("id", result.id)
                    ("error", statuses[batch_index].ToString()));
            const auto normalized_action =
               fc::json::from_string(normalized_values[batch_index]).get_object();
            EOS_ASSERT(normalized_action.contains("action_trace"), chain::plugin_exception,
                       "Normalized action ${ref} has no action_trace", ("ref", action_ref_keys[index]));
            result.traces[action_ref_indexes[index]] = normalized_action["action_trace"];
         }
      }
   }

   for (auto& action_value : result.traces) {
      try {
         eosio::chain::action_trace action;
         fc::from_variant(action_value, action);
         action_value = controller.to_variant_with_abi(action, abi_yield);
      } catch (...) {
         // Preserve the stored representation if an older record cannot be
         // converted with the current ABI.
      }
   }

   // Update current irreversible block
   result.last_irreversible_block = history->get_last_irreversible_block_num();

   return result;
}

read_only::get_actions_result read_only::get_actions(const get_actions_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_actions_result result;
   result.last_irreversible_block = history->my->chain_plug->chain().last_irreversible_block_num();
   result.more = false;
   result.time_limit_exceeded_error = false;

   const int32_t offset = params.offset.value_or(-20);
   EOS_ASSERT(offset >= -static_cast<int32_t>(transaction_history_plugin_impl::MAX_API_RESULTS) &&
              offset <= static_cast<int32_t>(transaction_history_plugin_impl::MAX_API_RESULTS),
              chain::plugin_exception, "offset must be between -${max} and ${max}",
              ("max", transaction_history_plugin_impl::MAX_API_RESULTS));
   if (offset == 0) {
      return result;
   }

   const std::string prefix = "acc:" + params.account_name.to_string() + ":";
   const auto deadline = fc::time_point::now() +
      fc::microseconds(transaction_history_plugin_impl::API_SCAN_TIME_US);
   const size_t requested = static_cast<size_t>(std::abs(static_cast<int64_t>(offset)));
   const size_t scan_limit = transaction_history_plugin_impl::MAX_API_RESULTS * 4;
   const size_t candidate_limit = std::min(scan_limit, requested * 4);
   const std::string upper_prefix = prefix + "\xff";
   const rocksdb::Slice lower_bound(prefix);
   const rocksdb::Slice upper_bound(upper_prefix);
   rocksdb::ReadOptions read_options;
   read_options.iterate_lower_bound = &lower_bound;
   read_options.iterate_upper_bound = &upper_bound;
   std::unique_ptr<rocksdb::Iterator> iterator(history->my->db_->new_iterator(read_options));
   EOS_ASSERT(iterator, chain::plugin_exception, "Transaction history database is not open");

   auto has_prefix = [&prefix](const rocksdb::Iterator& it) {
      return it.Valid() && it.key().starts_with(prefix);
   };
   const uint64_t max_response_bytes = history->my->max_api_response_bytes_;
   auto& controller = history->my->chain_plug->chain();
   const auto abi_yield = eosio::chain::abi_serializer::create_yield_function(
      history->my->chain_plug->get_abi_serializer_max_time());
   std::vector<std::map<std::string, fc::variant>> candidates;
   candidates.reserve(candidate_limit);
   auto collect_current = [&candidates, max_response_bytes](const rocksdb::Iterator& it) {
      const auto value = it.value();
      EOS_ASSERT(value.size() <= max_response_bytes, chain::plugin_exception,
                 "A single history action record exceeds the configured API response byte limit");
      try {
         candidates.emplace_back(fc::json::from_string(value.ToString())
            .as<std::map<std::string, fc::variant>>());
      } catch (...) {
         return false;
      }
      return true;
   };

   size_t scanned = 0;
   if (offset < 0) {
      if (!params.pos || *params.pos < 0) {
         iterator->Seek(upper_prefix);
         if (iterator->Valid()) {
            iterator->Prev();
         } else {
            iterator->SeekToLast();
         }
      } else {
         iterator->Seek(history->make_account_action_key(
            params.account_name, static_cast<uint64_t>(*params.pos)));
         if (!iterator->Valid()) {
            iterator->SeekToLast();
         } else if (iterator->key().ToString() != history->make_account_action_key(
                       params.account_name, static_cast<uint64_t>(*params.pos))) {
            iterator->Prev();
         }
      }

      while (has_prefix(*iterator) && candidates.size() < candidate_limit && scanned < scan_limit) {
         if (fc::time_point::now() >= deadline) {
            result.time_limit_exceeded_error = true;
            break;
         }
         collect_current(*iterator);
         ++scanned;
         iterator->Prev();
      }
   } else {
      if (params.pos && *params.pos > 0) {
         iterator->Seek(history->make_account_action_key(
            params.account_name, static_cast<uint64_t>(*params.pos)));
      } else {
         iterator->Seek(prefix);
      }

      while (has_prefix(*iterator) && candidates.size() < candidate_limit && scanned < scan_limit) {
         if (fc::time_point::now() >= deadline) {
            result.time_limit_exceeded_error = true;
            break;
         }
         collect_current(*iterator);
         ++scanned;
         iterator->Next();
      }
   }

   size_t response_bytes = 0;
   bool byte_limit_reached = false;
   size_t candidate_index = 0;
   // Account pages combine unrelated transactions, so keep a smaller batch to
   // bound the memory of unusually large action traces.
   constexpr size_t max_multi_get_keys = 8;
   for (size_t batch_begin = 0;
        batch_begin < candidates.size() && result.actions.size() < requested && !byte_limit_reached;
        batch_begin += max_multi_get_keys) {
      const size_t batch_end = std::min(candidates.size(), batch_begin + max_multi_get_keys);
      std::vector<size_t> reference_indexes;
      std::vector<std::string> reference_keys;
      for (size_t index = batch_begin; index < batch_end; ++index) {
         if (!candidates[index].count("action_trace")) {
            if (auto ref = candidates[index].find("action_ref"); ref != candidates[index].end()) {
               try {
                  reference_indexes.push_back(index);
                  reference_keys.push_back(ref->second.as_string());
               } catch (...) {
                  candidates[index].erase("action_ref");
               }
            }
         }
      }

      if (!reference_keys.empty()) {
         std::vector<std::string> normalized_values;
         const auto statuses = history->my->db_->multi_get(reference_keys, normalized_values);
         EOS_ASSERT(statuses.size() == reference_keys.size() &&
                    normalized_values.size() == reference_keys.size(),
                    chain::plugin_exception, "RocksDB returned an incomplete normalized-action batch");
         for (size_t index = 0; index < reference_keys.size(); ++index) {
            auto& action = candidates[reference_indexes[index]];
            if (statuses[index].ok()) {
               try {
                  auto normalized_action = fc::json::from_string(normalized_values[index])
                     .as<std::map<std::string, fc::variant>>();
                  if (auto trace = normalized_action.find("action_trace");
                      trace != normalized_action.end()) {
                     action["action_trace"] = std::move(trace->second);
                  }
               } catch (...) {
                  // The malformed row is omitted while valid rows remain queryable.
                  history->my->history_reference_misses_++;
               }
            } else if (statuses[index].IsNotFound()) {
               history->my->history_reference_misses_++;
            } else {
               history->my->history_read_errors_++;
               EOS_ASSERT(false, chain::plugin_exception,
                          "Failed to read normalized action ${ref}: ${error}",
                          ("ref", reference_keys[index])
                          ("error", statuses[index].ToString()));
            }
            action.erase("action_ref");
         }
      }

      for (size_t index = batch_begin;
           index < batch_end && result.actions.size() < requested; ++index) {
         candidate_index = index + 1;
         auto& action = candidates[index];
         if (!action.count("action_trace")) continue;
         if (!action.count("global_action_seq") && action.count("global_sequence")) {
            action["global_action_seq"] = action["global_sequence"];
         }
         if (auto trace_it = action.find("action_trace"); trace_it != action.end()) {
            try {
               eosio::chain::action_trace typed_trace;
               fc::from_variant(trace_it->second, typed_trace);
               trace_it->second = controller.to_variant_with_abi(typed_trace, abi_yield);
            } catch (...) {
               // Keep records written under an older schema queryable.
            }
         }
         const size_t serialized_size = object_to_json(action).size();
         EOS_ASSERT(serialized_size <= max_response_bytes, chain::plugin_exception,
                    "A single decoded history action exceeds the configured API response byte limit");
         if (response_bytes > max_response_bytes - serialized_size) {
            byte_limit_reached = true;
            break;
         }
         result.actions.emplace_back(std::move(action));
         response_bytes += serialized_size;
      }
   }

   result.more = candidate_index < candidates.size() || has_prefix(*iterator) ||
                 scanned == scan_limit || byte_limit_reached ||
                 result.time_limit_exceeded_error.value_or(false);
   if (offset < 0) {
      std::reverse(result.actions.begin(), result.actions.end());
   }

   EOS_ASSERT(iterator->status().ok(), chain::plugin_exception,
              "Failed to scan account history: ${error}",
              ("error", iterator->status().ToString()));

   EOS_ASSERT(scanned <= scan_limit,
              chain::plugin_exception, "Too many invalid action index entries");

   return result;
}

read_only::get_transaction_count_result read_only::get_transaction_count(const get_transaction_count_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_transaction_count_result result;
   result.count = 0;
   result.start_block = params.start_block.value_or(1);
   result.end_block = params.end_block.value_or(history->my->chain_plug->chain().head().block_num());

   EOS_ASSERT(result.start_block <= result.end_block, chain::plugin_exception,
              "start_block must not be greater than end_block");

   const auto deadline = fc::time_point::now() +
      fc::microseconds(transaction_history_plugin_impl::API_SCAN_TIME_US);
   const std::string first_key = "blk:" + fixed_width_number(result.start_block, 10) + ":";
   const std::string end_prefix = result.end_block == std::numeric_limits<uint32_t>::max()
      ? "blz:"
      : "blk:" + fixed_width_number(static_cast<uint64_t>(result.end_block) + 1, 10) + ":";
   const rocksdb::Slice lower_bound(first_key);
   const rocksdb::Slice upper_bound(end_prefix);
   rocksdb::ReadOptions read_options;
   read_options.iterate_lower_bound = &lower_bound;
   read_options.iterate_upper_bound = &upper_bound;
   std::unique_ptr<rocksdb::Iterator> iterator(history->my->db_->new_iterator(read_options));
   EOS_ASSERT(iterator, chain::plugin_exception, "Transaction history database is not open");

   uint64_t scanned = 0;
   for (iterator->Seek(first_key); iterator->Valid(); iterator->Next()) {
      const std::string key = iterator->key().ToString();
      if (key.compare(0, 4, "blk:") != 0 || key >= end_prefix) {
         break;
      }
      ++result.count;
      ++scanned;
      EOS_ASSERT(scanned <= transaction_history_plugin_impl::MAX_COUNT_SCAN_KEYS,
                 chain::plugin_exception, "Transaction count exceeds the ${max} key query limit",
                 ("max", transaction_history_plugin_impl::MAX_COUNT_SCAN_KEYS));
      EOS_ASSERT(fc::time_point::now() < deadline, chain::deadline_exception,
                 "get_transaction_count exceeded the query time limit");
   }

   EOS_ASSERT(iterator->status().ok(), chain::plugin_exception,
              "Failed to scan block transaction index: ${error}",
              ("error", iterator->status().ToString()));

   return result;
}

read_only::get_key_accounts_result read_only::get_key_accounts(const get_key_accounts_params& params) const {
   get_key_accounts_result result;
   std::set<eosio::chain::name> accounts;
   const auto deadline = fc::time_point::now() +
      fc::microseconds(transaction_history_plugin_impl::API_SCAN_TIME_US);

   try {
      auto chain_api = history->my->chain_plug->get_read_only_api(
         fc::microseconds(transaction_history_plugin_impl::API_SCAN_TIME_US));
      eosio::chain_apis::account_query_db::get_accounts_by_authorizers_params query;
      query.keys.push_back(params.public_key);
      const auto matches = chain_api.get_accounts_by_authorizers(query, deadline);
      for (const auto& match : matches.accounts) {
         accounts.insert(match.account_name);
         EOS_ASSERT(accounts.size() <= transaction_history_plugin_impl::MAX_API_RESULTS,
                    chain::plugin_exception, "get_key_accounts exceeds the result limit");
      }
   } catch (const eosio::chain::plugin_config_exception&) {
      const auto& permissions = history->my->chain_plug->chain().db()
         .get_index<eosio::chain::permission_index, ::by_id>();
      size_t checked = 0;
      for (const auto& permission : permissions) {
         for (const auto& key : permission.auth.keys) {
            if (key.key.to_public_key() == params.public_key) {
               accounts.insert(permission.owner);
               EOS_ASSERT(accounts.size() <= transaction_history_plugin_impl::MAX_API_RESULTS,
                          chain::plugin_exception, "get_key_accounts exceeds the result limit");
               break;
            }
         }
         if ((++checked & 0xff) == 0) {
            EOS_ASSERT(fc::time_point::now() < deadline, chain::deadline_exception,
                       "get_key_accounts exceeded the query time limit; enable account queries for indexed lookup");
         }
      }
   }

   EOS_ASSERT(accounts.size() <= transaction_history_plugin_impl::MAX_API_RESULTS,
              chain::plugin_exception, "get_key_accounts exceeds the ${max} result limit",
              ("max", transaction_history_plugin_impl::MAX_API_RESULTS));
   result.account_names.assign(accounts.begin(), accounts.end());

   return result;
}

read_only::get_controlled_accounts_result read_only::get_controlled_accounts(const get_controlled_accounts_params& params) const {
   get_controlled_accounts_result result;
   std::set<eosio::chain::name> accounts;
   const auto deadline = fc::time_point::now() +
      fc::microseconds(transaction_history_plugin_impl::API_SCAN_TIME_US);

   try {
      auto chain_api = history->my->chain_plug->get_read_only_api(
         fc::microseconds(transaction_history_plugin_impl::API_SCAN_TIME_US));
      eosio::chain_apis::account_query_db::get_accounts_by_authorizers_params query;
      eosio::chain_apis::account_query_db::get_accounts_by_authorizers_params::permission_level level;
      level.actor = params.controlling_account;
      level.permission = {};
      query.accounts.push_back(level);
      const auto matches = chain_api.get_accounts_by_authorizers(query, deadline);
      for (const auto& match : matches.accounts) {
         accounts.insert(match.account_name);
         EOS_ASSERT(accounts.size() <= transaction_history_plugin_impl::MAX_API_RESULTS,
                    chain::plugin_exception, "get_controlled_accounts exceeds the result limit");
      }
   } catch (const eosio::chain::plugin_config_exception&) {
      const auto& permissions = history->my->chain_plug->chain().db()
         .get_index<eosio::chain::permission_index, ::by_id>();
      size_t checked = 0;
      for (const auto& permission : permissions) {
         for (const auto& account : permission.auth.accounts) {
            if (account.permission.actor == params.controlling_account) {
               accounts.insert(permission.owner);
               EOS_ASSERT(accounts.size() <= transaction_history_plugin_impl::MAX_API_RESULTS,
                          chain::plugin_exception, "get_controlled_accounts exceeds the result limit");
               break;
            }
         }
         if ((++checked & 0xff) == 0) {
            EOS_ASSERT(fc::time_point::now() < deadline, chain::deadline_exception,
                       "get_controlled_accounts exceeded the query time limit; enable account queries for indexed lookup");
         }
      }
   }

   EOS_ASSERT(accounts.size() <= transaction_history_plugin_impl::MAX_API_RESULTS,
              chain::plugin_exception, "get_controlled_accounts exceeds the ${max} result limit",
              ("max", transaction_history_plugin_impl::MAX_API_RESULTS));
   result.controlled_accounts.assign(accounts.begin(), accounts.end());

   return result;
}

read_only::get_database_stats_result read_only::get_database_stats(const get_database_stats_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_database_stats_result result;

   if (history && history->my->db_) {
      std::string stats_json = history->my->db_->get_database_stats();
      try {
         result.stats = fc::json::from_string(stats_json);
         result.success = true;
      } catch (const std::exception& e) {
         result.success = false;
         result.error = e.what();
      }
   } else {
      result.success = false;
      result.error = "Database manager not available";
   }

   return result;
}

read_only::get_performance_metrics_result read_only::get_performance_metrics(const get_performance_metrics_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_performance_metrics_result result;

   if (history && history->get_db_manager()) {
      std::string metrics_json = history->get_db_manager()->get_performance_metrics();
      try {
         fc::variant parsed = fc::json::from_string(metrics_json);
         fc::mutable_variant_object metrics(std::move(parsed.get_object()));
         metrics["history_healthy"] = history->my->history_healthy_.load();
         metrics["history_gap_block"] = history->my->history_gap_block_.load();
         metrics["history_queue_tasks"] = history->my->worker_->pending_tasks();
         metrics["history_queue_bytes"] = history->my->worker_->pending_bytes();
         metrics["history_queue_max_tasks"] = history->my->worker_->pending_task_limit();
         metrics["history_queue_max_bytes"] = history->my->worker_->pending_byte_limit();
         metrics["history_maintenance_queue_tasks"] =
            history->my->maintenance_worker_->pending_tasks();
         metrics["history_health_task_pending"] = history->my->health_task_pending_.load();
         metrics["history_maintenance_task_pending"] = history->my->maintenance_task_pending_.load();
         metrics["history_analysis_task_pending"] = history->my->analysis_task_pending_.load();
         metrics["history_maintenance_tasks_coalesced"] =
            history->my->maintenance_tasks_coalesced_.load();
         metrics["history_maintenance_tasks_skipped"] =
            history->my->maintenance_tasks_skipped_.load();
         if (history->my->rollback_mgr_) {
            metrics["history_checkpoint_count"] = history->my->rollback_mgr_->rollback_point_count();
            metrics["history_latest_checkpoint_block"] =
               history->my->rollback_mgr_->get_latest_rollback_point().value_or(0);
            metrics["history_rollback_point_count"] = history->my->rollback_mgr_->rollback_point_count();
            metrics["history_latest_rollback_block"] =
               history->my->rollback_mgr_->get_latest_rollback_point().value_or(0);
         }
         metrics["transactions_processed"] = history->my->transactions_processed_.load();
         metrics["transactions_failed"] = history->my->transactions_failed_.load();
         const uint64_t batches = history->my->accepted_block_batches_.load();
         const uint64_t checkpoints = history->my->checkpoints_created_.load();
         metrics["history_accepted_block_batches"] = batches;
         metrics["history_accepted_block_batch_bytes"] =
            history->my->accepted_block_batch_bytes_.load();
         metrics["history_accepted_block_batch_time_us"] =
            history->my->accepted_block_batch_time_us_.load();
         metrics["history_accepted_block_batch_max_us"] =
            history->my->accepted_block_batch_max_us_.load();
         metrics["history_accepted_block_batch_average_us"] = batches == 0 ? 0 :
            history->my->accepted_block_batch_time_us_.load() / batches;
         metrics["history_checkpoints_created"] = checkpoints;
         metrics["history_undo_records_created"] = checkpoints;
         metrics["history_checkpoint_time_us"] = history->my->checkpoint_time_us_.load();
         metrics["history_rollback_point_time_us"] = history->my->checkpoint_time_us_.load();
         metrics["history_checkpoint_max_us"] = history->my->checkpoint_max_us_.load();
         metrics["history_rollback_point_max_us"] = history->my->checkpoint_max_us_.load();
         metrics["history_checkpoint_cleanup_time_us"] =
            history->my->checkpoint_cleanup_time_us_.load();
         metrics["history_checkpoint_cleanup_max_us"] =
            history->my->checkpoint_cleanup_max_us_.load();
         metrics["history_reference_misses"] = history->my->history_reference_misses_.load();
         metrics["history_read_errors"] = history->my->history_read_errors_.load();
         metrics["history_checkpoint_average_us"] = checkpoints == 0 ? 0 :
            history->my->checkpoint_time_us_.load() / checkpoints;
         result.metrics = std::move(metrics);
         result.success = true;
      } catch (const std::exception& e) {
         result.success = false;
         result.error = e.what();
      }
   } else {
      result.success = false;
      result.error = "Database manager not available";
   }

   return result;
}

read_only::get_optimization_suggestions_result read_only::get_optimization_suggestions(const get_optimization_suggestions_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_optimization_suggestions_result result;

   if (history && history->get_db_manager()) {
      std::string suggestions_json = history->get_db_manager()->get_optimization_suggestions();
      try {
         result.suggestions = fc::json::from_string(suggestions_json);
         result.success = true;
      } catch (const std::exception& e) {
         result.success = false;
         result.error = e.what();
      }
   } else {
      result.success = false;
      result.error = "Database manager not available";
   }

   return result;
}

read_only::get_cache_analysis_result read_only::get_cache_analysis(const get_cache_analysis_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_cache_analysis_result result;

   if (history && history->get_db_manager()) {
      std::string analysis_json = history->get_db_manager()->get_cache_analysis();
      try {
         result.analysis = fc::json::from_string(analysis_json);
         result.success = true;
      } catch (const std::exception& e) {
         result.success = false;
         result.error = e.what();
      }
   } else {
      result.success = false;
      result.error = "Database manager not available";
   }

   return result;
}

read_only::get_maintenance_needs_result read_only::get_maintenance_needs(const get_maintenance_needs_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   get_maintenance_needs_result result;

   if (history && history->get_db_manager()) {
      std::string needs_json = history->get_db_manager()->check_maintenance_needs();
      try {
         result.maintenance_needs = fc::json::from_string(needs_json);
         result.success = true;
      } catch (const std::exception& e) {
         result.success = false;
         result.error = e.what();
      }
   } else {
      result.success = false;
      result.error = "Database manager not available";
   }

   return result;
}

} // namespace transaction_history_apis

} // namespace eosio
