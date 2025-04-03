#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/deep_mind.hpp>

#include <bit>

namespace eosio::chain {

   transaction_checktime_timer::transaction_checktime_timer(platform_timer& timer)
         : expired(timer.expired), _timer(timer) {
      expired = 0;
   }

   void transaction_checktime_timer::start(fc::time_point tp) {
      _timer.start(tp);
   }

   void transaction_checktime_timer::stop() {
      _timer.stop();
   }

   void transaction_checktime_timer::set_expiration_callback(void(*func)(void*), void* user) {
      _timer.set_expiration_callback(func, user);
   }

   transaction_checktime_timer::~transaction_checktime_timer() {
      stop();
      _timer.set_expiration_callback(nullptr, nullptr);
   }

   transaction_context::transaction_context( controller& c,
                                             const packed_transaction& t,
                                             const transaction_id_type& trx_id,
                                             transaction_checktime_timer&& tmr,
                                             action_digests_t::store_which_t store_which,
                                             fc::time_point s,
                                             transaction_metadata::trx_type type)
   :control(c)
   ,packed_trx(t)
   ,id(trx_id)
   ,undo_session()
   ,trace(std::make_shared<transaction_trace>())
   ,start(s)
   ,executed_action_receipts(store_which)
   ,transaction_timer(std::move(tmr))
   ,trx_type(type)
   ,net_usage(trace->res_usage.net_usage)
   ,pseudo_start(s)
   {
      if (!c.skip_db_sessions() && !is_read_only()) {
         undo_session.emplace(c.mutable_db().start_undo_session(true));
      }
      trace->id = id;
      trace->block_num = c.head().block_num() + 1;
      trace->block_time = c.pending_block_time();
      trace->producer_block_id = c.pending_producer_block_id();

      if(auto dm_logger = c.get_deep_mind_logger(is_transient()))
      {
         dm_logger->on_start_transaction();
      }
   }

   transaction_context::~transaction_context()
   {
      if(auto dm_logger = control.get_deep_mind_logger(is_transient()))
      {
         dm_logger->on_end_transaction();
      }
   }

   void transaction_context::disallow_transaction_extensions( const char* error_msg )const {
      if( control.is_speculative_block() ) {
         EOS_THROW( subjective_block_production_exception, error_msg );
      } else {
         EOS_THROW( disallowed_transaction_extensions_bad_block_exception, error_msg );
      }
   }

   void transaction_context::init(uint64_t initial_net_usage)
   {
      EOS_ASSERT( !is_initialized, transaction_exception, "cannot initialize twice" );

      // set maximum to a semi-valid deadline to allow for pause math and conversion to dates for logging
      if( block_deadline == fc::time_point::maximum() ) block_deadline = start + fc::hours(24*7*52);

      const auto& cfg = control.get_global_properties().configuration;
      auto& rl = control.get_mutable_resource_limits_manager();

      net_limit = rl.get_block_net_limit();

      objective_duration_limit = fc::microseconds( rl.get_block_cpu_limit() );
      assert(initial_cpu_exception_code == block_cpu_usage_exceeded::code_value);
      _deadline = start + objective_duration_limit;

      // Possibly lower net_limit to the maximum net usage a transaction is allowed to be billed
      if( cfg.max_transaction_net_usage <= net_limit && !is_read_only() ) {
         net_limit = cfg.max_transaction_net_usage;
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to the maximum cpu usage a transaction is allowed to be billed
      if( cfg.max_transaction_cpu_usage <= objective_duration_limit.count() && !is_read_only() ) {
         objective_duration_limit = fc::microseconds(cfg.max_transaction_cpu_usage);
         initial_cpu_exception_code = tx_cpu_usage_exceeded::code_value;
         initial_tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::on_chain_consensus_max_transaction_cpu_usage;
         _deadline = start + objective_duration_limit;
      }

      const transaction& trx = packed_trx.get_transaction();
      // Possibly lower net_limit to optional limit set in the transaction header
      uint64_t trx_specified_net_usage_limit = static_cast<uint64_t>(trx.max_net_usage_words.value) * 8;
      if( trx_specified_net_usage_limit > 0 && trx_specified_net_usage_limit <= net_limit ) {
         net_limit = trx_specified_net_usage_limit;
         net_limit_due_to_block = false;
      }

      // Possibly lower objective_duration_limit to optional limit set in transaction header
      if( trx.max_cpu_usage_ms > 0 ) {
         auto trx_specified_cpu_usage_limit = fc::milliseconds(trx.max_cpu_usage_ms);
         if( trx_specified_cpu_usage_limit <= objective_duration_limit ) {
            objective_duration_limit = trx_specified_cpu_usage_limit;
            initial_cpu_exception_code = tx_cpu_usage_exceeded::code_value;
            initial_tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::user_specified_trx_max_cpu_usage_ms;
            _deadline = start + objective_duration_limit;
         }
      }

      // Possibly limit deadline to subjective max_transaction_time
      if( max_transaction_time_subjective != fc::microseconds::maximum() && max_transaction_time_subjective <= objective_duration_limit ) {
         objective_duration_limit = max_transaction_time_subjective;
         initial_cpu_exception_code = tx_cpu_usage_exceeded::code_value;
         initial_tx_cpu_usage_reason = billed_cpu_time_us > 0 ?
            tx_cpu_usage_exceeded_reason::speculative_executed_adjusted_max_transaction_time :
            tx_cpu_usage_exceeded_reason::node_configured_max_transaction_time;
         _deadline = start + objective_duration_limit;
      }

      initial_objective_duration_limit = objective_duration_limit;
      tx_cpu_usage_reason = initial_tx_cpu_usage_reason;
      deadline_exception_code = initial_cpu_exception_code;

      if ( !is_read_only() ) {
         bill_to_account = trx.first_authorizer();
         // TODO: check first_authorizer is valid?
         trace->res_usage.payer = bill_to_account;
         // TODO: need to do?  validate_ram_usage.reserve( 1 );
      }

      // Possibly limit deadline to caller provided wall clock block deadline
      if( block_deadline < _deadline ) {
         _deadline = block_deadline;
         initial_cpu_exception_code = deadline_exception::code_value;
      }

      net_limit = (net_limit/8)*8; // Round down to nearest multiple of word size (8 bytes) so check_net_limit can be efficient

      // add net usage
      if (initial_net_usage > 0) {
         // net_usage is reference of trace->res_usage.net_usage
         calc_utils::verify_add(net_usage, initial_net_usage, "initial net usage to pending net usage of transaction payer");
         net_usage += initial_net_usage;
      }

      auto now = fc::time_point::now();
      if ( !is_read_only() && !control.skip_trx_checks() ) {

         trace->res_usage.cpu_usage = update_billed_cpu_time(now);

         check_net_limit();
         check_cpu_limit(now, false);

         uint64_t reserved_gas = 0;
         bool is_unlimited = false;
         rl.get_account_limits(bill_to_account, reserved_gas, is_unlimited);
         if (!is_unlimited) {
            auto convertible_gas = rl.get_account_convertible_gas(bill_to_account);
            // the trace->res_usage.cpu_usage and trace->res_usage.net_usage have been set above
            rl.calc_transaction_gas_usage( trace->res_usage);
            rl.verify_transaction_gas_usage(trace->res_usage, reserved_gas, convertible_gas);

            if( !explicit_billed_cpu_time ) {
               // Calculate the highest network usage and CPU time that billed account(payer) can afford to be billed
               calc_utils::verify_add(reserved_gas, convertible_gas, "reserved gas and convertible gas of transaction payer");
               uint64_t gas_limit = reserved_gas + convertible_gas;
               assert(std::numeric_limits<int64_t>::max() - trace->res_usage.cpu_gas >= trace->res_usage.net_gas); // has been verified in calc_transaction_gas_usage()
               uint64_t used_gas = trace->res_usage.cpu_gas + trace->res_usage.net_gas;
               EOS_ASSERT( gas_limit > used_gas,
                  tx_gas_usage_exceeded,
                  "authorizing account '${n}' has insufficient gas for cpu to execute this transaction",
                  ("n", trace->res_usage.payer));
               uint64_t available_gas = gas_limit - used_gas;
               uint64_t account_cpu_limit = rl.convert_gas_to_cpu(available_gas);

               // Possibly limit deadline if the duration accounts can be billed for (+ a subjective leeway) does not exceed current delta
               if( account_cpu_limit < (uint64_t)objective_duration_limit.count() ) {
                  objective_duration_limit = fc::microseconds(account_cpu_limit);
                  deadline_exception_code = leeway_deadline_exception::code_value;
                  tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
                  _deadline = start + objective_duration_limit;
               }

               if( subjective_cpu_bill_us > 0) {
                  EOS_ASSERT( account_cpu_limit > (uint64_t)subjective_cpu_bill_us,
                     tx_gas_usage_exceeded,
                     "authorizing account '${n}' has insufficient gas for cpu to execute this transaction"
                     " with a subjective cpu of (${subjective} us",
                     ("n", trace->res_usage.payer)
                     ("subjective", subjective_cpu_bill_us)
                  );

                  account_cpu_limit = account_cpu_limit - (uint64_t)subjective_cpu_bill_us;
                  if( account_cpu_limit < (uint64_t)objective_duration_limit.count() ) {
                     objective_duration_limit = fc::microseconds(account_cpu_limit);
                     deadline_exception_code = tx_cpu_usage_exceeded::code_value;
                     tx_cpu_usage_reason = tx_cpu_usage_exceeded_reason::account_cpu_limit;
                     _deadline = start + objective_duration_limit;
                  }
               }
            }
         }
      }

      // Explicit billed_cpu_time_us should be used, block_deadline will be maximum unless in test code
      if( explicit_billed_cpu_time ) {
         _deadline = block_deadline;
         deadline_exception_code = deadline_exception::code_value;
      }

      if(control.skip_trx_checks()) {
         transaction_timer.start( fc::time_point::maximum() );
      } else {
         transaction_timer.start( _deadline );
         checktime(); // Fail early if deadline has already been exceeded
      }

      is_initialized = true;
   }

   void transaction_context::init_for_implicit_trx( uint64_t initial_net_usage  )
   {
      const transaction& trx = packed_trx.get_transaction();
      if( trx.transaction_extensions.size() > 0 ) {
         disallow_transaction_extensions( "no transaction extensions supported yet for implicit transactions" );
      }

      published = control.pending_block_time();
      init( initial_net_usage);
   }

   void transaction_context::init_for_input_trx( uint64_t packed_trx_unprunable_size,
                                                 uint64_t packed_trx_prunable_size )
   {
      const transaction& trx = packed_trx.get_transaction();
      #ifndef ENABLE_DEFERRED_TRANSACTION
      EOS_ASSERT( trx.delay_sec.value == 0, transaction_exception, "transaction cannot be delayed" );
      #else //ENABLE_DEFERRED_TRANSACTION
      // delayed transactions are not allowed after protocol feature
      // DISABLE_DEFERRED_TRXS_STAGE_1 is activated;
      // read-only and dry-run transactions are not allowed to be delayed at any time
      if( control.is_builtin_activated(builtin_protocol_feature_t::disable_deferred_trxs_stage_1) || is_transient() ) {
         EOS_ASSERT( trx.delay_sec.value == 0, transaction_exception, "transaction cannot be delayed" );
      }
      #endif//ENABLE_DEFERRED_TRANSACTION

      if( trx.transaction_extensions.size() > 0 ) {
         disallow_transaction_extensions( "no transaction extensions supported yet for input transactions" );
      }

      const auto& cfg = control.get_global_properties().configuration;

      uint64_t discounted_size_for_pruned_data = packed_trx_prunable_size;
      if( cfg.context_free_discount_net_usage_den > 0
          && cfg.context_free_discount_net_usage_num < cfg.context_free_discount_net_usage_den )
      {
         calc_utils::verify_multiply(discounted_size_for_pruned_data, cfg.context_free_discount_net_usage_num, "discounted_size_for_pruned_data and context_free_discount_net_usage_num");
         discounted_size_for_pruned_data *= cfg.context_free_discount_net_usage_num;
         calc_utils::verify_add(discounted_size_for_pruned_data, cfg.context_free_discount_net_usage_den - 1, "discounted_size_for_pruned_data and context_free_discount_net_usage_den-1");
         discounted_size_for_pruned_data =  ( discounted_size_for_pruned_data + cfg.context_free_discount_net_usage_den - 1)
                                                                                    / cfg.context_free_discount_net_usage_den; // rounds up
      }

      uint64_t initial_net_usage = static_cast<uint64_t>(cfg.base_per_transaction_net_usage);
      calc_utils::verify_add(initial_net_usage, packed_trx_unprunable_size, "initial_net_usage and packed_trx_unprunable_size");
      initial_net_usage += packed_trx_unprunable_size;
      calc_utils::verify_add(initial_net_usage, discounted_size_for_pruned_data, "initial_net_usage and discounted_size_for_pruned_data");
      initial_net_usage += discounted_size_for_pruned_data;

      #ifdef ENABLE_DEFERRED_TRANSACTION
      if( trx.delay_sec.value > 0 ) {
          // If delayed, also charge ahead of time for the additional net usage needed to retire the delayed transaction
          // whether that be by successfully executing, soft failure, hard failure, or expiration.
         initial_net_usage += static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                               + static_cast<uint64_t>(config::transaction_id_net_usage);
      }
      #endif//ENABLE_DEFERRED_TRANSACTION

      published = control.pending_block_time();
      is_input = true;
      if (!control.skip_trx_checks()) {
         if ( !is_read_only() ) {
            control.validate_expiration(trx);
            control.validate_tapos(trx);
         }
         validate_referenced_accounts( trx, enforce_whiteblacklist && control.is_speculative_block() );
      }

      init( initial_net_usage );
      if ( !is_read_only() ) {
         record_transaction( id, trx.expiration );
      }
   }

   #ifdef ENABLE_DEFERRED_TRANSACTION
   void transaction_context::init_for_deferred_trx( fc::time_point p )
   {
      const transaction& trx = packed_trx.get_transaction();
      if( (trx.expiration.sec_since_epoch() != 0) && (trx.transaction_extensions.size() > 0) ) {
         disallow_transaction_extensions( "no transaction extensions supported yet for deferred transactions" );
      }
      // If (trx.expiration.sec_since_epoch() == 0) then it was created after NO_DUPLICATE_DEFERRED_ID activation,
      // and so validation of its extensions was done either in:
      //   * apply_context::schedule_deferred_transaction for contract-generated transactions;
      //   * or transaction_context::init_for_input_trx for delayed input transactions.

      published = p;
      trace->scheduled = true;
      apply_context_free = false;
      init( 0 );
   }
   #endif//ENABLE_DEFERRED_TRANSACTION

   void transaction_context::exec() {
      EOS_ASSERT( is_initialized, transaction_exception, "must first initialize" );

      const transaction& trx = packed_trx.get_transaction();
      if( apply_context_free ) {
         for( const auto& act : trx.context_free_actions ) {
            schedule_action( act, act.account, true, 0, 0 );
         }
      }

      if( delay == fc::microseconds() ) {
         for( const auto& act : trx.actions ) {
            schedule_action( act, act.account, false, 0, 0 );
         }
      }

      auto& action_traces = trace->action_traces;
      uint32_t num_original_actions_to_execute = action_traces.size();
      for( uint32_t i = 1; i <= num_original_actions_to_execute; ++i ) {
         execute_action( i, 0 );
      }

      #ifdef ENABLE_DEFERRED_TRANSACTION
      if( delay != fc::microseconds() ) {
         schedule_transaction();
      }
      #else //!ENABLE_DEFERRED_TRANSACTION
      EOS_ASSERT( delay == fc::microseconds(), transaction_exception, "transaction cannot be delayed" );
      #endif//ENABLE_DEFERRED_TRANSACTION
   }

   void transaction_context::finalize() {
      EOS_ASSERT( is_initialized, transaction_exception, "must first initialize" );

      // read-only transactions only need net_usage and elapsed in the trace
      if ( is_read_only() ) {
         calc_utils::verify_add(net_usage, 7, "net_usage and 7");
         net_usage = ((net_usage + 7)/8)*8; // Round up to nearest multiple of word size (8 bytes)
         trace->elapsed = fc::time_point::now() - start;
         return;
      }

      if( is_input ) {
         const transaction& trx = packed_trx.get_transaction();
         auto& am = control.get_mutable_authorization_manager();
         for( const auto& act : trx.actions ) {
            for( const auto& auth : act.authorization ) {
               am.update_permission_usage( am.get_permission(auth) );
            }
         }
      }

      auto& rl = control.get_mutable_resource_limits_manager();
      flat_set<account_gas_trace> gas_traces;
      gas_traces.reserve(ram_deltas.size() + 1);
      for( auto a : ram_deltas ) {
         std::optional<account_gas_trace> gas_trace;
         rl.add_ram_usage(a.first, a.second, gas_trace);
         if (gas_trace) {
            gas_traces.emplace(std::move(*gas_trace));
         }
      }

      calc_utils::verify_add(net_usage, 7, "net_usage and 7");
      net_usage = ((net_usage + 7)/8)*8; // Round up to nearest multiple of word size (8 bytes)
      check_net_limit();

      auto now = fc::time_point::now();
      trace->elapsed = now - start;
      update_billed_cpu_time( now );
      check_cpu_limit(now, true);

      // validate_cpu_usage_to_bill( billed_cpu_time_us, account_cpu_limit, true, subjective_cpu_bill_us );

      trace->res_usage.cpu_usage = static_cast<uint64_t>(billed_cpu_time_us);
      trace->res_usage.net_usage = net_usage;
      std::optional<account_gas_trace> trx_gas_trace;
      rl.add_transaction_usage( trace->res_usage, trx_gas_trace, is_transient() ); // Should never fail
      if (trx_gas_trace) {
         auto itr = trace->gas_traces.find(trace->res_usage.payer);
         if (itr == trace->gas_traces.end()) {
            trace->gas_traces.emplace(std::move(*trx_gas_trace));
         } else {
            // the itr->reserved_gas_before exists, can not assign again

            calc_utils::verify_add(itr->used_gas, trx_gas_trace->used_gas, "new used gas to existed of transaction payer");
            calc_utils::verify_add(itr->converted_gas, trx_gas_trace->converted_gas, "new converted gas to existed of transaction payer");
            itr->reserved_gas_after = trx_gas_trace->reserved_gas_after;
            itr->used_gas          += trx_gas_trace->used_gas;
            itr->converted_gas     += trx_gas_trace->converted_gas;
         }
      }


      trace->gas_traces = std::move(gas_traces);
   }

   void transaction_context::squash() {
      if (undo_session) undo_session->squash();
      control.apply_trx_block_context(trx_blk_context);
   }

   void transaction_context::undo() {
      if (undo_session) undo_session->undo();
   }

   void transaction_context::add_net_usage( uint64_t u ) {
      calc_utils::verify_add(net_usage, u, "new net net usage to pending net usage of transaction payer");
      net_usage += u;
      check_net_limit();
      validate_transaction_usage();
   }

   void transaction_context::check_cpu_limit(const fc::time_point& now, bool check_minimum) const {
      if (!control.skip_trx_checks()) {
         if( check_minimum ) {
            const auto& cfg = control.get_global_properties().configuration;
            EOS_ASSERT( billed_cpu_time_us >= cfg.min_transaction_cpu_usage, transaction_exception,
                        "cannot bill CPU time less than the minimum of ${min_billable} us",
                        ("min_billable", cfg.min_transaction_cpu_usage)("billed_cpu_time_us", billed_cpu_time_us)
                      );
         }
         // TODO: rename initial_objective_duration_limit -> initial_cpu_limit
         if (billed_cpu_time_us > initial_objective_duration_limit.count() ) {
            if( deadline_exception_code == block_cpu_usage_exceeded::code_value ) {
               EOS_THROW( block_cpu_usage_exceeded,
                           "not enough time left in block to complete executing transaction ${billed_cpu_time_us}us",
                           ("now", now)("deadline", _deadline)("start", start)("billed_cpu_time_us", billed_cpu_time_us) );
            } else if( deadline_exception_code == tx_cpu_usage_exceeded::code_value ) {
               std::string assert_msg = "transaction ${id} was executing for too long ${billed_cpu_time_us}us";

               if (subjective_cpu_bill_us > 0) {
                  assert_msg += " with a subjective cpu of (${subjective} us)";
               }

               // fc::microseconds limit;
               assert_msg += get_tx_cpu_usage_exceeded_reason_msg(initial_tx_cpu_usage_reason);
               EOS_THROW( tx_cpu_usage_exceeded, assert_msg, ("id", packed_trx.id())
                        ("billed_cpu_time_us", billed_cpu_time_us)
                        ("subjective", subjective_cpu_bill_us)
                        ("limit", initial_objective_duration_limit) );

            }
         }
      }
   }

   void transaction_context::check_net_limit()const {
      if (!control.skip_trx_checks()) {
         if( BOOST_UNLIKELY(net_usage > net_limit) ) {
            if ( net_limit_due_to_block ) {
               EOS_THROW( block_net_usage_exceeded,
                          "not enough space left in block: ${net_usage} > ${net_limit}",
                          ("net_usage", net_usage)("net_limit", net_limit) );
            } else {
               EOS_THROW( tx_net_usage_exceeded,
                          "transaction net usage is too high: ${net_usage} > ${net_limit}",
                          ("net_usage", net_usage)("net_limit", net_limit) );
            }
         }

      }
   }

   void transaction_context::validate_transaction_usage() const {
      if (!control.skip_trx_checks()) {
         auto& rl = control.get_mutable_resource_limits_manager();
         uint64_t reserved_gas = 0;
         bool is_unlimited = false;
         rl.get_account_limits(bill_to_account, reserved_gas, is_unlimited);
         if (!is_unlimited) {
            auto convertible_gas = rl.get_account_convertible_gas(bill_to_account);
            rl.calc_transaction_gas_usage( trace->res_usage);
            rl.verify_transaction_gas_usage(trace->res_usage, reserved_gas, convertible_gas);
         }
      }
   }

   std::string transaction_context::get_tx_cpu_usage_exceeded_reason_msg(tx_cpu_usage_exceeded_reason reason) const {
      switch( reason ) {
         case tx_cpu_usage_exceeded_reason::account_cpu_limit:
            return " reached account cpu limit ${limit}us";
         case tx_cpu_usage_exceeded_reason::on_chain_consensus_max_transaction_cpu_usage:
            return " reached on chain max_transaction_cpu_usage ${limit}us";
         case tx_cpu_usage_exceeded_reason::user_specified_trx_max_cpu_usage_ms:
            return " reached trx specified max_cpu_usage_ms ${limit}us";
         case tx_cpu_usage_exceeded_reason::node_configured_max_transaction_time:
            return " reached node configured max-transaction-time ${limit}us";
         case tx_cpu_usage_exceeded_reason::speculative_executed_adjusted_max_transaction_time:
            return " reached speculative executed adjusted trx max time ${limit}us";
      }
      return "unknown tx_cpu_usage_exceeded ${limit}us";
   }

   void transaction_context::checktime()const {
      if(BOOST_LIKELY(transaction_timer.expired == false))
         return;

      // TODO: update billed_cpu_time_us   if not explicit_billed_cpu_time
      // TODO: if  billed_cpu_time_us > cpu_limit
      // TODO: validate transaction usage ()
      auto now = fc::time_point::now();
      if( explicit_billed_cpu_time || deadline_exception_code == deadline_exception::code_value ) {
         EOS_THROW( deadline_exception, "deadline exceeded ${billing_timer}us",
                     ("billing_timer", now - pseudo_start)("now", now)("deadline", _deadline)("start", start) );
      } else if( deadline_exception_code == block_cpu_usage_exceeded::code_value ) {
         EOS_THROW( block_cpu_usage_exceeded,
                     "not enough time left in block to complete executing transaction ${billing_timer}us",
                     ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
      } else if( deadline_exception_code == tx_cpu_usage_exceeded::code_value ) {
         std::string assert_msg = "transaction ${id} was executing for too long ${billing_timer}us";
         if (subjective_cpu_bill_us > 0) {
            assert_msg += " with a subjective cpu of (${subjective} us)";
         }
         // fc::microseconds limit;
         assert_msg += get_tx_cpu_usage_exceeded_reason_msg(tx_cpu_usage_reason);
         EOS_THROW( tx_cpu_usage_exceeded, assert_msg, ("id", packed_trx.id())
                  ("billing_timer", now - pseudo_start)("subjective", subjective_cpu_bill_us)("limit", objective_duration_limit) );

      } else if( deadline_exception_code == leeway_deadline_exception::code_value ) {
         EOS_THROW( leeway_deadline_exception,
                     "the transaction was unable to complete by deadline, "
                     "but it is possible it could have succeeded if it were allowed to run to completion ${billing_timer}",
                     ("now", now)("deadline", _deadline)("start", start)("billing_timer", now - pseudo_start) );
      }
      EOS_ASSERT( false,  transaction_exception, "unexpected deadline exception code ${code}", ("code", deadline_exception_code) );
   }

   void transaction_context::pause_billing_timer() {
      if( explicit_billed_cpu_time || pseudo_start == fc::time_point() ) return; // either irrelevant or already paused

      paused_time = fc::time_point::now();
      billed_time = paused_time - pseudo_start;
      pseudo_start = fc::time_point();
      transaction_timer.stop();
   }

   void transaction_context::resume_billing_timer() {
      if( explicit_billed_cpu_time || pseudo_start != fc::time_point() ) return; // either irrelevant or already running

      auto now = fc::time_point::now();
      auto paused = now - paused_time;

      pseudo_start = now - billed_time;
      _deadline += paused;

      // do not allow to go past block wall clock deadline
      if( block_deadline < _deadline ) {
         deadline_exception_code = deadline_exception::code_value;
         _deadline = block_deadline;
      }

      transaction_timer.start(_deadline);
   }

   void transaction_context::add_ram_usage( account_name account, int64_t ram_delta ) {
      auto itr = ram_deltas.find(account);
      if (itr == ram_deltas.end()) {
         ram_deltas.emplace(account, ram_delta);
      } else {
         calc_utils::verify_add(itr->second, ram_delta, "delta add to ram usage of account");
         itr->second += ram_delta;
      }
   }

   uint32_t transaction_context::update_billed_cpu_time( fc::time_point now ) {
      if( explicit_billed_cpu_time ) return static_cast<uint32_t>(billed_cpu_time_us);

      const auto& cfg = control.get_global_properties().configuration;
      billed_cpu_time_us = std::max( (now - pseudo_start).count(), static_cast<int64_t>(cfg.min_transaction_cpu_usage) );

      return static_cast<uint32_t>(billed_cpu_time_us);
   }

   int64_t transaction_context::max_cpu_gas_billed_account_can_pay(bool is_cpu_only) {
      auto& rl = control.get_resource_limits_manager();
      return rl.get_account_cpu_limit(bill_to_account);
   }

   action_trace& transaction_context::get_action_trace( uint32_t action_ordinal ) {
      EOS_ASSERT( 0 < action_ordinal && action_ordinal <= trace->action_traces.size() ,
                  transaction_exception,
                  "action_ordinal ${ordinal} is outside allowed range [1,${max}]",
                  ("ordinal", action_ordinal)("max", trace->action_traces.size())
      );
      return trace->action_traces[action_ordinal-1];
   }

   const action_trace& transaction_context::get_action_trace( uint32_t action_ordinal )const {
      EOS_ASSERT( 0 < action_ordinal && action_ordinal <= trace->action_traces.size() ,
                  transaction_exception,
                  "action_ordinal ${ordinal} is outside allowed range [1,${max}]",
                  ("ordinal", action_ordinal)("max", trace->action_traces.size())
      );
      return trace->action_traces[action_ordinal-1];
   }

   uint32_t transaction_context::schedule_action( const action& act, account_name receiver, bool context_free,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.emplace_back( *trace, act, receiver, context_free,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   uint32_t transaction_context::schedule_action( action&& act, account_name receiver, bool context_free,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.emplace_back( *trace, std::move(act), receiver, context_free,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   uint32_t transaction_context::schedule_action( uint32_t action_ordinal, account_name receiver, bool context_free,
                                                  uint32_t creator_action_ordinal,
                                                  uint32_t closest_unnotified_ancestor_action_ordinal )
   {
      uint32_t new_action_ordinal = trace->action_traces.size() + 1;

      trace->action_traces.reserve( std::bit_ceil(new_action_ordinal) ); // bit_ceil to avoid vector copy on every reserve call.

      const action& provided_action = get_action_trace( action_ordinal ).act;

      // The reserve above is required so that the emplace_back below does not invalidate the provided_action reference,
      // which references an action within the `trace->action_traces` vector we are appending to.

      trace->action_traces.emplace_back( *trace, provided_action, receiver, context_free,
                                         new_action_ordinal, creator_action_ordinal,
                                         closest_unnotified_ancestor_action_ordinal );

      return new_action_ordinal;
   }

   void transaction_context::execute_action( uint32_t action_ordinal, uint32_t recurse_depth ) {
      apply_context acontext( control, *this, action_ordinal, recurse_depth );

      if (recurse_depth == 0) {
         if (auto dm_logger = control.get_deep_mind_logger(is_transient())) {
            dm_logger->on_input_action();
         }
      }

      acontext.exec();
   }

   #ifdef ENABLE_DEFERRED_TRANSACTION
   void transaction_context::schedule_transaction() {
      // Charge ahead of time for the additional net usage needed to retire the delayed transaction
      // whether that be by successfully executing, soft failure, hard failure, or expiration.
      const transaction& trx = packed_trx.get_transaction();
      if( trx.delay_sec.value == 0 ) { // Do not double bill. Only charge if we have not already charged for the delay.
         const auto& cfg = control.get_global_properties().configuration;
         add_net_usage( static_cast<uint64_t>(cfg.base_per_transaction_net_usage)
                         + static_cast<uint64_t>(config::transaction_id_net_usage) ); // Will exit early if net usage cannot be payed.
      }

      auto first_auth = trx.first_authorizer();

      uint32_t trx_size = 0;
      const auto& cgto = control.mutable_db().create<generated_transaction_object>( [&]( auto& gto ) {
        gto.trx_id      = id;
        gto.payer       = first_auth;
        gto.sender      = account_name(); /// delayed transactions have no sender
        gto.sender_id   = transaction_id_to_sender_id( gto.trx_id );
        gto.published   = control.pending_block_time();
        gto.delay_until = gto.published + delay;
        gto.expiration  = gto.delay_until + fc::seconds(control.get_global_properties().configuration.deferred_trx_expiration_window);
        trx_size = gto.set( trx );

        if (auto dm_logger = control.get_deep_mind_logger(is_transient())) {
           std::string event_id = RAM_EVENT_ID("${id}", ("id", gto.id));

           dm_logger->on_create_deferred(deep_mind_handler::operation_qualifier::push, gto, packed_trx);
           dm_logger->on_ram_trace(std::move(event_id), "deferred_trx", "push", "deferred_trx_pushed");
        }
      });

      int64_t ram_delta = (config::billable_size_v<generated_transaction_object> + trx_size);
      add_ram_usage( cgto.payer, ram_delta );
      trace->trx_ram_delta = account_delta( cgto.payer, ram_delta );
   }
   #endif//ENABLE_DEFERRED_TRANSACTION

   void transaction_context::record_transaction( const transaction_id_type& id, fc::time_point_sec expire ) {
      try {
          control.mutable_db().create<transaction_object>([&](transaction_object& transaction) {
              transaction.trx_id = id;
              transaction.expiration = expire;
          });
      } catch( const boost::interprocess::bad_alloc& ) {
         throw;
      } catch ( ... ) {
          EOS_ASSERT( false, tx_duplicate,
                     "duplicate transaction ${id}", ("id", id ) );
      }
   } /// record_transaction

   void transaction_context::validate_referenced_accounts( const transaction& trx, bool enforce_actor_whitelist_blacklist )const {
      const auto& db = control.db();
      const auto& auth_manager = control.get_authorization_manager();

      if( !trx.context_free_actions.empty() && !control.skip_trx_checks() ) {
         for( const auto& a : trx.context_free_actions ) {
            auto* code = db.find<account_object, by_name>( a.account );
            EOS_ASSERT( code != nullptr, transaction_exception,
                        "action's code account '${account}' does not exist", ("account", a.account) );
            EOS_ASSERT( a.authorization.size() == 0, transaction_exception,
                        "context-free actions cannot have authorizations" );
         }
      }

      flat_set<account_name> actors;

      bool one_auth = false;
      for( const auto& a : trx.actions ) {
         auto* code = db.find<account_object, by_name>(a.account);
         EOS_ASSERT( code != nullptr, transaction_exception,
                     "action's code account '${account}' does not exist", ("account", a.account) );
         if ( is_read_only() ) {
            EOS_ASSERT( a.authorization.size() == 0, transaction_exception,
                       "read-only action '${name}' cannot have authorizations", ("name", a.name) );
         }
         for( const auto& auth : a.authorization ) {
            one_auth = true;
            auto* actor = db.find<account_object, by_name>(auth.actor);
            EOS_ASSERT( actor  != nullptr, transaction_exception,
                        "action's authorizing actor '${account}' does not exist", ("account", auth.actor) );
            EOS_ASSERT( auth_manager.find_permission(auth) != nullptr, transaction_exception,
                        "action's authorizations include a non-existent permission: ${permission}",
                        ("permission", auth) );
            if( enforce_actor_whitelist_blacklist )
               actors.insert( auth.actor );
         }
      }
      EOS_ASSERT( one_auth || is_read_only(), tx_no_auths, "transaction must have at least one authorization" );

      if( enforce_actor_whitelist_blacklist ) {
         control.check_actor_list( actors );
      }
   }

   int64_t transaction_context::set_proposed_producers(vector<producer_authority> producers) {
      if (producers.empty())
         return -1; // SAVANNA depends on DISALLOW_EMPTY_PRODUCER_SCHEDULE

      EOS_ASSERT(producers.size() <= config::max_proposers, wasm_execution_error,
                 "Producer schedule exceeds the maximum proposer count for this chain");

      trx_blk_context.proposed_schedule_block_num = control.head().block_num() + 1;
      // proposed_schedule.version is set in assemble_block
      trx_blk_context.proposed_schedule.producers = std::move(producers);

      return std::numeric_limits<uint32_t>::max();
   }

   void transaction_context::set_proposed_finalizers(finalizer_policy&& fin_pol) {
      trx_blk_context.proposed_fin_pol_block_num = control.head().block_num() + 1;
      trx_blk_context.proposed_fin_pol = std::move(fin_pol);
   }

} /// eosio::chain
