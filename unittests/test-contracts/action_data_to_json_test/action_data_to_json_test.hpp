#pragma once

#include <eosio/eosio.hpp>

class [[eosio::contract]] action_data_to_json_test : public eosio::contract {
public:
   using eosio::contract::contract;

   [[eosio::action]]
   void actionjson(const eosio::name& contract_name, const eosio::name& action_name, const std::vector<char> action_data,
                   const std::string expected_json);

};
