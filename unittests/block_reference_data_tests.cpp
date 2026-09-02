#include <eosio/chain/exceptions.hpp>
#include <eosio/testing/tester.hpp>

#include <fc/io/raw.hpp>

#include <boost/test/unit_test.hpp>

using namespace eosio::chain;
using namespace eosio::testing;

namespace {

const char block_reference_data_wast[] = R"=====(
(module
  (import "env" "get_recent_block_id" (func $get_recent_block_id (param i32 i32) (result i32)))
  (import "env" "get_last_irreversible_block_num" (func $get_last_irreversible_block_num (result i32)))
  (import "env" "read_action_data" (func $read_action_data (param i32 i32) (result i32)))
  (import "env" "memcmp" (func $memcmp (param i32 i32 i32) (result i32)))
  (import "env" "eosio_assert" (func $eosio_assert (param i32 i32)))
  (memory 1)
  (func (export "apply") (param i64 i64 i64)
    (call $eosio_assert
      (i32.eq (call $read_action_data (i32.const 0) (i32.const 48)) (i32.const 48))
      (i32.const 256))
    (call $eosio_assert
      (call $get_recent_block_id (i32.load (i32.const 0)) (i32.const 64))
      (i32.const 288))
    (call $eosio_assert
      (i32.eq (call $memcmp (i32.const 4) (i32.const 64) (i32.const 32)) (i32.const 0))
      (i32.const 320))
    (call $eosio_assert
      (i32.eq (call $get_last_irreversible_block_num) (i32.load (i32.const 36)))
      (i32.const 352))
    (call $eosio_assert
      (i32.eq (call $get_recent_block_id (i32.const 0) (i32.const 96)) (i32.const 0))
      (i32.const 384))
    (call $eosio_assert
      (i32.eq (call $get_recent_block_id (i32.load (i32.const 40)) (i32.const 96)) (i32.const 0))
      (i32.const 416))
    (call $eosio_assert
      (i32.eq (call $get_recent_block_id (i32.load (i32.const 44)) (i32.const 96)) (i32.const 0))
      (i32.const 448))
  )
  (data (i32.const 256) "wrong action data size\00")
  (data (i32.const 288) "recent block unavailable\00")
  (data (i32.const 320) "recent block ID mismatch\00")
  (data (i32.const 352) "LIB number mismatch\00")
  (data (i32.const 384) "block zero was accepted\00")
  (data (i32.const 416) "current block was accepted\00")
  (data (i32.const 448) "future block was accepted\00")
)
)=====";

const char last_irreversible_block_num_wast[] = R"=====(
(module
  (import "env" "get_last_irreversible_block_num" (func $get_last_irreversible_block_num (result i32)))
  (func (export "apply") (param i64 i64 i64)
    (drop (call $get_last_irreversible_block_num))
  )
)
)=====";

} // namespace

BOOST_AUTO_TEST_SUITE(block_reference_data_tests)

BOOST_AUTO_TEST_CASE(block_reference_data_intrinsics) { try {
   tester chain(setup_policy::preactivate_feature_and_new_bios);

   const account_name test_account = "blockrefs"_n;
   chain.create_account(test_account);
   chain.produce_block();

   BOOST_CHECK_EXCEPTION(chain.set_code(test_account, block_reference_data_wast),
                         wasm_exception,
                         fc_exception_message_is("env.get_recent_block_id unresolveable"));
   BOOST_CHECK_EXCEPTION(chain.set_code(test_account, last_irreversible_block_num_wast),
                         wasm_exception,
                         fc_exception_message_is("env.get_last_irreversible_block_num unresolveable"));

   const auto& feature_manager = chain.control->get_protocol_feature_manager();
   const auto feature_digest = feature_manager.get_builtin_digest(
      builtin_protocol_feature_t::block_reference_data);
   BOOST_REQUIRE(feature_digest);

   chain.preactivate_protocol_features({*feature_digest});
   chain.produce_block();
   chain.set_code(test_account, block_reference_data_wast);

   const auto recent_block = chain.control->head();
   const uint32_t current_block_num = recent_block.block_num() + 1;
   const auto action_data = fc::raw::pack(recent_block.block_num(),
                                          recent_block.id(),
                                          recent_block.irreversible_blocknum(),
                                          current_block_num,
                                          current_block_num + 1);

   action test_action({{test_account, config::active_name}}, test_account, action_name{}, action_data);
   BOOST_REQUIRE_EQUAL(chain.push_action(std::move(test_action), test_account.to_uint64_t()), chain.success());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
