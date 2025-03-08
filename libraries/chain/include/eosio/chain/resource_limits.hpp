#pragma once
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/types.hpp>
#include <eosio/chain/chain_config.hpp>
#include <eosio/chain/trace.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/chain/block_timestamp.hpp>
#include <chainbase/chainbase.hpp>
#include <set>

#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/asset.hpp>
#include <eosio/chain/core_symbol.hpp>
namespace eosio { namespace chain {

   class deep_mind_handler;

   namespace resource_limits {
   namespace impl {
      template<typename T>
      struct ratio {
         static_assert(std::is_integral<T>::value, "ratios must have integral types");
         T numerator;
         T denominator;

         friend inline bool operator ==( const ratio& lhs, const ratio& rhs ) {
            return std::tie(lhs.numerator, lhs.denominator) == std::tie(rhs.numerator, rhs.denominator);
         }

         friend inline bool operator !=( const ratio& lhs, const ratio& rhs ) {
            return !(lhs == rhs);
         }
      };
   }

   using ratio = impl::ratio<uint64_t>;

   struct elastic_limit_parameters {
      uint64_t target;           // the desired usage
      uint64_t max;              // the maximum usage
      uint32_t periods;          // the number of aggregation periods that contribute to the average usage

      uint32_t max_multiplier;   // the multiplier by which virtual space can oversell usage when uncongested
      ratio    contract_rate;    // the rate at which a congested resource contracts its limit
      ratio    expand_rate;       // the rate at which an uncongested resource expands its limits

      void validate()const; // throws if the parameters do not satisfy basic sanity checks

      friend inline bool operator ==( const elastic_limit_parameters& lhs, const elastic_limit_parameters& rhs ) {
         return std::tie(lhs.target, lhs.max, lhs.periods, lhs.max_multiplier, lhs.contract_rate, lhs.expand_rate)
                  == std::tie(rhs.target, rhs.max, rhs.periods, rhs.max_multiplier, rhs.contract_rate, rhs.expand_rate);
      }

      friend inline bool operator !=( const elastic_limit_parameters& lhs, const elastic_limit_parameters& rhs ) {
         return !(lhs == rhs);
      }
   };

   struct account_resource_limit {
      int64_t used = 0; ///< quantity used in current window
      int64_t available = 0; ///< quantity available in current window (based upon fractional reserve)
      int64_t max = 0; ///< max per window under current congestion
      block_timestamp_type last_usage_update_time; ///< last usage timestamp
      int64_t current_used = 0;  ///< current usage according to the given timestamp
   };

   class resource_limits_manager {
      public:

         explicit resource_limits_manager(chainbase::database& db, std::function<deep_mind_handler*(bool is_trx_transient)> get_deep_mind_logger)
         :_db(db),_get_deep_mind_logger(get_deep_mind_logger)
         {
         }

         void add_indices();
         void initialize_database(const chain_config& cfg);
         size_t expected_snapshot_row_count() const;
         void add_to_snapshot( const snapshot_writer_ptr& snapshot, snapshot_written_row_counter& row_counter ) const;
         void read_from_snapshot( const snapshot_reader_ptr& snapshot, std::atomic_size_t& read_row_count, boost::asio::io_context& ctx );

         void initialize_account( const account_name& account, bool is_trx_transient );
         void set_block_parameters( const chain_config& cfg, const elastic_limit_parameters& cpu_limit_parameters, const elastic_limit_parameters& net_limit_parameters );

         // void update_account_usage( const flat_set<account_name>& accounts, uint32_t ordinal );
         void add_transaction_usage( transaction_gas_usage& trx_gas_usage, bool is_trx_transient = false );
         void calc_transaction_gas_usage( transaction_gas_usage& trx_gas_usage);
         void verify_transaction_gas_usage( transaction_gas_usage& trx_gas_usage, uint64_t reserved_gas, uint64_t convertible_gas);

         void add_ram_usage( const account_name account, int64_t ram_delta, bool is_trx_transient = false );

         void set_account_limits( const account_name& account, uint64_t gas, bool is_unlimited, bool is_trx_transient);

         void get_account_limits( const account_name& account, uint64_t& gas, bool& is_unlimited ) const;
         uint64_t get_account_convertible_gas( const account_name& account ) const;
         uint64_t get_account_gas_max( const account_name& account, uint64_t reserved_gas ) const;
         uint64_t get_account_gas( const account_name& account) const;

         bool is_account_unlimited( const account_name& account ) const;

         void process_block_usage( uint32_t block_num );

         // accessors
         uint64_t get_total_cpu_weight() const;
         uint64_t get_total_net_weight() const;

         uint64_t get_virtual_block_cpu_limit() const;
         uint64_t get_virtual_block_net_limit() const;

         uint64_t get_block_cpu_limit() const;
         uint64_t get_block_net_limit() const;

         uint64_t get_account_cpu_limit( const account_name& name) const;
         uint64_t get_account_net_limit( const account_name& name) const;


         int64_t get_account_ram_usage( const account_name& name ) const;

         uint64_t convert_cpu_to_gas(uint64_t gas);
         uint64_t convert_net_to_gas(uint64_t gas);
         uint64_t convert_gas_to_cpu(uint64_t gas);
         uint64_t convert_gas_to_net(uint64_t gas);
      private:
         chainbase::database&         _db;
         std::function<deep_mind_handler*(bool is_trx_transient)> _get_deep_mind_logger;
   };


   struct core_asset_account;
   using core_asset_account_ptr = std::shared_ptr<core_asset_account>;
   struct token_account_data {
      asset                   balance;
      vector<char>            remaining_data;

      static token_account_data unpack_from(const key_value_object& obj);

      void pack_to(key_value_object& obj);
   };

   struct core_asset_account {
      const key_value_object&    table_obj;
      token_account_data         acct_data;

      core_asset_account( const key_value_object& table_obj, token_account_data acct_data)
      :table_obj(table_obj), acct_data(acct_data) {}

      static core_asset_account_ptr create(chainbase::database& db, const account_name& account);

      void save(chainbase::database& db);

      inline const asset& balance() const {
         return acct_data.balance;
      }
      inline asset& balance() {
         return acct_data.balance;
      }

   };

} } } /// eosio::chain

FC_REFLECT( eosio::chain::resource_limits::account_resource_limit, (used)(available)(max)(last_usage_update_time)(current_used) )
FC_REFLECT( eosio::chain::resource_limits::ratio, (numerator)(denominator))
FC_REFLECT( eosio::chain::resource_limits::elastic_limit_parameters, (target)(max)(periods)(max_multiplier)(contract_rate)(expand_rate))
