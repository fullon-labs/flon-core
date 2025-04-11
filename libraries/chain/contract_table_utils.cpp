#include <eosio/chain/contract_table_utils.hpp>

namespace eosio { namespace chain { namespace contract_table_utils {

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

   core_asset_account_ptr core_asset_account::create(const chainbase::database& db, const account_name& account) {
      const auto* tid_obj = table_id_finder::find( db, config::token_account_name, account, "accounts"_n );
      if (!tid_obj) return nullptr;

      auto kv_obj = key_value_finder::find(db, tid_obj, config::core_symbol_code.value );
      if (!kv_obj) return nullptr;

      auto data = token_account_data::unpack_from(*kv_obj);
      EOS_ASSERT( data.balance.get_symbol() == config::core_symbol,
                  tx_gas_exception,
                  "precision of core symbol ${sym} in token contract mismatch with config ${cfg_sym}",
                  ("sym", data.balance.get_symbol())("cfg_sym", config::core_symbol)
      );

      return std::make_shared<core_asset_account>(*kv_obj, std::move(data));
   }

   void core_asset_account::save(chainbase::database& db) {
      db.modify(table_obj, [&](auto& obj) {
         acct_data.pack_to(obj);
         // TODO: dmlog
      });
   }

} } }  // namespace eosio::chain::contract_table_utils

