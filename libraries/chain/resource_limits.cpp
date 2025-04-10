#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/resource_limits_private.hpp>
#include <eosio/chain/transaction_metadata.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/deep_mind.hpp>
#include <boost/tuple/tuple_io.hpp>
#include <eosio/chain/database_utils.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <algorithm>

namespace eosio { namespace chain {

namespace calc_utils {
   void verify_add(uint64_t a, uint64_t b, const char* description) {
      EOS_ASSERT( std::numeric_limits<uint64_t>::max() - a >= b,
                  calc_overflow_exception,
                  std::string("Overflow when adding(uint64) ") + description);
   }

   void verify_add(int64_t a, int64_t b, const char* description) {
      if (a > 0 && b > 0) {
         EOS_ASSERT( std::numeric_limits<int64_t>::max() - a >= b,
                     calc_overflow_exception,
                     std::string("Overflow when adding(int64 positive) ") + description);
      } else if (a < 0 && b < 0) {
         EOS_ASSERT( std::numeric_limits<int64_t>::min() - a <= b,
                     calc_overflow_exception,
                     std::string("Underflow when adding(int64 negative) of ") + description);
      }
   }

   void verify_multiply(uint64_t a, uint64_t b, const char* description) {
      if (a != 0 && b != 0) {
         EOS_ASSERT( std::numeric_limits<uint64_t>::max() / a >= b,
                     calc_overflow_exception,
                     std::string("Overflow when multiplying(uint64) ") + description);
      }
   }
}

namespace resource_limits {

using resource_index_set = index_set<
   resource_limits_index,
   resource_usage_index,
   resource_limits_state_index,
   resource_limits_config_index
>;

static_assert( config::rate_limiting_precision > 0, "config::rate_limiting_precision must be positive" );

namespace res_utils {

   void verify_delta_converting(uint64_t a, const char* description) {
      EOS_ASSERT( a <= std::numeric_limits<int64_t>::max(),
                  calc_overflow_exception,
                  std::string("Overflow when ") + description);
   }

   void verify_sub_core_asset(const asset& a, const asset& b, const char* description) {
      EOS_ASSERT( a.get_amount() >= 0 && a >= b,
         substraction_insufficent_exception,
                  std::string("Insufficent when substracting ") + description);
   }

   void verify_add_core_asset(const asset& a, const asset& b, const char* description) {
      EOS_ASSERT( a.get_amount() >= 0 && b.get_amount() >= 0 &&
                  std::numeric_limits<int64_t>::max() - a.get_amount() >= b.get_amount() ,
                  calc_overflow_exception,
                  std::string("Overflow when adding ") + description);
   }

   uint64_t calc_transaction_gas_usage(  transaction_res_usage& res_usage, const resource_limits_config_object& config );


   template<typename UInt, typename CalcUInt>
   UInt multiply_decimal(UInt a, UInt b, UInt precision, const char* description = nullptr) {
      if (a == 0 ||  b == 0 || precision == 0) return 0;

      static_assert(sizeof(CalcUInt) > sizeof(UInt));
      CalcUInt tmp = a * b / precision;
      EOS_ASSERT( tmp <= (CalcUInt)std::numeric_limits<UInt>::max(),
                  calc_overflow_exception,
                  std::string("multiply_decimal overflow when ") + (description ? description : ""));
      return tmp;
   }

   template<typename UInt, typename CalcUInt>
   UInt multiply_decimal_ceil(UInt a, UInt b, UInt precision, const char* description = nullptr) {
      if (a == 0 || b == 0 || precision == 0) return 0;

      static_assert(sizeof(CalcUInt) > sizeof(UInt));
      CalcUInt tmp = 10 * a * b / precision;
      EOS_ASSERT( tmp <= (CalcUInt)std::numeric_limits<UInt>::max(),
                  calc_overflow_exception,
                  std::string("multiply_decimal_ceil overflow when ") + (description ? description : ""));
      return (tmp + 9) / 10; // ceil
   }

   #define MULTIPLY_DECIMAL_U64        multiply_decimal<uint64_t, uint128_t>
   #define MULTIPLY_DECIMAL_CEIL_U64   multiply_decimal_ceil<uint64_t, uint128_t>

   uint64_t convert_cpu_to_gas(const resource_limits_config_object& config,  uint64_t value) {
      return MULTIPLY_DECIMAL_CEIL_U64(value, (uint64_t)config.gas_per_cpu_ms, (uint64_t)config::gas_rate_precision,
                                       "converting cpu us to gas");
   }
   uint64_t convert_net_to_gas(const resource_limits_config_object& config, uint64_t value) {
      return MULTIPLY_DECIMAL_CEIL_U64(value, config.gas_per_net_kb, config::gas_rate_precision,
                                       "converting net bytes to gas");
   }

   uint64_t convert_ram_to_gas(const resource_limits_config_object& config, uint64_t value) {
      return MULTIPLY_DECIMAL_CEIL_U64(value, config.gas_per_ram_kb, config::gas_rate_precision,
                                       "converting ram bytes to gas");
   }

   uint64_t convert_gas_to_cpu(const resource_limits_config_object& config, uint64_t gas) {
      return MULTIPLY_DECIMAL_U64(gas, config.gas_per_cpu_ms, config::gas_rate_precision,
         "converting gas to cpu us");
   }

   uint64_t convert_gas_to_net(const resource_limits_config_object& config, uint64_t gas) {
      return MULTIPLY_DECIMAL_U64(gas, config.gas_per_net_kb, config::gas_rate_precision,
         "converting gas to net bytes");
   }

   uint64_t convert_gas_to_ram(const resource_limits_config_object& config, uint64_t gas) {
      return MULTIPLY_DECIMAL_U64(gas, config.gas_per_ram_kb, config::gas_rate_precision,
         "converting gas to ram bytes");
   }

   asset convert_gas_to_core_asset(uint64_t gas) {
      // The conversion rate of gas to core asset(ELON) is 1:1
      EOS_ASSERT( gas <= (uint64_t)std::numeric_limits<int64_t>::max(),
               calc_overflow_exception,
               "Overflow when converting gas to core asset!");
      return asset(gas, config::core_symbol);
   }

   uint64_t convert_core_asset_to_gas(int64_t core_asset_amount) {
      // The conversion rate of core asset(ELON) to gas is 1:1
      return core_asset_amount > 0 ? core_asset_amount : 0;
   }

   uint64_t convert_core_asset_to_gas(const asset& core_asset) {
      return convert_core_asset_to_gas(core_asset.get_amount());
   }

   const resource_limits_object& get_account_limits( const chainbase::database& db, const account_name& account ) {
      return db.get<resource_limits_object,by_owner>( account );
   }

   void set_account_reserved_gas(chainbase::database& db, const resource_limits_object& acc_limits, uint64_t gas, deep_mind_handler* dm_logger) {
      if (acc_limits.gas == gas) return;

      db.modify( acc_limits, [&]( resource_limits_object& rlo ){
         rlo.gas = gas;
         if (dm_logger) {
            dm_logger->on_set_account_limits(rlo);
         }
      });
   }
} // namespace res_utils

struct core_gas_accessor;
using core_gas_accessor_ptr = std::shared_ptr<core_gas_accessor>;
struct core_gas_accessor {
   core_asset_account_ptr sys_gas_account;
   core_asset_account_ptr payer_gas_account;
   uint64_t convertible_gas   = 0;

   static core_gas_accessor_ptr create(chainbase::database& db, const account_name& payer);
   void pay_gas(chainbase::database& db, uint64_t gas);
};

static uint64_t update_elastic_limit(uint64_t current_limit, uint64_t average_usage, const elastic_limit_parameters& params) {
   uint64_t result = current_limit;
   if (average_usage > params.target ) {
      result = result * params.contract_rate;
   } else {
      result = result * params.expand_rate;
   }
   return std::min(std::max(result, params.max), params.max * params.max_multiplier);
}

void elastic_limit_parameters::validate()const {
   // At the very least ensure parameters are not set to values that will cause divide by zero errors later on.
   // Stricter checks for sensible values can be added later.
   EOS_ASSERT( periods > 0, resource_limit_exception, "elastic limit parameter 'periods' cannot be zero" );
   EOS_ASSERT( contract_rate.denominator > 0, resource_limit_exception, "elastic limit parameter 'contract_rate' is not a well-defined ratio" );
   EOS_ASSERT( expand_rate.denominator > 0, resource_limit_exception, "elastic limit parameter 'expand_rate' is not a well-defined ratio" );
}


void resource_limits_state_object::update_virtual_cpu_limit( const resource_limits_config_object& cfg ) {
   //idump((average_block_cpu_usage.average()));
   virtual_cpu_limit = update_elastic_limit(virtual_cpu_limit, average_block_cpu_usage.average(), cfg.cpu_limit_parameters);
   //idump((virtual_cpu_limit));
}

void resource_limits_state_object::update_virtual_net_limit( const resource_limits_config_object& cfg ) {
   virtual_net_limit = update_elastic_limit(virtual_net_limit, average_block_net_usage.average(), cfg.net_limit_parameters);
}

void resource_limits_manager::add_indices() {
   resource_index_set::add_indices(_db);
}

void resource_limits_manager::initialize_database(const chain_config& cfg) {
   const auto& config = _db.create<resource_limits_config_object>([&cfg](resource_limits_config_object& c){
      // see default settings in the declaration
      c.gas_per_cpu_ms = cfg.gas_per_cpu_ms;
      c.gas_per_net_kb = cfg.gas_per_net_kb;
      c.gas_per_ram_kb = cfg.gas_per_ram_kb;
   });

   const auto& state = _db.create<resource_limits_state_object>([&config](resource_limits_state_object& state){
      // see default settings in the declaration

      // start the chain off in a way that it is "congested" aka slow-start
      state.virtual_cpu_limit = config.cpu_limit_parameters.max;
      state.virtual_net_limit = config.net_limit_parameters.max;

   });

   // At startup, no transaction specific logging is possible
   if (auto dm_logger = _get_deep_mind_logger(false)) {
      dm_logger->on_init_resource_limits(config, state);
   }
}

size_t resource_limits_manager::expected_snapshot_row_count() const {
   size_t ret = 0;
   resource_index_set::walk_indices([this, &ret]( auto utils ) {
      ret += _db.get_index<typename decltype(utils)::index_t>().size();
   });
   return ret;
}

void resource_limits_manager::add_to_snapshot( const snapshot_writer_ptr& snapshot, snapshot_written_row_counter& row_counter ) const {
   resource_index_set::walk_indices([this, &snapshot, &row_counter]( auto utils ){
      snapshot->write_section<typename decltype(utils)::index_t::value_type>([this, &row_counter]( auto& section ){
         decltype(utils)::walk(_db, [this, &section, &row_counter]( const auto &row ) {
            section.add_row(row, _db);
            row_counter.progress();
         });
      });
   });
}

void resource_limits_manager::read_from_snapshot( const snapshot_reader_ptr& snapshot, std::atomic_size_t& read_row_count, boost::asio::io_context& ctx ) {
   resource_index_set::walk_indices_via_post(ctx, [this, &snapshot, &read_row_count]( auto utils ){
      snapshot->read_section<typename decltype(utils)::index_t::value_type>([this, &read_row_count]( auto& section ) {
         bool more = !section.empty();
         while(more) {
            decltype(utils)::create(_db, [this, &section, &more]( auto &row ) {
               more = section.read_row(row, _db);
            });
            read_row_count.fetch_add(1u, std::memory_order_relaxed);
         }
      });
   });
}

void resource_limits_manager::initialize_account(const account_name& account, bool is_trx_transient) {
   const auto& limits = _db.create<resource_limits_object>([&]( resource_limits_object& bl ) {
      bl.owner = account;
   });

   const auto& usage = _db.create<resource_usage_object>([&]( resource_usage_object& bu ) {
      bu.owner = account;
   });
   if (auto dm_logger = _get_deep_mind_logger(is_trx_transient)) {
      dm_logger->on_newaccount_resource_limits(limits, usage);
   }
}

void resource_limits_manager::set_block_parameters(const chain_config& cfg, const elastic_limit_parameters& cpu_limit_parameters, const elastic_limit_parameters& net_limit_parameters ) {
   cpu_limit_parameters.validate();
   net_limit_parameters.validate();
   const auto& config = _db.get<resource_limits_config_object>();
   if( config.cpu_limit_parameters == cpu_limit_parameters && config.net_limit_parameters == net_limit_parameters )
      return;
   _db.modify(config, [&](resource_limits_config_object& c){
      c.cpu_limit_parameters = cpu_limit_parameters;
      c.net_limit_parameters = net_limit_parameters;
      c.gas_per_cpu_ms = cfg.gas_per_cpu_ms;
      c.gas_per_net_kb = cfg.gas_per_net_kb;
      c.gas_per_ram_kb = cfg.gas_per_ram_kb;

      // set_block_parameters is called by controller::finish_block,
      // where transaction specific logging is not possible
      if (auto dm_logger = _get_deep_mind_logger(false)) {
         dm_logger->on_update_resource_limits_config(c);
      }
   });
}

void resource_limits_manager::add_transaction_usage(transaction_res_usage& res_usage, std::optional<account_gas_trace>& gas_trace, bool is_trx_transient ) {

   const auto& state = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();

   const auto& usage = _db.get<resource_usage_object,by_owner>( res_usage.payer );
   const auto& cpu_usage = res_usage.cpu_usage;
   const auto& net_usage = res_usage.net_usage;

   uint64_t used_gas = 0;
   const auto& acc_limits = res_utils::get_account_limits(_db, res_usage.payer);
   if (!acc_limits.is_unlimited) {
      core_gas_accessor_ptr cgs;

      uint64_t reserved_gas      = acc_limits.gas;
      uint64_t convertible_gas   = 0;

      used_gas = res_utils::calc_transaction_gas_usage(res_usage, config);

      if ( used_gas > reserved_gas ) {
         cgs = core_gas_accessor::create(_db, res_usage.payer);
         convertible_gas = cgs->convertible_gas;
      }

      calc_utils::verify_add(acc_limits.gas, convertible_gas, "reserved gas and convertible gas of transaction payer");
      uint64_t gas_limit = acc_limits.gas + convertible_gas;
      // verify transaction gas
      EOS_ASSERT( used_gas <= gas_limit,
         tx_gas_usage_exceeded,
         "authorizing account '${n}' has insufficient gas for cpu and net usage of this transaction ,"
         "needs gas ${used_gas} , but has available gas ${gas}",
         ("n", res_usage.payer)
         ("cpu_usage", res_usage.cpu_usage)
         ("net_usage", res_usage.net_usage)
         ("used_gas", used_gas)
         ("cpu_gas", res_usage.cpu_gas)
         ("net_gas", res_usage.net_gas)
         ("gas", gas_limit)
         ("reserved_gas", reserved_gas)
         ("convertible_gas", convertible_gas)
      );

      gas_trace.emplace(res_usage.payer);
      gas_trace->reserved_gas_before = reserved_gas;
      if ( used_gas > reserved_gas ) {
         assert(cgs);
         gas_trace->converted_gas = used_gas - reserved_gas;
         cgs->pay_gas(_db, gas_trace->converted_gas);
         reserved_gas = 0; // reserved_gas must be used up.
      } else {// used_gas <= reserved_gas
         reserved_gas -= used_gas;
      }
      gas_trace->reserved_gas_after = reserved_gas;
      gas_trace->used_gas = used_gas;

      res_utils::set_account_reserved_gas(_db, acc_limits, reserved_gas, _get_deep_mind_logger(is_trx_transient));
   }

   calc_utils::verify_add(usage.cpu_usage, cpu_usage, "new cpu usage to existed of transaction payer");
   calc_utils::verify_add(usage.net_usage, net_usage, "new net usage to existed of transaction payer");

   _db.modify( usage, [&]( auto& bu ){
      bu.net_usage += net_usage;
      bu.cpu_usage += net_usage;

      if (auto dm_logger = _get_deep_mind_logger(is_trx_transient)) {
         dm_logger->on_update_account_usage(bu);
      }
   });

   calc_utils::verify_add(state.pending_cpu_usage, cpu_usage, "new cpu usage to pending cpu usage of block");
   calc_utils::verify_add(state.pending_net_usage, net_usage, "new net usage to pending net usage of block");

   calc_utils::verify_add(state.total_cpu_usage, cpu_usage, "new cpu usage to total cpu usage");
   calc_utils::verify_add(state.total_net_usage, net_usage, "new net usage to total net usage");

   // account for this transaction in the block and do not exceed those limits either
   _db.modify(state, [&](resource_limits_state_object& rls){
      rls.pending_cpu_usage += cpu_usage;
      rls.pending_net_usage += net_usage;

      rls.total_cpu_usage += cpu_usage;
      rls.total_net_usage += net_usage;
   });

   EOS_ASSERT( state.pending_cpu_usage <= config.cpu_limit_parameters.max, block_resource_exhausted, "Block has insufficient cpu resources" );
   EOS_ASSERT( state.pending_net_usage <= config.net_limit_parameters.max, block_resource_exhausted, "Block has insufficient net resources" );
}

uint64_t res_utils::calc_transaction_gas_usage( transaction_res_usage& res_usage,
                                                const resource_limits_config_object& config )
{
   // must set the res_usage.cpu_usage and res_usage.net_usage before here
   res_usage.cpu_gas = res_utils::convert_cpu_to_gas(config, res_usage.cpu_usage);
   res_usage.net_gas = res_utils::convert_net_to_gas(config, res_usage.net_usage);

   calc_utils::verify_add(res_usage.cpu_gas, res_usage.net_gas, "cpu gas and net gas of transaction payer");
   return res_usage.cpu_gas + res_usage.net_gas;
}

void resource_limits_manager::calc_transaction_gas_usage( transaction_res_usage& res_usage) {
   const auto& config = _db.get<resource_limits_config_object>();
   res_utils::calc_transaction_gas_usage(res_usage, config);
}

void resource_limits_manager::verify_transaction_gas_usage( transaction_res_usage& res_usage, uint64_t reserved_gas, uint64_t convertible_gas) {

   calc_utils::verify_add(res_usage.cpu_gas, res_usage.net_gas, "cpu gas and net gas of transaction payer");
   calc_utils::verify_add(reserved_gas, convertible_gas, "cpu gas and net gas of transaction payer");
   uint64_t used_gas = res_usage.cpu_gas + res_usage.net_gas;
   uint64_t gas_limit = reserved_gas + convertible_gas;

   EOS_ASSERT( used_gas <= reserved_gas + convertible_gas,
               tx_gas_usage_exceeded,
               "authorizing account '${n}' has insufficient gas for cpu usage ${cpu_usage} and net usage ${net_usage} of this transaction,"
               "needs gas ${used_gas} , but has available gas ${gas}",
               ("n", res_usage.payer)
               ("cpu_usage", res_usage.cpu_usage)
               ("net_usage", res_usage.net_usage)
               ("used_gas", used_gas)
               ("cpu_gas", res_usage.cpu_gas)
               ("net_gas", res_usage.net_gas)
               ("gas", gas_limit)
               ("reserved_gas", reserved_gas)
               ("convertible_gas", convertible_gas)
   );
}

void resource_limits_manager::add_ram_usage( const account_name account, int64_t ram_delta, std::optional<account_gas_trace>& gas_trace, bool is_trx_transient ) {
   if (ram_delta == 0) {
      return;
   }

   const auto& usage  = _db.get<resource_usage_object,by_owner>( account );
   if (ram_delta > 0) {
      calc_utils::verify_add(usage.ram_usage, (uint64_t)ram_delta, "delta to existed ram usage of account");
   } else { // ram_delta < 0
      EOS_ASSERT( usage.ram_usage >= (uint64_t)(-ram_delta), transaction_exception,
                 "Ram usage insufficent when substracting delta");
   }

   const auto& config = _db.get<resource_limits_config_object>();
   const auto& acc_limits = res_utils::get_account_limits(_db, account);

   if (!acc_limits.is_unlimited) {
      gas_trace.emplace(account);
      gas_trace->reserved_gas_before = acc_limits.gas;
      gas_trace->ram_gas_delta.ram_delta = ram_delta;
      if (ram_delta > 0) {
         auto used_gas = res_utils::convert_ram_to_gas(config, ram_delta);

         core_gas_accessor_ptr cgs;

         uint64_t reserved_gas      = acc_limits.gas;
         uint64_t convertible_gas   = 0;

         if ( used_gas > reserved_gas ) {
            cgs = core_gas_accessor::create(_db, account);
            if (cgs) {
               convertible_gas = cgs->convertible_gas;
            }
         }

         calc_utils::verify_add(reserved_gas, convertible_gas, "reserved gas and convertible gas of ram account");
         uint64_t gas_limit = reserved_gas + convertible_gas;
         // verify transaction gas
         EOS_ASSERT( used_gas <= gas_limit,
            tx_gas_usage_exceeded,
            "authorizing account '${n}' has insufficient gas for ram usage, "
            "needs gas ${used_gas} , but has available gas ${gas}",
            ("n", account)
            ("ram_usage", ram_delta)
            ("used_gas", used_gas)
            ("gas", gas_limit)
            ("reserved_gas", reserved_gas)
            ("convertible_gas", convertible_gas)
         );

         if ( cgs != nullptr && convertible_gas > 0 ) {
            assert(cgs);
            gas_trace->converted_gas = used_gas - reserved_gas;
            cgs->pay_gas(_db, gas_trace->converted_gas);
            reserved_gas = 0; // reserved_gas must be used up.
         } else {// used_gas <= reserved_gas
            reserved_gas -= used_gas;
         }

         res_utils::set_account_reserved_gas(_db, acc_limits, reserved_gas, _get_deep_mind_logger(is_trx_transient));

         res_utils::verify_delta_converting(used_gas, "converting ram gas decreased delta");
         gas_trace->used_gas                 = used_gas;
         gas_trace->ram_gas_delta.gas_delta  = -used_gas; // sub gas
      } else { // ram_delta < 0
         auto ram_gas = res_utils::convert_ram_to_gas(config, (uint64_t)(-ram_delta));
         calc_utils::verify_add(acc_limits.gas, ram_gas, "refunded ram gas to gas of account");
         res_utils::set_account_reserved_gas(_db, acc_limits, acc_limits.gas + ram_gas, _get_deep_mind_logger(is_trx_transient));
         res_utils::verify_delta_converting(ram_gas, "converting ram gas increased delta");
         gas_trace->ram_gas_delta.gas_delta = ram_gas; // add gas
      }
      gas_trace->reserved_gas_after = acc_limits.gas;
   }

   // modify ram_usage
   _db.modify( usage, [&]( auto& u ) {
      u.ram_usage += ram_delta;

      if (auto dm_logger = _get_deep_mind_logger(is_trx_transient)) {
         dm_logger->on_ram_event(account, u.ram_usage, ram_delta);
      }
   });


   const auto& state = _db.get<resource_limits_state_object>();
   if (ram_delta > 0) {
      calc_utils::verify_add(state.total_ram_usage, (uint64_t)ram_delta, "delta to total ram usage");
   } else { // ram_delta < 0
      EOS_ASSERT( state.total_ram_usage >= (uint64_t)(-ram_delta), transaction_exception,
                 "Total ram usage insufficent when substracting delta");
   }

   // account for this transaction in the block and do not exceed those limits either
   _db.modify(state, [&](resource_limits_state_object& rls){
      rls.total_ram_usage += ram_delta;
   });

   const auto& chain_config = _db.get<global_property_object>().configuration;
   EOS_ASSERT( state.total_ram_usage <= chain_config.max_total_ram_usage, resource_exhausted_exception,
               "Total ram usage ${t} exceeds the max limit ${max}",
               ("t", state.total_ram_usage)("max", chain_config.max_total_ram_usage));
}

int64_t resource_limits_manager::get_account_ram_usage( const account_name& name )const {
   return _db.get<resource_usage_object,by_owner>( name ).ram_usage;
}

void resource_limits_manager::set_account_limits( const account_name& account, uint64_t gas, bool is_unlimited, bool is_trx_transient) {

   // make sure the system gas account must be resource unlimited, so that it does not need to pay GAS
   EOS_ASSERT( account != config::gas_account_name, resource_limit_exception, "can not set the resource limits of account ${a}", ("a", config::gas_account_name) );
   const auto& limits = _db.get<resource_limits_object, by_owner>( account );
   _db.modify( limits, [&]( resource_limits_object& rlo ){
      rlo.gas = gas;
      rlo.is_unlimited = is_unlimited;

      if (auto dm_logger = _get_deep_mind_logger(is_trx_transient)) {
         dm_logger->on_set_account_limits(rlo);
      }
   });
}

void resource_limits_manager::get_account_limits( const account_name& account, uint64_t& gas, bool& is_unlimited ) const {
   const auto& rlo = res_utils::get_account_limits(_db, account);
   gas = rlo.gas;
   is_unlimited = rlo.is_unlimited;
}

uint64_t resource_limits_manager::get_account_convertible_gas( const account_name& account ) const {
   auto payer_gas_account = core_asset_account::create(_db, account);
   if (payer_gas_account && payer_gas_account->balance().get_amount() > 0) {
      auto sys_gas_account = core_asset_account::create(_db, config::gas_account_name);
      if (sys_gas_account) {
         return res_utils::convert_core_asset_to_gas(payer_gas_account->balance());
      }
   }
   return 0;
}

uint64_t resource_limits_manager::get_account_gas_max( const account_name& account, uint64_t reserved_gas ) const {
   uint64_t convertible_gas = get_account_convertible_gas(account);
   calc_utils::verify_add(reserved_gas, convertible_gas, "reserved gas and convertible gas of getting account");
   return reserved_gas + convertible_gas;
}

uint64_t resource_limits_manager::get_account_gas( const account_name& account) const {
   return res_utils::get_account_limits(_db, account).gas;
}

bool resource_limits_manager::is_account_unlimited( const account_name& account ) const {
   return res_utils::get_account_limits(_db, account).is_unlimited;
}

void resource_limits_manager::process_block_usage(uint32_t block_num) {
   const auto& s = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();
   _db.modify(s, [&](resource_limits_state_object& state){
      // apply pending usage, update virtual limits and reset the pending

      state.average_block_cpu_usage.add(state.pending_cpu_usage, block_num, config.cpu_limit_parameters.periods);
      state.update_virtual_cpu_limit(config);
      state.pending_cpu_usage = 0;

      state.average_block_net_usage.add(state.pending_net_usage, block_num, config.net_limit_parameters.periods);
      state.update_virtual_net_limit(config);
      state.pending_net_usage = 0;

      // process_block_usage is called by controller::finish,
      // where transaction specific logging is not possible
      if (auto dm_logger = _get_deep_mind_logger(false)) {
         dm_logger->on_update_resource_limits_state(state);
      }
   });

}

uint64_t resource_limits_manager::get_total_cpu_usage() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.total_cpu_usage;
}

uint64_t resource_limits_manager::get_total_net_usage() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.total_net_usage;
}

uint64_t resource_limits_manager::get_total_ram_usage() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.total_ram_usage;
}

uint64_t resource_limits_manager::get_virtual_block_cpu_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.virtual_cpu_limit;
}

uint64_t resource_limits_manager::get_virtual_block_net_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   return state.virtual_net_limit;
}

uint64_t resource_limits_manager::get_block_cpu_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();
   return config.cpu_limit_parameters.max - state.pending_cpu_usage;
}

uint64_t resource_limits_manager::get_block_net_limit() const {
   const auto& state = _db.get<resource_limits_state_object>();
   const auto& config = _db.get<resource_limits_config_object>();
   return config.net_limit_parameters.max - state.pending_net_usage;
}

uint64_t resource_limits_manager::get_account_cpu_limit( const account_name& account) const {
   const auto& rlo = res_utils::get_account_limits(_db, account);
   if (!rlo.is_unlimited) {
      auto gas = get_account_gas_max(account, rlo.gas);
      return convert_gas_to_cpu(gas);
   } else {
      return std::numeric_limits<int64_t>::max();
   }
}

uint64_t resource_limits_manager::get_account_net_limit( const account_name& account) const {
   const auto& rlo = res_utils::get_account_limits(_db, account);
   if (!rlo.is_unlimited) {
      auto gas = get_account_gas_max(account, rlo.gas);
      return convert_gas_to_cpu(gas);
   } else {
      return std::numeric_limits<int64_t>::max();
   }
}

uint64_t resource_limits_manager::convert_cpu_to_gas(uint64_t cpu) const {
   const auto& config = _db.get<resource_limits_config_object>();
   return res_utils::convert_cpu_to_gas(config, cpu);

}
uint64_t resource_limits_manager::convert_net_to_gas(uint64_t net) const {
   const auto& config = _db.get<resource_limits_config_object>();
   return res_utils::convert_net_to_gas(config, net);

}

uint64_t resource_limits_manager::convert_gas_to_cpu(uint64_t gas) const {
   const auto& config = _db.get<resource_limits_config_object>();
   return res_utils::convert_gas_to_cpu(config, gas);
}

uint64_t resource_limits_manager::convert_gas_to_net(uint64_t gas) const {
   const auto& config = _db.get<resource_limits_config_object>();
   return res_utils::convert_gas_to_net(config, gas);
}

token_account_data token_account_data::unpack_from(const key_value_object& obj) {
   fc::datastream<const char*> ds(obj.value.data(), obj.value.size());
   token_account_data ret;
   fc::raw::unpack(ds, ret.balance);
   ds.read(ret.remaining_data.data(), ret.remaining_data.size());
   return ret;
}

void token_account_data::pack_to(key_value_object& obj) {
   // Should not process the payer of ram
   size_t sz = fc::raw::pack_size( balance ) + remaining_data.size();
   obj.value.resize_and_fill( sz, [&](char* data, std::size_t size) {
      fc::datastream<char*> ds( data, size );
      fc::raw::pack( ds, balance );
      ds.write(remaining_data.data(), remaining_data.size());
   });
}

core_asset_account_ptr core_asset_account::create(chainbase::database& db, const account_name& account) {
   const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::token_account_name, account, "accounts"_n ));
   if (!t_id) return nullptr;

   const auto &idx = db.get_index<key_value_index, by_scope_primary>();

   auto itr = idx.find(boost::make_tuple( t_id->id, config::core_symbol_code.value ));
   if (itr == idx.end()) return nullptr;

   const key_value_object& obj = *itr;
   auto data = token_account_data::unpack_from(obj);
   EOS_ASSERT( data.balance.get_symbol() == config::core_symbol,
               tx_gas_exception,
               "precision of core symbol ${sym} in token contract mismatch with config ${cfg_sym}",
               ("sym", data.balance.get_symbol())("cfg_sym", config::core_symbol)
   );

   return std::make_shared<core_asset_account>(obj, std::move(data));
}

void core_asset_account::save(chainbase::database& db) {
   db.modify(table_obj, [&](auto& obj) {
      acct_data.pack_to(obj);
      // TODO: dmlog
   });
}

core_gas_accessor_ptr core_gas_accessor::create(chainbase::database& db, const account_name& payer) {

   core_asset_account_ptr sys_gas_account;
   core_asset_account_ptr payer_gas_account;
   uint64_t convertible_gas   = 0;
   payer_gas_account = core_asset_account::create(db, payer);
   if (!payer_gas_account) return nullptr;

   convertible_gas = res_utils::convert_core_asset_to_gas(payer_gas_account->balance());
   if (convertible_gas == 0) return nullptr;

   sys_gas_account = core_asset_account::create(db, config::gas_account_name);
   // if the core asset account of system gas not exists,
   // can not transfer core asset of converted_gas to system gas account
   if (!sys_gas_account) return nullptr;

   return std::make_shared<core_gas_accessor>(sys_gas_account, payer_gas_account, convertible_gas);
}

void core_gas_accessor::pay_gas(chainbase::database& db, uint64_t gas) {
   assert( payer_gas_account && sys_gas_account && convertible_gas > 0 );
   auto quant = res_utils::convert_gas_to_core_asset(gas);
   // transfer core asset of converted_gas from payer to system gas account
   res_utils::verify_sub_core_asset(payer_gas_account->balance(), quant, "core asset of converted gas from payer account");
   res_utils::verify_add_core_asset(sys_gas_account->balance(), quant, "core asset of converted gas to system gas account");
   payer_gas_account->balance() -= quant;
   sys_gas_account->balance() += quant;
   payer_gas_account->save(db);
   sys_gas_account->save(db);
}

} } } /// eosio::chain::resource_limits
