#include <boost/test/unit_test.hpp>

#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/producer_slot_stats.hpp>
#include <eosio/testing/tester.hpp>

#include <algorithm>
#include <numeric>

using namespace eosio::chain;
using namespace eosio::chain::literals;
using namespace eosio::testing;

BOOST_AUTO_TEST_SUITE(producer_slot_stats_tests)

namespace {
const char producer_slot_stats_wast[] = R"=====(
(module
  (import "env" "get_producer_slot_stats"
    (func $get_producer_slot_stats (param i32 i32 i32 i32) (result i32)))
  (func (export "apply") (param i64 i64 i64))
)
)=====";

producer_authority make_producer(account_name producer) {
   return producer_authority{producer, block_signing_authority_v0{}};
}

const producer_slot_stats_v0& find_stats(const std::vector<producer_slot_stats_v0>& stats,
                                         account_name producer) {
   const auto itr = std::find_if(stats.begin(), stats.end(), [&](const auto& row) {
      return row.producer == producer;
   });
   BOOST_REQUIRE(itr != stats.end());
   return *itr;
}
}

BOOST_AUTO_TEST_CASE(intrinsic_requires_protocol_feature_activation) { try {
   tester chain(setup_policy::preactivate_feature_and_new_bios);

   const account_name test_account = "slotstats"_n;
   chain.create_account(test_account);
   chain.produce_block();

   BOOST_CHECK_EXCEPTION(chain.set_code(test_account, producer_slot_stats_wast),
                         wasm_exception,
                         fc_exception_message_is("env.get_producer_slot_stats unresolveable"));

   const auto& feature_manager = chain.control->get_protocol_feature_manager();
   const auto feature_digest = feature_manager.get_builtin_digest(
      builtin_protocol_feature_t::producer_slot_stats);
   BOOST_REQUIRE(feature_digest);

   chain.preactivate_protocol_features({*feature_digest});
   chain.produce_block();
   chain.set_code(test_account, producer_slot_stats_wast);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(counts_missed_slots_and_committed_head) {
   producer_authority_schedule schedule{
      1, {make_producer("alice"_n), make_producer("bob"_n)}
   };
   const uint32_t from_slot = 1;
   const uint32_t to_slot = 25;
   const auto actual = schedule.get_scheduled_producer(block_timestamp_type{to_slot}).producer_name;

   const auto stats = calculate_producer_slot_stats(
      from_slot, to_slot, to_slot, actual,
      [&](block_timestamp_type) -> const producer_authority_schedule& { return schedule; });

   BOOST_REQUIRE_EQUAL(stats.size(), 2u);
   const auto expected_total = std::accumulate(stats.begin(), stats.end(), uint32_t{0},
      [](uint32_t total, const auto& row) { return total + row.expected; });
   const auto produced_total = std::accumulate(stats.begin(), stats.end(), uint32_t{0},
      [](uint32_t total, const auto& row) { return total + row.produced; });
   const auto missed_total = std::accumulate(stats.begin(), stats.end(), uint32_t{0},
      [](uint32_t total, const auto& row) { return total + row.missed; });
   BOOST_REQUIRE_EQUAL(expected_total, 25u);
   BOOST_REQUIRE_EQUAL(produced_total, 1u);
   BOOST_REQUIRE_EQUAL(missed_total, 24u);

   const auto& actual_stats = find_stats(stats, actual);
   BOOST_REQUIRE_EQUAL(actual_stats.trailing_missed_streak, 0u);
   BOOST_REQUIRE_EQUAL(actual_stats.expected, actual_stats.produced + actual_stats.missed);
}

BOOST_AUTO_TEST_CASE(uses_schedule_effective_for_each_slot) {
   producer_authority_schedule first{
      1, {make_producer("alice"_n), make_producer("bob"_n)}
   };
   producer_authority_schedule second{
      2, {make_producer("carol"_n), make_producer("dan"_n)}
   };
   constexpr uint32_t transition_slot = 24;
   constexpr uint32_t to_slot = 30;
   const auto actual = second.get_scheduled_producer(block_timestamp_type{to_slot}).producer_name;

   const auto stats = calculate_producer_slot_stats(
      1, to_slot, to_slot, actual,
      [&](block_timestamp_type timestamp) -> const producer_authority_schedule& {
         return timestamp.slot < transition_slot ? first : second;
      });

   BOOST_REQUIRE(find_stats(stats, "alice"_n).expected > 0);
   BOOST_REQUIRE(find_stats(stats, "bob"_n).expected > 0);
   BOOST_REQUIRE(find_stats(stats, "carol"_n).expected > 0);
   BOOST_REQUIRE(find_stats(stats, "dan"_n).expected > 0);
   BOOST_REQUIRE(std::is_sorted(stats.begin(), stats.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.producer < rhs.producer;
   }));
}

BOOST_AUTO_TEST_SUITE_END()
