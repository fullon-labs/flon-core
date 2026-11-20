#pragma once

#include <eosio/chain/types.hpp>
#include <fc/variant_object.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <tuple>

namespace eosio::interop_relayer_detail {
using chain::digest_type;
using chain::name;
using chain::transaction_id_type;

using submission_callback = std::function<void(int, std::optional<fc::variant>)>;

struct request_error {
   int status;
   const char* code;
   const char* message;
};

inline void send_error(const submission_callback& cb, const request_error& error) {
   cb(error.status, fc::variant(fc::mutable_variant_object()("code", error.code)("message", error.message)));
}

using pending_key = std::tuple<name, bool, std::string, uint64_t>;

inline std::chrono::steady_clock::time_point relay_deadline(
      std::chrono::steady_clock::time_point transport, std::chrono::steady_clock::time_point transaction) {
   if (transport == std::chrono::steady_clock::time_point::max())
      return transaction;
   return std::min(transaction, transport - std::chrono::seconds(2));
}

struct pending_submissions {
   using clock = std::chrono::steady_clock;

   struct entry {
      uint64_t generation;
      clock::time_point deadline;
      transaction_id_type transaction_id;
      digest_type fingerprint;
      std::vector<submission_callback> callbacks;
   };

   std::mutex mutex;
   std::map<pending_key, entry> entries;
   size_t limit = 0;
   uint64_t next_generation = 0;

   std::optional<uint64_t> reserve(const pending_key& key, const digest_type& fingerprint,
                                   const transaction_id_type& transaction_id, clock::time_point deadline,
                                   const submission_callback& cb) {
      std::lock_guard lock(mutex);
      auto it = entries.find(key);
      if (it != entries.end()) {
         if (it->second.fingerprint != fingerprint)
            throw request_error{409, "submission_conflict", "A different request for this address and nonce is already in flight"};
         if (it->second.callbacks.size() >= 8)
            throw request_error{429, "too_many_retries", "Too many callers waiting for this submission"};
         it->second.callbacks.push_back(cb);
         it->second.deadline = std::min(it->second.deadline, deadline);
         return std::nullopt;
      }
      if (entries.size() >= limit)
         throw request_error{429, "relayer_busy", "Interop relayer is busy; retry later"};
      const auto generation = ++next_generation;
      entries.emplace(key, entry{generation, deadline, transaction_id, fingerprint, {cb}});
      return generation;
   }

   bool active(const pending_key& key, uint64_t generation) {
      std::lock_guard lock(mutex);
      const auto it = entries.find(key);
      return it != entries.end() && it->second.generation == generation && clock::now() < it->second.deadline;
   }

   void finish(const pending_key& key, uint64_t generation, int status, const std::optional<fc::variant>& result) {
      std::vector<submission_callback> callbacks;
      {
         std::lock_guard lock(mutex);
         auto it = entries.find(key);
         if (it == entries.end() || it->second.generation != generation)
            return;
         callbacks = std::move(it->second.callbacks);
         entries.erase(it);
      }
      for (const auto& cb : callbacks)
         cb(status, result);
   }

   void expire() {
      std::vector<entry> expired;
      {
         std::lock_guard lock(mutex);
         const auto now = clock::now();
         for (auto it = entries.begin(); it != entries.end();) {
            if (now >= it->second.deadline) {
               expired.push_back(std::move(it->second));
               it = entries.erase(it);
            } else {
               ++it;
            }
         }
      }
      for (const auto& item : expired) {
         const fc::variant response(fc::mutable_variant_object()
            ("code", "submission_timeout")
            ("message", "Result unknown; check transaction status before retrying")
            ("transaction_id", item.transaction_id));
         for (const auto& cb : item.callbacks)
            cb(504, response);
      }
   }

   void shutdown() {
      decltype(entries) remaining;
      {
         std::lock_guard lock(mutex);
         remaining.swap(entries);
      }
      for (const auto& item : remaining)
         for (const auto& cb : item.second.callbacks)
            send_error(cb, {503, "relayer_stopping", "Interop relayer is shutting down"});
   }
};
} // namespace eosio::interop_relayer_detail
