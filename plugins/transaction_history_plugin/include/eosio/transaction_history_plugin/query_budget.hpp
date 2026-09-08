#pragma once

#include <eosio/chain/abi_serializer.hpp>
#include <eosio/chain/exceptions.hpp>
#include <fc/io/json.hpp>

namespace eosio {

class history_query_budget {
public:
   const fc::time_point deadline;
   const uint64_t max_bytes;

   history_query_budget(fc::time_point until, uint64_t bytes)
      : deadline(until), max_bytes(bytes) {}

   void check() const {
      EOS_ASSERT(fc::time_point::now() < deadline, chain::deadline_exception,
                 "Transaction history query exceeded its execution time limit");
   }
   void check_size(uint64_t bytes) const {
      EOS_ASSERT(bytes <= max_bytes, chain::plugin_exception,
                 "Transaction history record or response exceeds the ${limit} byte limit",
                 ("limit", max_bytes));
   }
   fc::variant parse(const std::string& json) const {
      check();
      check_size(json.size());
      auto value = fc::json::from_string(json);
      check();
      return value;
   }
   auto abi_yield(fc::microseconds abi_limit) const {
      const auto abi = chain::abi_serializer::create_yield_function(abi_limit);
      return [budget = *this, abi](size_t depth) {
         budget.check();
         abi(depth);
      };
   }
   template<typename T>
   size_t serialized_size(const T& object) const {
      check();
      const auto json = fc::json::to_string(fc::variant(object), [this](size_t bytes) {
         check();
         check_size(bytes);
      });
      check();
      check_size(json.size());
      return json.size();
   }
};

} // namespace eosio
