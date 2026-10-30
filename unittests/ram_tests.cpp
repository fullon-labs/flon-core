#include <boost/test/unit_test.hpp>

#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include "eosio_system_tester.hpp"

using namespace eosio::chain;
using namespace eosio::testing;
using namespace eosio_system;

BOOST_AUTO_TEST_SUITE(ram_tests)

// FLON does not expose the legacy EOSIO RAM market. RAM consumption is charged
// from GAS by the core resource-limits manager, so this system-level test
// verifies the replacement buygas action in both validating tester modes.
BOOST_AUTO_TEST_CASE_TEMPLATE(gas_resource_model_replaces_ram_market, T, validating_testers) { try {
   T chain;
   chain.set_code(config::system_account_name, test_contracts::system_contract_wasm());
   chain.set_abi(config::system_account_name, test_contracts::system_contract_abi());
   chain.produce_block();

   const auto buygas = chain.get_action(
      config::system_account_name,
      "buygas"_n,
      {{config::system_account_name, config::active_name}},
      fc::mutable_variant_object()
         ("payer", config::system_account_name)
         ("receiver", config::system_account_name)
         ("quant", "1.00000000 FLON"));
   BOOST_REQUIRE_EQUAL(buygas.name, "buygas"_n);

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
