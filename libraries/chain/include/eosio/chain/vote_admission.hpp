#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace eosio::chain {

// Reservations cover queued, executing AND deferred votes. Shared ownership
// releases capacity even when asio discards a handler without invoking it.
class vote_admission {
   struct state {
      std::mutex mutex;
      size_t remote = 0, local = 0;
      std::unordered_map<uint32_t, size_t> peers;
   };
   std::shared_ptr<state> counts = std::make_shared<state>();
   const size_t global_limit, peer_limit, local_limit;
public:
   struct reservation {
      std::shared_ptr<state> counts;
      uint32_t peer;
      bool active = false;
      reservation(std::shared_ptr<state> counts, uint32_t peer)
         : counts(std::move(counts)), peer(peer) {}
      ~reservation() {
         if (!active) return;
         std::lock_guard lock(counts->mutex);
         if (peer == 0) { --counts->local; return; }
         --counts->remote;
         auto found = counts->peers.find(peer);
         if (--found->second == 0) counts->peers.erase(found);
      }
      reservation(const reservation&) = delete;
      reservation& operator=(const reservation&) = delete;
   };
   using ticket = std::shared_ptr<reservation>;

   explicit vote_admission(size_t global = 10000, size_t peer = 2500, size_t local = 256)
      : global_limit(global), peer_limit(peer), local_limit(local) {}

   ticket acquire(uint32_t peer) {
      std::lock_guard lock(counts->mutex);
      if (peer == 0) {
         if (counts->local >= local_limit) return {};
      } else {
         const auto found = counts->peers.find(peer);
         if (counts->remote >= global_limit ||
             (found != counts->peers.end() && found->second >= peer_limit)) return {};
      }
      auto result = std::make_shared<reservation>(counts, peer);
      if (peer == 0) ++counts->local;
      else {
         ++counts->peers[peer]; // allocation may throw before any counter changes
         ++counts->remote;
      }
      result->active = true;
      return result;
   }

   size_t size() const {
      std::lock_guard lock(counts->mutex);
      return counts->remote + counts->local;
   }
};

} // namespace eosio::chain
