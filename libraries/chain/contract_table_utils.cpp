#include <eosio/chain/contract_table_utils.hpp>

namespace eosio { namespace chain { namespace contract_table_utils {

   system_creator_account_ptr system_creator_account::create(const chainbase::database& db, const account_name& account) {
      const auto* kv_obj = key_value_finder::find(db, config::system_account_name,
                              config::system_account_name, "creators"_n, account );
      if (!kv_obj) return nullptr;

      return std::make_shared<system_creator_account>(
         fc::raw::unpack<system_creator_account>(kv_obj->value.data(), kv_obj->value.size())
      );
   }

   token_account_data token_account_data::unpack_from(const key_value_object& obj) {
      fc::datastream<const char*> ds(obj.value.data(), obj.value.size());
      token_account_data ret;
      fc::raw::unpack(ds, ret.balance);
      ret.remaining_data.resize(ds.remaining());
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
      const auto* kv_obj = key_value_finder::find(db, config::token_account_name,
               account, "accounts"_n, config::core_symbol_code.value );
      if (!kv_obj) {
         return nullptr;
      }

      if (kv_obj->value.size() < min_packed_size) {
         wlog("core asset account found, but the data size ${sz} is too small than min ${m}",
               ("sz", kv_obj->value.size())("m", min_packed_size));
         return nullptr;
      }

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

