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
#include <fc/io/json.hpp>
#include <fc/crypto/sha256.hpp>

#include <boost/signals2/connection.hpp>
#include <boost/program_options.hpp>
#include <filesystem>

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
   std::atomic<uint32_t> last_health_check_block_{0};
   std::atomic<uint32_t> last_maintenance_block_{0};
   std::atomic<uint32_t> last_analysis_block_{0};
   const uint32_t warning_interval_ = 10000; // Warn every 10000 blocks
   const uint32_t health_check_interval_ = 50000; // Health check every 50000 blocks
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
      ("transaction-history-filter-on", bpo::value<std::vector<std::string>>()->composing(),
       "Track actions which match account:action:actor. Actor may be blank to include all actors.")
      ("transaction-history-filter-out", bpo::value<std::vector<std::string>>()->composing(),
       "Do not track actions which match account:action:actor. Actor may be blank to exclude all actors.");
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

      // Validate configuration parameters
      EOS_ASSERT(!my->db_path_.empty(), eosio::chain::plugin_exception,
                 "transaction-history-dir cannot be empty");

      ilog("Transaction history monitoring: max trace size: ${trace_size} bytes, max retained blocks: ${blocks}, max actions per tx: ${actions}, compression: ${compression}",
           ("trace_size", transaction_history_plugin_impl::MAX_TRACE_SIZE)
           ("blocks", transaction_history_plugin_impl::MAX_RETAINED_BLOCKS)
           ("actions", transaction_history_plugin_impl::MAX_ACTIONS_PER_TX)
           ("compression", my->compression_enabled_ ? "enabled" : "disabled"));

      // Initialize components
      my->db_ = std::make_shared<rocksdb_manager>();
      my->worker_ = std::make_unique<async_worker>();
      my->rollback_mgr_ = std::make_unique<rollback_manager>(my->db_);

      if (!my->db_->open(my->db_path_, my->compression_enabled_)) {
         throw std::runtime_error("Failed to open transaction history database at: " + my->db_path_);
      }

      // Handle force clean option
      if (my->force_clean_enabled_) {
         ilog("Force clean enabled: clearing all transaction history data");
         if (!my->db_->clear_all_data()) {
            wlog("Failed to perform force clean of transaction history database");
         } else {
            ilog("Force clean completed successfully");
         }
      }

      // Validate database integrity if requested
      if (my->validate_on_startup_enabled_) {
         ilog("Validating database integrity on startup...");
         if (!my->db_->validate_and_repair_database()) {
            wlog("Database validation encountered issues, but continuing startup");
         }
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
      } else if (earliest_available > 1 && chain_head_block - earliest_available < 1000) {
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
         // Continue anyway, but warn user
         wlog("Transaction history plugin will continue, but data consistency may be affected");
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

   // Log final database statistics
   std::string db_stats = my->db_->get_database_stats();
   dlog("Transaction history database statistics: ${stats}", ("stats", db_stats));

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

         // Periodic database health check
         if (result.block_num > last_health_check_block_ + health_check_interval_) {
            last_health_check_block_ = result.block_num;

            // Enhanced health monitoring
            worker_->enqueue_task([this, block_num = result.block_num]() {
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
            });
         }

         // Periodic database maintenance
         if (maintenance_interval_ > 0 && result.block_num > last_maintenance_block_ + maintenance_interval_) {
            last_maintenance_block_ = result.block_num;

            // Run maintenance in background to avoid blocking transaction processing
            worker_->enqueue_task([this, block_num = result.block_num]() {
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
            });
         }

         // Comprehensive database analysis (less frequent)
         if (auto_tuning_enabled_ && analysis_interval_ > 0 &&
             result.block_num > last_analysis_block_ + analysis_interval_) {
            last_analysis_block_ = result.block_num;

            // Run comprehensive analysis in background
            worker_->enqueue_task([this, block_num = result.block_num]() {
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
            });
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

         // Update last processed block number for database state tracking
         static uint32_t last_updated_block = 0;
         if (result.block_num > last_updated_block) {
            if (!db_->update_last_block_number(result.block_num)) {
               wlog("Failed to update last block number to ${block}", ("block", result.block_num));
            }
            last_updated_block = result.block_num;
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
   std::string key = history->make_transaction_key(params.id);
   get_transaction_result result;

   if (!history->my->db_->get_object(key, result)) {
      EOS_THROW(chain::tx_not_found, "Transaction ${id} not found in history", ("id", params.id));
   }

   // Update current irreversible block
   result.last_irreversible_block = history->get_last_irreversible_block_num();

   return result;
}

read_only::get_actions_result read_only::get_actions(const get_actions_params& params) const {
   get_actions_result result;
   result.last_irreversible_block = history->my->chain_plug->chain().last_irreversible_block_num();
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
   result.end_block = params.end_block.value_or(history->my->chain_plug->chain().head().block_num());

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

read_only::get_database_stats_result read_only::get_database_stats(const get_database_stats_params& params) const {
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
   get_performance_metrics_result result;

   if (history && history->get_db_manager()) {
      std::string metrics_json = history->get_db_manager()->get_performance_metrics();
      try {
         result.metrics = fc::json::from_string(metrics_json);
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
