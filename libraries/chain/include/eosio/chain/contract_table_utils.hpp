#pragma once

#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/asset.hpp>

#include <vector>
#include <memory>

namespace eosio { namespace chain { namespace contract_table_utils {



   struct table_id_finder {
      static inline const table_id_object* find( const chainbase::database& db,
                                          const account_name&        code,
                                          const scope_name&          scope,
                                          const table_name&          table)
      {
         auto itr = db.find<chain::table_id_object, chain::by_code_scope_table>(
                  boost::make_tuple( code, scope, table ));
         return itr ? &(*itr) : nullptr;
      }
   };

   struct key_value_finder {
      static inline const key_value_object* find(const chainbase::database& db, const table_id_object* tid_obj, uint64_t pk) {
         if (!tid_obj) return nullptr;

         const auto &idx = db.get_index<key_value_index, by_scope_primary>();
         auto itr = idx.find(boost::make_tuple( tid_obj->id, pk ));
         return itr ? &(*itr) : nullptr;
      }

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

      static core_asset_account_ptr create(const chainbase::database& db, const account_name& account);

      void save(chainbase::database& db);

      inline const asset& balance() const {
         return acct_data.balance;
      }
      inline asset& balance() {
         return acct_data.balance;
      }

   };

} } }  // namespace eosio::chain::contract_table_utils

