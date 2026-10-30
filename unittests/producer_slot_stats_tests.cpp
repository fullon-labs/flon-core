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

const char producer_slot_stats_v2_wast[] = R"=====(
(module
  (import "env" "get_producer_slot_stats_v2"
    (func $get_producer_slot_stats_v2 (param i32 i32 i32 i32 i32) (result i32)))
  (func (export "apply") (param i64 i64 i64))
)
)=====";

producer_authority make_producer(account_name producer) {
   return producer_authority{producer, block_signing_authority_v0{}};
}

template <typename Stats>
const Stats& find_stats(const std::vector<Stats>& stats, account_name producer) {
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

BOOST_AUTO_TEST_CASE(v2_intrinsic_requires_its_own_protocol_feature) { try {
   tester chain(setup_policy::preactivate_feature_and_new_bios);
   const account_name test_account = "slotstats"_n;
   chain.create_account(test_account);
   chain.produce_block();

   const auto& manager = chain.control->get_protocol_feature_manager();
   const auto v1 = manager.get_builtin_digest(builtin_protocol_feature_t::producer_slot_stats);
   const auto v2 = manager.get_builtin_digest(builtin_protocol_feature_t::producer_slot_stats_v2);
   BOOST_REQUIRE(v1);
   BOOST_REQUIRE(v2);
   chain.preactivate_protocol_features({*v1});
   chain.produce_block();
   BOOST_CHECK_EXCEPTION(chain.set_code(test_account, producer_slot_stats_v2_wast),
                         wasm_exception,
                         fc_exception_message_is("env.get_producer_slot_stats_v2 unresolveable"));
   chain.preactivate_protocol_features({*v2});
   chain.produce_block();
   chain.set_code(test_account, producer_slot_stats_v2_wast);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(v2_excludes_elastic_idle_slots_from_misses) {
   producer_authority_schedule schedule{1, {make_producer("alice"_n)}};
   const auto stats = calculate_producer_slot_stats_v2(
      101, 124, 100, 124, 24, "alice"_n,
      [&](block_timestamp_type) -> const producer_authority_schedule& { return schedule; });

   BOOST_REQUIRE_EQUAL(stats.size(), 1u);
   const auto& row = stats.front();
   BOOST_REQUIRE_EQUAL(row.expected, 1u);
   BOOST_REQUIRE_EQUAL(row.produced, 1u);
   BOOST_REQUIRE_EQUAL(row.missed, 0u);
   BOOST_REQUIRE_EQUAL(row.idle_skipped, 23u);
   BOOST_REQUIRE_EQUAL(row.longest_missed_streak, 0u);
}

BOOST_AUTO_TEST_CASE(v2_counts_only_overdue_slots_as_missed) {
   producer_authority_schedule schedule{1, {make_producer("alice"_n)}};
   const auto stats = calculate_producer_slot_stats_v2(
      101, 130, 100, 130, 24, "alice"_n,
      [&](block_timestamp_type) -> const producer_authority_schedule& { return schedule; });

   const auto& row = stats.front();
   BOOST_REQUIRE_EQUAL(row.expected, 7u);
   BOOST_REQUIRE_EQUAL(row.produced, 1u);
   BOOST_REQUIRE_EQUAL(row.missed, 6u);
   BOOST_REQUIRE_EQUAL(row.idle_skipped, 23u);
   BOOST_REQUIRE_EQUAL(row.longest_missed_streak, 6u);
   BOOST_REQUIRE_EQUAL(row.leading_missed_streak, 6u);
   BOOST_REQUIRE_EQUAL(row.trailing_missed_streak, 0u);
}

BOOST_AUTO_TEST_CASE(committed_producer_overrides_transition_resolver) {
   producer_authority_schedule stale_schedule{
      4, {make_producer("alice"_n), make_producer("bob"_n)}
   };
   constexpr uint32_t previous_produced_slot = 100;
   constexpr uint32_t produced_slot = 124;

   // Model the first committed block after a proposer-policy transition: the
   // accepted header is from carol, while historical resolution from the head
   // state still returns the adjacent policy for this slot.
   const auto stats = calculate_producer_slot_stats_v2(
      previous_produced_slot + 1, produced_slot, previous_produced_slot,
      produced_slot, 24, "carol"_n,
      [&](block_timestamp_type) -> const producer_authority_schedule& {
         return stale_schedule;
      });

   const auto& carol = find_stats(stats, "carol"_n);
   BOOST_REQUIRE_EQUAL(carol.expected, 1u);
   BOOST_REQUIRE_EQUAL(carol.produced, 1u);
   BOOST_REQUIRE_EQUAL(carol.missed, 0u);

   const auto produced_total = std::accumulate(stats.begin(), stats.end(), uint32_t{0},
      [](uint32_t total, const auto& row) { return total + row.produced; });
   const auto expected_total = std::accumulate(stats.begin(), stats.end(), uint32_t{0},
      [](uint32_t total, const auto& row) { return total + row.expected; });
   const auto idle_total = std::accumulate(stats.begin(), stats.end(), uint32_t{0},
      [](uint32_t total, const auto& row) { return total + row.idle_skipped; });
   BOOST_REQUIRE_EQUAL(produced_total, 1u);
   BOOST_REQUIRE_EQUAL(expected_total, 1u);
   BOOST_REQUIRE_EQUAL(expected_total + idle_total, produced_slot - previous_produced_slot);
}

BOOST_AUTO_TEST_CASE(v2_chunking_preserves_idle_classification) {
   producer_authority_schedule schedule{1, {make_producer("alice"_n)}};
   const auto resolve = [&](block_timestamp_type) -> const producer_authority_schedule& {
      return schedule;
   };
   const auto first = calculate_producer_slot_stats_v2(
      101, 112, 100, 124, 24, "alice"_n, resolve);
   const auto second = calculate_producer_slot_stats_v2(
      113, 124, 100, 124, 24, "alice"_n, resolve);

   BOOST_REQUIRE_EQUAL(first.size(), 1u);
   BOOST_REQUIRE_EQUAL(second.size(), 1u);
   BOOST_REQUIRE_EQUAL(first.front().idle_skipped + second.front().idle_skipped, 23u);
   BOOST_REQUIRE_EQUAL(first.front().expected + second.front().expected, 1u);
   BOOST_REQUIRE_EQUAL(first.front().produced + second.front().produced, 1u);
   BOOST_REQUIRE_EQUAL(first.front().missed + second.front().missed, 0u);
}

BOOST_AUTO_TEST_SUITE_END()
