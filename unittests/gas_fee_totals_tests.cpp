#include <boost/test/unit_test.hpp>

#include <eosio/chain/exceptions.hpp>
#include <eosio/testing/tester.hpp>

using namespace eosio::chain;
using namespace eosio::chain::literals;
using namespace eosio::testing;

BOOST_AUTO_TEST_SUITE(gas_fee_totals_tests)

namespace {
const char gas_fee_totals_wast[] = R"=====(
(module
  (import "env" "get_gas_fee_totals"
    (func $get_gas_fee_totals (param i32 i32) (result i32)))
  (func (export "apply") (param i64 i64 i64))
)
)=====";
}

BOOST_AUTO_TEST_CASE(intrinsic_requires_protocol_feature_activation) { try {
   tester chain(setup_policy::preactivate_feature_and_new_bios);
   const account_name test_account = "gasfeetotals"_n;
   chain.create_account(test_account);
   chain.produce_block();

   BOOST_CHECK_EXCEPTION(chain.set_code(test_account, gas_fee_totals_wast),
                         wasm_exception,
                         fc_exception_message_is("env.get_gas_fee_totals unresolveable"));

   const auto digest = chain.control->get_protocol_feature_manager().get_builtin_digest(
      builtin_protocol_feature_t::gas_fee_totals);
   BOOST_REQUIRE(digest);
   chain.preactivate_protocol_features({*digest});
   chain.produce_block();
   chain.set_code(test_account, gas_fee_totals_wast);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
