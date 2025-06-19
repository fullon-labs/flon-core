#include "action_data_to_json_test.hpp"
#include <eosio/transaction.hpp>

using namespace eosio;

void action_data_to_json_test::actionjson(const eosio::name& contract_name,
                                          const eosio::name& action_name,
                                          const std::vector<char> action_data,
                                          const std::string expected_json)
{
   auto result = eosio::action_data_to_json( contract_name, action_name, action_data );
   check(result == expected_json, "action data to json conversion failed, expected: " + expected_json + ", got: " + result);
}

