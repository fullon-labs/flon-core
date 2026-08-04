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

#include <boost/algorithm/string.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/program_options.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
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

class transaction_history_plugin_impl {
   friend class transaction_history_apis::read_only;  // Allow read_only to access private members

public:
   eosio::chain_plugin* chain_plug = nullptr;
   std::shared_ptr<rocksdb_manager> db_;
   std::unique_ptr<async_worker> worker_;
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
   static constexpr uint32_t MAX_API_RESULTS = 1000;
   static constexpr uint64_t MAX_COUNT_SCAN_KEYS = 1000000;
   static constexpr int64_t API_SCAN_TIME_US = 20000;

   // Statistics for monitoring
   std::atomic<uint64_t> transactions_processed_{0};
   std::atomic<uint64_t> transactions_failed_{0};
   std::atomic<uint64_t> total_processing_time_us_{0};
   std::atomic<bool> history_healthy_{true};
   std::atomic<uint32_t> history_gap_block_{0};
   fc::time_point startup_time_;

   // For monitoring and warnings
   std::atomic<uint32_t> last_warning_block_{0};
   std::atomic<uint32_t> last_health_check_block_{0};
   std::atomic<uint32_t> last_maintenance_block_{0};
   std::atomic<uint32_t> last_analysis_block_{0};
   std::mutex last_updated_block_mutex_;
   uint32_t last_updated_block_ = 0;
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
   void check_data_size_warnings(uint32_t current_block_num, uint32_t lib_block_num);
   bool filter_action(const eosio::chain::action_trace& action_trace) const;
   uint64_t load_account_sequence(const eosio::chain::name& account) const;

   std::string make_transaction_key(const eosio::chain::transaction_id_type& id) const;
   std::string make_account_action_key(const eosio::chain::name& account, uint64_t seq) const;
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
       "Enable automatic database compaction for performance optimization")
      ("transaction-history-validate-on-startup", bpo::value<bool>()->default_value(false),
       "Validate and repair database integrity on startup")
      ("transaction-history-maintenance-interval", bpo::value<uint32_t>()->default_value(100000),
       "Interval in blocks for automatic database maintenance (compaction, validation)")
      ("transaction-history-detailed-monitoring", bpo::value<bool>()->default_value(false),
       "Enable detailed performance monitoring and statistics logging (may impact performance)")
      ("transaction-history-auto-tuning", bpo::value<bool>()->default_value(false),
       "Enable automatic performance tuning recommendations and analysis")
      ("transaction-history-analysis-interval", bpo::value<uint32_t>()->default_value(500000),
       "Interval in blocks for comprehensive database analysis (distribution, optimization suggestions)")
      ("transaction-history-max-retained-blocks", bpo::value<uint32_t>()->default_value(1000),
       "Maximum number of rollback checkpoints to retain")
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
       "Minimum filesystem free bytes preserved while retaining rollback checkpoints")
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
      EOS_ASSERT(my->max_retained_blocks_ > 0 && my->max_trace_size_ > 0 &&
                 my->max_actions_per_tx_ > 0 && my->max_account_indexes_per_tx_ > 0 &&
                 my->max_write_batch_bytes_ > 0 && my->max_api_response_bytes_ > 0,
                 eosio::chain::plugin_exception, "transaction history limits must be greater than zero");
      EOS_ASSERT(my->max_trace_size_ <= async_worker::max_pending_bytes &&
                 my->max_write_batch_bytes_ <= async_worker::max_pending_bytes &&
                 my->max_api_response_bytes_ <= async_worker::max_pending_bytes,
                 eosio::chain::plugin_exception,
                 "transaction history trace, write batch, and API response limits cannot exceed the memory safety cap of ${max} bytes",
                 ("max", async_worker::max_pending_bytes));

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

      ilog("Transaction history monitoring: max trace size: ${trace_size} bytes, max retained blocks: ${blocks}, max actions per tx: ${actions}, compression: ${compression}",
           ("trace_size", my->max_trace_size_)
           ("blocks", my->max_retained_blocks_)
           ("actions", my->max_actions_per_tx_)
           ("compression", my->compression_enabled_ ? "enabled" : "disabled"));

      // Initialize components
      my->db_ = std::make_shared<rocksdb_manager>();
      my->worker_ = std::make_unique<async_worker>();

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

   // Detect startup mode and handle database state accordingly
   bool is_snapshot_load = false;
   bool is_replay = false;

   uint32_t chain_head_block = chain.head().block_num();
   uint32_t earliest_available = chain.earliest_available_block_num();
   uint32_t lib_num = chain.last_irreversible_block_num();

   // More reliable detection logic
   // Check if we're loading from snapshot (significant gap between earliest and 1)
   if (earliest_available > 1000 && chain_head_block >= earliest_available) {
      is_snapshot_load = true;
      ilog("Detected snapshot load: earliest available block is ${earliest}, head is ${head}",
           ("earliest", earliest_available)("head", chain_head_block));
   }

   // Check for replay scenarios:
   // 1. Head is significantly behind LIB (shouldn't happen in normal operation)
   // 2. Large gap between earliest available and head when not loading from snapshot
   if (!is_snapshot_load) {
      if (chain_head_block < lib_num && lib_num - chain_head_block > 100) {
         is_replay = true;
         ilog("Detected replay: head block ${head} is behind LIB ${lib}",
              ("head", chain_head_block)("lib", lib_num));
      } else if (earliest_available > 1 && chain_head_block >= earliest_available &&
                 chain_head_block - earliest_available < 1000) {
         is_replay = true;
         ilog("Detected potential replay: small range between earliest (${earliest}) and head (${head})",
              ("earliest", earliest_available)("head", chain_head_block));
      }
   }

   // Get current database state for logging
   uint32_t db_last_block = my->db_->get_last_block_number();

   ilog("Transaction history startup analysis - Chain head: ${head}, LIB: ${lib}, "
        "Earliest: ${earliest}, DB last: ${db}, Snapshot: ${snapshot}, Replay: ${replay}",
        ("head", chain_head_block)("lib", lib_num)("earliest", earliest_available)
        ("db", db_last_block)("snapshot", is_snapshot_load)("replay", is_replay));

   // Initialize database state based on startup conditions
   if (my->auto_repair_enabled_) {
      if (!my->db_->check_and_repair_database_state(chain_head_block, is_snapshot_load, is_replay)) {
         elog("Failed to initialize transaction history database state");
         my->history_healthy_ = false;
         my->history_gap_block_ = chain_head_block;
      }
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
         if (stored_num == chain_head_block) {
            chain_identity_matches = stored_accepted_id == chain_head_id;
         } else if (stored_num <= chain_head_block && stored_num >= earliest_available) {
            const auto active_id = chain.chain_block_id_for_num(stored_num);
            chain_identity_matches = active_id && active_id->str() == stored_accepted_id;
         } else if (stored_num < earliest_available && !is_snapshot_load && !is_replay) {
            // A normal pruned-node restart may be unable to verify an old but
            // otherwise valid baseline. Preserve it and let the live gap logic
            // advance the identity on the next block.
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
         wlog("Transaction history belongs to an unverified chain branch; clearing history and rollback checkpoints");
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
   } else if (!has_chain_identity) {
      EOS_ASSERT(my->db_->batch_write({
           {"_internal_last_accepted_block_num", std::to_string(chain_head_block)},
           {"_internal_last_accepted_block_id", chain_head_id}
        }, {}), chain::plugin_exception,
        "Failed to initialize transaction history chain baseline");
   }

   // Load rollback metadata only after startup repair has finalized the active
   // branch and removed any incompatible external checkpoints.
   my->rollback_mgr_ = std::make_unique<rollback_manager>(my->db_);

   // A newly established or repaired baseline must itself be restorable. This
   // closes the window in which the first live block can fork before any
   // accepted-block callback has created a checkpoint for its parent.
   if (my->history_healthy_.load() &&
       !my->rollback_mgr_->has_rollback_point(chain_head_block) &&
       !my->rollback_mgr_->create_rollback_point(chain_head_block)) {
      my->record_history_gap(chain_head_block, "failed to create startup checkpoint");
   }

   // Log final database statistics
   std::string db_stats = my->db_->get_database_stats();
   dlog("Transaction history database statistics: ${stats}", ("stats", db_stats));

   // Initialize the monotonic database height after any startup repair or cleanup.
   my->last_updated_block_ = my->db_->get_last_block_number();

   // A bounded, ordered writer is required so accepted-block checkpoints are
   // taken only after all transaction writes for that block have completed.
   my->worker_->start();

   // block_start is emitted before transactions for the next block. Queueing
   // the parent check here ensures a fork rollback runs before any replacement
   // branch transaction reaches RocksDB.
   my->block_start_connection_ = chain.block_start().connect(
      [&](uint32_t) {
         const uint32_t parent_block_num = chain.head().block_num();
         const std::string parent_block_id = chain.head().id().str();
         if (!my->worker_->try_enqueue_task([impl = my.get(), parent_block_num, parent_block_id]() {
                impl->ensure_chain_parent(parent_block_num, parent_block_id);
             })) {
            my->record_history_gap(parent_block_num, "history queue full before fork-parent check");
         }
      });

   my->applied_transaction_connection_ = chain.applied_transaction().connect(
      [&](std::tuple<const eosio::chain::transaction_trace_ptr&, const eosio::chain::packed_transaction_ptr&> t) {
         my->applied_transaction(std::get<0>(t), std::get<1>(t));
      });

   my->accepted_block_connection_ = chain.accepted_block().connect(
      [&](const eosio::chain::block_signal_params& event) {
         const uint32_t block_num = std::get<0>(event)->block_num();
         const std::string block_id = std::get<1>(event).str();
         const uint32_t irreversible_block_num = chain.last_irreversible_block_num();
         if (!my->worker_->try_enqueue_task([impl = my.get(), block_num, block_id, irreversible_block_num]() {
            if (!impl->history_healthy_.load()) {
               return;
            }

            if (!impl->batch_write_with_retry({
                   {"_internal_last_accepted_block_num", std::to_string(block_num)},
                   {"_internal_last_accepted_block_id", block_id}
                }, {})) {
               impl->record_history_gap(block_num, "failed to persist accepted-block identity");
               return;
            }

            // Reclaim checkpoints below LIB before creating the next one, but
            // never sacrifice a checkpoint in the reversible fork window for
            // a configured count or free-space target.
            impl->rollback_mgr_->cleanup_irreversible_rollback_points(irreversible_block_num);
            impl->rollback_mgr_->cleanup_old_rollback_points(
               impl->max_retained_blocks_, impl->min_checkpoint_free_bytes_,
               irreversible_block_num);
            if (!impl->rollback_mgr_->create_rollback_point(block_num)) {
               impl->record_history_gap(block_num, "failed to create accepted-block checkpoint");
               return;
            }
            impl->rollback_mgr_->cleanup_old_rollback_points(
               impl->max_retained_blocks_, impl->min_checkpoint_free_bytes_,
               irreversible_block_num);
             })) {
            my->record_history_gap(block_num, "history queue full before accepted-block checkpoint");
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

   if (!rollback_mgr_->rollback_to_block(parent_block_num)) {
      record_history_gap(parent_block_num, "no usable checkpoint for fork parent");
      return;
   }

   std::string restored_id;
   if (!db_->get("_internal_last_accepted_block_id", restored_id) ||
       restored_id != parent_block_id) {
      record_history_gap(parent_block_num, "fork checkpoint belongs to a different branch");
      return;
   }

   std::lock_guard<std::mutex> lock(last_updated_block_mutex_);
   last_updated_block_ = db_->get_last_block_number();
}

void transaction_history_plugin_impl::applied_transaction(
   const eosio::chain::transaction_trace_ptr& trace,
   const eosio::chain::packed_transaction_ptr& packed) {
   if (!trace || !trace->receipt) return;

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
      retained_trace_bytes = fc::raw::pack_size(*trace);
      if (packed) {
         const size_t packed_bytes = packed->get_estimated_size();
         if (packed_bytes > std::numeric_limits<size_t>::max() - retained_trace_bytes) {
            throw std::overflow_error("retained transaction size overflow");
         }
         retained_trace_bytes += packed_bytes;
      }
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

   // Process transaction asynchronously to avoid blocking main chain
   if (!worker_->try_enqueue_task_with_size(retained_trace_bytes,
      [this, trace, packed, block_num, block_time, last_irreversible_block]() {
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

         // Check for data size warnings periodically
         if (result.block_num > last_warning_block_ + warning_interval_) {
            last_warning_block_ = result.block_num;
            check_data_size_warnings(result.block_num, result.last_irreversible_block);
         }

         // Periodic database health check
         if (result.block_num > last_health_check_block_ + health_check_interval_) {
            last_health_check_block_ = result.block_num;

            // Enhanced health monitoring
            if (!worker_->try_enqueue_task([this, block_num = result.block_num]() {
               // Basic health check
               bool health_ok = db_->health_check();

               // Get detailed statistics
               std::string stats = db_->get_database_stats();
               std::string performance = db_->get_performance_metrics();
               std::string size_breakdown = db_->get_size_breakdown();

               if (health_ok) {
                  ilog("Transaction history database health check PASSED at block ${block}", ("block", block_num));
                  if (detailed_monitoring_enabled_) {
                     dlog("Database statistics: ${stats}", ("stats", stats));
                     dlog("Performance metrics: ${performance}", ("performance", performance));
                     dlog("Size breakdown: ${size}", ("size", size_breakdown));
                  }
               } else {
                  wlog("Transaction history database health check FAILED at block ${block}", ("block", block_num));
                  if (detailed_monitoring_enabled_) {
                     wlog("Database statistics: ${stats}", ("stats", stats));
                     wlog("Performance metrics: ${performance}", ("performance", performance));
                  }

                  // Attempt automatic repair if health check fails
                  if (auto_repair_enabled_) {
                     ilog("Attempting automatic database repair due to health check failure");
                     if (db_->validate_and_repair_database()) {
                        ilog("Automatic database repair completed successfully");
                     } else {
                        elog("Automatic database repair failed - manual intervention may be required");
                     }
                  }
               }
            })) {
               wlog("Skipped transaction history health check at block ${block}: worker queue is full",
                    ("block", result.block_num));
            }
         }

         // Periodic database maintenance
         if (maintenance_interval_ > 0 && result.block_num > last_maintenance_block_ + maintenance_interval_) {
            last_maintenance_block_ = result.block_num;

            // Run maintenance in background to avoid blocking transaction processing
            if (!worker_->try_enqueue_task([this, block_num = result.block_num]() {
               ilog("Starting scheduled database maintenance at block ${block}", ("block", block_num));

               bool needs_compaction = false;

               // Intelligent compaction decision
               if (auto_compact_enabled_) {
                  // Check if compaction is needed based on database statistics
                  if (detailed_monitoring_enabled_) {
                     std::string performance_metrics = db_->get_performance_metrics();
                     try {
                        fc::variant metrics_json = fc::json::from_string(performance_metrics);
                        auto metrics_obj = metrics_json.get_object();

                        // Trigger compaction if L0 files are too many or background compaction is pending
                        if (metrics_obj.find("compaction_pending") != metrics_obj.end() &&
                            metrics_obj["compaction_pending"].as_bool()) {
                           needs_compaction = true;
                           ilog("Compaction needed: background compaction is pending");
                        }
                     } catch (...) {
                        // Fallback to always compact if we can't parse metrics
                        needs_compaction = true;
                     }
                  } else {
                     // Simple periodic compaction when detailed monitoring is disabled
                     needs_compaction = true;
                  }

                  if (needs_compaction && db_->compact_database()) {
                     ilog("Scheduled database compaction completed successfully");
                  } else if (needs_compaction) {
                     wlog("Scheduled database compaction failed");
                  } else {
                     dlog("Database compaction skipped - not needed at this time");
                  }
               }

               if (db_->validate_and_repair_database()) {
                  ilog("Scheduled database validation completed successfully");
               } else {
                  wlog("Scheduled database validation encountered issues");
               }
            })) {
               wlog("Skipped transaction history maintenance at block ${block}: worker queue is full",
                    ("block", result.block_num));
            }
         }

         // Comprehensive database analysis (less frequent)
         if (auto_tuning_enabled_ && analysis_interval_ > 0 &&
             result.block_num > last_analysis_block_ + analysis_interval_) {
            last_analysis_block_ = result.block_num;

            // Run comprehensive analysis in background
            if (!worker_->try_enqueue_task([this, block_num = result.block_num]() {
               ilog("Starting comprehensive database analysis at block ${block}", ("block", block_num));

               try {
                  // Get tuning recommendations
                  std::string tuning_recommendations = db_->get_tuning_recommendations();
                  ilog("Database tuning recommendations: ${recommendations}",
                       ("recommendations", tuning_recommendations));

                  // Analyze key distribution
                  std::string distribution_analysis = db_->analyze_key_distribution();
                  ilog("Key distribution analysis: ${analysis}",
                       ("analysis", distribution_analysis));

                  // Check for optimization opportunities
                  fc::variant tuning_json = fc::json::from_string(tuning_recommendations);
                  if (tuning_json.is_object()) {
                     auto tuning_obj = tuning_json.get_object();

                     // Check for performance warnings
                     if (tuning_obj.contains("performance")) {
                        auto perf_obj = tuning_obj["performance"].get_object();
                        if (perf_obj.size() > 0) {
                           wlog("Performance issues detected during analysis at block ${block}. "
                                "Check tuning recommendations for details.", ("block", block_num));
                        }
                     }

                     // Check for maintenance recommendations
                     if (tuning_obj.contains("maintenance")) {
                        auto maint_obj = tuning_obj["maintenance"].get_object();
                        if (maint_obj.size() > 0) {
                           ilog("Maintenance recommendations available at block ${block}. "
                                "Consider applying suggested optimizations.", ("block", block_num));
                        }
                     }
                  }

                  // Estimate optimal cache size
                  size_t optimal_cache = db_->estimate_optimal_cache_size();
                  ilog("Estimated optimal cache size: ${size} MB",
                       ("size", optimal_cache / (1024 * 1024)));

               } catch (const std::exception& e) {
                  elog("Error during comprehensive database analysis: ${error}", ("error", e.what()));
               }
            })) {
               wlog("Skipped transaction history analysis at block ${block}: worker queue is full",
                    ("block", result.block_num));
            }
         }

         // Convert action traces to variants with size monitoring
         size_t total_size = 0;

         for (const auto* action_trace : filtered_actions) {
            fc::variant action_var;
            fc::to_variant(*action_trace, action_var);

            size_t action_size = fc::raw::pack_size(action_var);
            total_size += action_size;

            result.traces.push_back(action_var);
         }

         // Warn if transaction trace size exceeds configured limit
         if (total_size > max_trace_size_) {
            wlog("Transaction ${id} trace size ${size} bytes exceeds limit ${limit}, but storing anyway",
                 ("id", trace->id)("size", total_size)("limit", max_trace_size_));
         }

         std::vector<std::pair<std::string, std::string>> writes;
         size_t write_bytes = 0;
         constexpr uint64_t fixed_metadata_reserve = 256;
         constexpr uint64_t account_metadata_reserve = 128;
         const auto append_write = [&writes, &write_bytes, this](std::string key, std::string value) {
            const uint64_t candidate = write_bytes + key.size() + value.size();
            if (candidate > max_write_batch_bytes_) return false;
            write_bytes = candidate;
            writes.emplace_back(std::move(key), std::move(value));
            return true;
         };
         const std::string transaction_json = object_to_json(result);
         const std::string block_key = make_block_transaction_key(result.block_num, trace->id);
         const uint64_t base_bytes = trx_key.size() + transaction_json.size() +
                                     block_key.size() + trx_key.size();
         if (base_bytes > max_write_batch_bytes_ ||
             fixed_metadata_reserve > max_write_batch_bytes_ - base_bytes ||
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
               fc::variant action_variant;
               fc::to_variant(*action_trace, action_variant);

               std::map<std::string, fc::variant> base_action_info;
               base_action_info["trx_id"] = trace->id;
               base_action_info["block_num"] = result.block_num;
               base_action_info["block_time"] = result.block_time;
               base_action_info["global_sequence"] = action_trace->receipt->global_sequence;
               base_action_info["global_action_seq"] = action_trace->receipt->global_sequence;
               base_action_info["account"] = action_trace->receipt->receiver;
               base_action_info["action_name"] = action_trace->act.name;
               base_action_info["action_trace"] = std::move(action_variant);

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
                  auto action_info = base_action_info;
                  const uint64_t account_sequence = new_account ? load_account_sequence(account)
                                                                 : next_sequences.at(account);
                  action_info["account_action_seq"] = account_sequence;
                  const std::string account_key = make_account_action_key(account, account_sequence);
                  const std::string action_json = object_to_json(action_info);
                  const uint64_t reserve = fixed_metadata_reserve +
                     (next_sequences.size() + (new_account ? 1 : 0)) * account_metadata_reserve;
                  const uint64_t index_bytes = account_key.size() + action_json.size();
                  if (index_bytes > max_write_batch_bytes_ ||
                      write_bytes > max_write_batch_bytes_ - index_bytes ||
                      reserve > max_write_batch_bytes_ - write_bytes - index_bytes) {
                     index_budget_exhausted = true;
                     break;
                  }

                  auto [sequence_it, inserted] = next_sequences.try_emplace(account, account_sequence);
                  if (inserted) {
                     sequence_it->second = account_sequence;
                  }
                  ++sequence_it->second;
                  EOS_ASSERT(append_write(account_key, action_json), chain::plugin_exception,
                             "transaction history index exceeded the reserved byte budget");
                  ++account_indexes;
               }
               indexed_actions++;
            }
         }

         for (const auto& [account, next_sequence] : next_sequences) {
            EOS_ASSERT(append_write("_internal_account_sequence:" + account.to_string(),
                                    std::to_string(next_sequence)),
                       chain::plugin_exception,
                       "transaction history write batch limit leaves no room for sequence metadata");
         }

         bool advance_last_block = false;
         {
            std::lock_guard<std::mutex> lock(last_updated_block_mutex_);
            advance_last_block = result.block_num > last_updated_block_;
         }
         if (advance_last_block) {
            EOS_ASSERT(append_write("_internal_last_block_number", std::to_string(result.block_num)),
                       chain::plugin_exception, "transaction history write batch limit leaves no room for metadata");
         }

         // The transaction record, block index, account indexes, sequence
         // cursors, and database height must become visible atomically.
         if (!batch_write_with_retry(writes, {})) {
            transactions_failed_++;
            record_history_gap(result.block_num,
                               "failed to atomically store transaction " + trace->id.str());
            return;
         }

         if (advance_last_block) {
            std::lock_guard<std::mutex> lock(last_updated_block_mutex_);
            last_updated_block_ = std::max(last_updated_block_, result.block_num);
         }

         // Warn if too many actions were skipped in indexing
         if (filtered_actions.size() > max_actions_per_tx_ || index_budget_exhausted) {
            wlog("Transaction ${id} has ${total} actions, only indexed first ${indexed} actions",
                 ("id", trace->id)("total", filtered_actions.size())("indexed", indexed_actions));
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
         record_history_gap(block_num, std::string("transaction history processing exception: ") + e.what());
         elog("Error processing transaction ${id}: ${what}",
              ("id", trace->id)("what", e.what()));
      } catch (...) {
         transactions_failed_++;
         record_history_gap(block_num, "unknown transaction history processing exception");
         elog("Unknown error processing transaction ${id}", ("id", trace->id));
      }
      })) {
      transactions_failed_++;
      record_history_gap(block_num, "history queue byte or task budget exhausted");
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

bool transaction_history_plugin_impl::filter_action(const eosio::chain::action_trace& action_trace) const {
   if (!action_trace.receipt) {
      return false;
   }

   const auto matches = [&action_trace](const std::set<filter_entry>& filters) {
      const auto receiver = action_trace.receipt->receiver;
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

   // Warn about max_retained_blocks setting if it's too low
   if (max_retained_blocks_ < pending_blocks + 1000) {
      wlog("Transaction history warning: max-retained-blocks (${max}) is close to pending blocks (${pending}). "
           "Increase checkpoint retention to preserve the complete reversible fork window.",
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
   std::unique_ptr<rocksdb::Iterator> iterator(history->my->db_->new_iterator());
   EOS_ASSERT(iterator, chain::plugin_exception, "Transaction history database is not open");

   auto has_prefix = [&prefix](const rocksdb::Iterator& it) {
      return it.Valid() && it.key().starts_with(prefix);
   };
   size_t response_bytes = 0;
   bool byte_limit_reached = false;
   const uint64_t max_response_bytes = history->my->max_api_response_bytes_;
   auto& controller = history->my->chain_plug->chain();
   const auto abi_yield = eosio::chain::abi_serializer::create_yield_function(
      history->my->chain_plug->get_abi_serializer_max_time());
   auto append_current = [&result, &response_bytes, &byte_limit_reached,
                          max_response_bytes, &controller, &abi_yield](const rocksdb::Iterator& it) {
      const auto value = it.value();
      EOS_ASSERT(value.size() <= max_response_bytes, chain::plugin_exception,
                 "A single history action record exceeds the configured API response byte limit");
      std::map<std::string, fc::variant> action;
      try {
         action = fc::json::from_string(value.ToString())
            .as<std::map<std::string, fc::variant>>();
      } catch (...) {
         return false;
      }
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
         return false;
      }
      result.actions.emplace_back(std::move(action));
      response_bytes += serialized_size;
      return true;
   };

   size_t scanned = 0;
   if (offset < 0) {
      if (!params.pos || *params.pos < 0) {
         iterator->Seek(prefix + "\xff");
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

      while (has_prefix(*iterator) && result.actions.size() < requested) {
         if (fc::time_point::now() >= deadline) {
            result.time_limit_exceeded_error = true;
            break;
         }
         if (!append_current(*iterator) && byte_limit_reached) break;
         ++scanned;
         iterator->Prev();
      }
      result.more = has_prefix(*iterator) || byte_limit_reached ||
                    result.time_limit_exceeded_error.value_or(false);
      std::reverse(result.actions.begin(), result.actions.end());
   } else {
      if (params.pos && *params.pos > 0) {
         iterator->Seek(history->make_account_action_key(
            params.account_name, static_cast<uint64_t>(*params.pos)));
      } else {
         iterator->Seek(prefix);
      }

      while (has_prefix(*iterator) && result.actions.size() < requested) {
         if (fc::time_point::now() >= deadline) {
            result.time_limit_exceeded_error = true;
            break;
         }
         if (!append_current(*iterator) && byte_limit_reached) break;
         ++scanned;
         iterator->Next();
      }
      result.more = has_prefix(*iterator) || byte_limit_reached ||
                    result.time_limit_exceeded_error.value_or(false);
   }

   EOS_ASSERT(iterator->status().ok(), chain::plugin_exception,
              "Failed to scan account history: ${error}",
              ("error", iterator->status().ToString()));

   EOS_ASSERT(scanned <= transaction_history_plugin_impl::MAX_API_RESULTS * 4,
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
   std::unique_ptr<rocksdb::Iterator> iterator(history->my->db_->new_iterator());
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
         if (history->my->rollback_mgr_) {
            metrics["history_checkpoint_count"] = history->my->rollback_mgr_->rollback_point_count();
            metrics["history_latest_checkpoint_block"] =
               history->my->rollback_mgr_->get_latest_rollback_point().value_or(0);
         }
         metrics["transactions_processed"] = history->my->transactions_processed_.load();
         metrics["transactions_failed"] = history->my->transactions_failed_.load();
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

read_only::trigger_auto_optimize_result read_only::trigger_auto_optimize(const trigger_auto_optimize_params& params) const {
   auto database_lock = history->my->db_->acquire_read_lock();
   trigger_auto_optimize_result result;

   if (history && history->get_db_manager()) {
      uint32_t max_duration = params.max_duration_seconds.value_or(300);
      bool success = history->get_db_manager()->auto_optimize(max_duration);

      result.success = success;
      if (!success) {
         result.error = "Auto-optimization failed - check logs for details";
      } else {
         result.message = "Auto-optimization completed successfully";
      }
   } else {
      result.success = false;
      result.error = "Database manager not available";
   }

   return result;
}

} // namespace transaction_history_apis

} // namespace eosio
