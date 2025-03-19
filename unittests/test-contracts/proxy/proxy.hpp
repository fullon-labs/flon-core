#pragma once

#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>
#include <eosio/asset.hpp>

#define TO_STRING(x) #x

#ifdef SYSTEM_ACCOUNT_PREFIX
   static const eosio::name token_account_name = eosio::name{TO_STRING(SYSTEM_ACCOUNT_PREFIX ) ".token"};
#else
#error "SYSTEM_ACCOUNT_PREFIX not defined"
#endif

// Extacted from token contract:
namespace eosio {
   class [[eosio::contract]] token : public eosio::contract {
   public:
      using eosio::contract::contract;

      [[eosio::action]]
      void transfer( eosio::name        from,
                     eosio::name        to,
                     eosio::asset       quantity,
                     const std::string& memo );
      using transfer_action = eosio::action_wrapper<"transfer"_n, &token::transfer>;
   };
}

// This contract:
class [[eosio::contract]] proxy : public eosio::contract {
public:
   proxy( eosio::name self, eosio::name first_receiver, eosio::datastream<const char*> ds );

   [[eosio::action]]
   void setowner( eosio::name owner, uint32_t delay );

   [[eosio::on_notify("*::transfer")]]
   void on_transfer( eosio::name        from,
                     eosio::name        to,
                     eosio::asset       quantity,
                     const std::string& memo );

   [[eosio::on_notify("*::onerror")]]
   void on_error( uint128_t sender_id, eosio::ignore<std::vector<char>> sent_trx );

   struct [[eosio::table]] config {
      eosio::name owner;
      uint32_t    delay   = 0;
      uint32_t    next_id = 0;

      EOSLIB_SERIALIZE( config, (owner)(delay)(next_id) )
   };

   using config_singleton = eosio::singleton< "config"_n,  config >;

protected:
   config_singleton _config;
};
