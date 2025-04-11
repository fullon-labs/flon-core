#pragma once

#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/asset.hpp>

#include <vector>
#include <memory>

namespace eosio { namespace chain { namespace contract_table_utils {

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

