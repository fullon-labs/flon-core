#include <eosio/chain/webassembly/interface.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/producer_slot_stats.hpp>
#include <eosio/chain/gas_fee_totals.hpp>
#include <eosio/chain/resource_limits.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace eosio { namespace chain { namespace webassembly {
   namespace {
      // Bound a single host call so a block following a long outage cannot spend
      // unbounded native CPU. Contracts can cover the full gap using chunks.
      constexpr uint32_t max_producer_stats_slots_per_call = 100'000;

   }

   int32_t interface::get_active_producers( legacy_span<account_name> producers ) const {
      auto active_producers = context.get_active_producers();

      size_t len = active_producers.size();
      auto s = len * sizeof(chain::account_name);
      if( producers.size_bytes() == 0 ) return s;

      auto copy_size = std::min( producers.size(), s );
      std::memcpy( producers.data(), active_producers.data(), copy_size );

      return copy_size;
   }

   int32_t interface::get_producer_slot_stats(uint32_t from_slot, uint32_t to_slot,
                                               legacy_span<char> stats) const {
      const auto head_slot = context.control.head().timestamp().slot;
      const auto pending_slot = context.control.pending_block_timestamp().slot;

      // onblock executes while the block being applied is already exposed as
      // head(), so its timestamp is a valid lower bound. Ordinary transaction
      // execution still cannot inspect slots before the current head.
      EOS_ASSERT(from_slot >= head_slot, wasm_execution_error,
                 "producer slot statistics must start at or after the current head slot");
      EOS_ASSERT(from_slot <= to_slot, wasm_execution_error,
                 "producer slot statistics range is invalid");
      EOS_ASSERT(to_slot <= pending_slot, wasm_execution_error,
                 "producer slot statistics cannot inspect slots after the pending block");

      const uint64_t slot_count = uint64_t(to_slot) - from_slot + 1;
      EOS_ASSERT(slot_count <= max_producer_stats_slots_per_call, wasm_execution_error,
                 "producer slot statistics range exceeds ${max} slots",
                 ("max", max_producer_stats_slots_per_call));

      const auto actual_producer = context.control.pending_block_producer();
      const auto result = calculate_producer_slot_stats(
         from_slot, to_slot, pending_slot, actual_producer,
         [&](block_timestamp_type timestamp) -> const producer_authority_schedule& {
            const auto& schedule = context.control.head_active_producers(timestamp);
            EOS_ASSERT(!schedule.producers.empty(), wasm_execution_error,
                       "active producer schedule is empty");
            return schedule;
         });

      const size_t required_size = result.size() * sizeof(producer_slot_stats_v0);
      EOS_ASSERT(required_size <= std::numeric_limits<int32_t>::max(), wasm_execution_error,
                 "producer slot statistics result is too large");
      if (stats.size_bytes() == 0) {
         return static_cast<int32_t>(required_size);
      }

      const size_t copy_size = std::min(stats.size_bytes(), required_size);
      std::memcpy(stats.data(), result.data(), copy_size);
      return static_cast<int32_t>(copy_size);
   }

   int32_t interface::get_gas_fee_totals(legacy_span<char> totals) const {
      const auto& resource_limits = context.control.get_resource_limits_manager();
      const gas_fee_totals_v0 result{
         resource_limits.get_total_gas_consumed(),
         resource_limits.get_total_gas_value_flon()
      };

      constexpr size_t required_size = sizeof(gas_fee_totals_v0);
      if (totals.size_bytes() == 0) {
         return static_cast<int32_t>(required_size);
      }

      const size_t copy_size = std::min(totals.size_bytes(), required_size);
      std::memcpy(totals.data(), &result, copy_size);
      return static_cast<int32_t>(copy_size);
   }
}}} // ns eosio::chain::webassembly
