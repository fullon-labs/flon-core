#pragma once

#include <cstdint>
#include <type_traits>

namespace eosio::chain {

struct gas_fee_totals_v0 {
   uint64_t total_gas_consumed = 0;
   uint64_t total_gas_value_flon = 0;
};

static_assert(std::is_trivially_copyable_v<gas_fee_totals_v0>);
static_assert(sizeof(gas_fee_totals_v0) == 16);

} // namespace eosio::chain
