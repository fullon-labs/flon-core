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

} } }  // namespace eosio::chain::contract_table_utils

