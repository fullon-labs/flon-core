#pragma once

#include <eosio/chain/producer_schedule.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <type_traits>
#include <vector>

namespace eosio::chain {

struct producer_slot_stats_v0 {
   account_name producer;
   uint32_t expected = 0;
   uint32_t produced = 0;
   uint32_t missed = 0;
   uint32_t longest_missed_streak = 0;
   uint32_t leading_missed_streak = 0;
   uint32_t trailing_missed_streak = 0;
};

static_assert(std::is_trivially_copyable_v<producer_slot_stats_v0>);
static_assert(sizeof(producer_slot_stats_v0) == 32);

struct producer_slot_stats_v1 {
   account_name producer;
   uint32_t expected = 0;
   uint32_t produced = 0;
   uint32_t missed = 0;
   uint32_t idle_skipped = 0;
   uint32_t longest_missed_streak = 0;
   uint32_t leading_missed_streak = 0;
   uint32_t trailing_missed_streak = 0;
};

static_assert(std::is_trivially_copyable_v<producer_slot_stats_v1>);
static_assert(sizeof(producer_slot_stats_v1) == 40);

/**
 * Calculate deterministic producer statistics for an inclusive range of slots.
 * The resolver must return the producer schedule effective at the supplied slot.
 * Only produced_slot can contain a produced block; every other slot in the
 * range represents a missed block preceding that committed block.
 */
template <typename ScheduleResolver>
std::vector<producer_slot_stats_v0> calculate_producer_slot_stats(
   uint32_t from_slot,
   uint32_t to_slot,
   uint32_t produced_slot,
   account_name actual_producer,
   ScheduleResolver&& resolve_schedule) {
   struct accumulator {
      producer_slot_stats_v0 result;
      uint32_t current_missed_streak = 0;
      bool seen_produced = false;
   };

   std::map<account_name, accumulator> by_producer;
   for (uint64_t slot = from_slot; slot <= to_slot; ++slot) {
      const block_timestamp_type timestamp{static_cast<uint32_t>(slot)};
      const auto& schedule = resolve_schedule(timestamp);
      const auto scheduled_producer = schedule.get_scheduled_producer(timestamp).producer_name;
      auto& entry = by_producer[scheduled_producer];
      entry.result.producer = scheduled_producer;
      ++entry.result.expected;

      const bool produced = slot == produced_slot && scheduled_producer == actual_producer;
      if (produced) {
         ++entry.result.produced;
         entry.seen_produced = true;
         entry.current_missed_streak = 0;
      } else {
         ++entry.result.missed;
         ++entry.current_missed_streak;
         entry.result.longest_missed_streak =
            std::max(entry.result.longest_missed_streak, entry.current_missed_streak);
         if (!entry.seen_produced) {
            ++entry.result.leading_missed_streak;
         }
      }
   }

   std::vector<producer_slot_stats_v0> result;
   result.reserve(by_producer.size());
   for (auto& [producer, entry] : by_producer) {
      entry.result.trailing_missed_streak = entry.current_missed_streak;
      result.push_back(entry.result);
   }
   return result;
}

/**
 * Calculate elastic-block-aware producer statistics. Slots before the first
 * mandatory heartbeat after previous_produced_slot are protocol-authorized
 * idle skips and are excluded from `expected`. Once the heartbeat is due,
 * every unproduced slot is a real miss until a block is committed.
 */
template <typename ScheduleResolver>
std::vector<producer_slot_stats_v1> calculate_producer_slot_stats_v2(
   uint32_t from_slot,
   uint32_t to_slot,
   uint32_t previous_produced_slot,
   uint32_t produced_slot,
   uint32_t idle_interval_slots,
   account_name actual_producer,
   ScheduleResolver&& resolve_schedule) {
   struct accumulator {
      producer_slot_stats_v1 result;
      uint32_t current_missed_streak = 0;
      bool seen_mandatory_result = false;
   };

   const uint64_t mandatory_from = static_cast<uint64_t>(previous_produced_slot) +
                                   std::max<uint32_t>(idle_interval_slots, 1u);
   std::map<account_name, accumulator> by_producer;
   for (uint64_t slot = from_slot; slot <= to_slot; ++slot) {
      const block_timestamp_type timestamp{static_cast<uint32_t>(slot)};
      const auto& schedule = resolve_schedule(timestamp);
      const auto scheduled_producer = schedule.get_scheduled_producer(timestamp).producer_name;
      auto& entry = by_producer[scheduled_producer];
      entry.result.producer = scheduled_producer;

      const bool produced = slot == produced_slot && scheduled_producer == actual_producer;
      if (produced) {
         ++entry.result.expected;
         ++entry.result.produced;
         entry.seen_mandatory_result = true;
         entry.current_missed_streak = 0;
      } else if (slot < mandatory_from) {
         ++entry.result.idle_skipped;
      } else {
         ++entry.result.expected;
         ++entry.result.missed;
         ++entry.current_missed_streak;
         entry.result.longest_missed_streak =
            std::max(entry.result.longest_missed_streak, entry.current_missed_streak);
         if (!entry.seen_mandatory_result) {
            ++entry.result.leading_missed_streak;
         }
      }
   }

   std::vector<producer_slot_stats_v1> result;
   result.reserve(by_producer.size());
   for (auto& [producer, entry] : by_producer) {
      entry.result.trailing_missed_streak = entry.current_missed_streak;
      result.push_back(entry.result);
   }
   return result;
}

} // namespace eosio::chain
