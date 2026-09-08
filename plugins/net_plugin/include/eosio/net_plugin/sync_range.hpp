#pragma once

#include <cstdint>

namespace eosio::detail {

// A peer sends a requested range in order on one TCP stream. Only new,
// contiguous receipts renew its timeout; duplicates cannot keep it alive.
struct sync_range {
   uint32_t start = 0;
   uint32_t end = 0;
   uint64_t next = 0;

   sync_range() = default;
   sync_range(uint32_t first, uint32_t last) : start(first), end(last), next(first) {}

   bool receive(uint32_t block) {
      if (block != next || block > end) return false;
      ++next;
      return true;
   }
   bool complete() const { return next > end; }
};

template<typename Connection, typename Reassign>
void handle_sync_timeout(const Connection& connection, Reassign&& reassign) {
   // Mark the peer closing before selecting another source. An unqualified
   // close(true) here would instead close the process's stdout descriptor.
   connection->close(true);
   reassign(connection);
}

} // namespace eosio::detail
