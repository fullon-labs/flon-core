#include "pending_submissions.hpp"

#include <eosio/chain/name.hpp>
#include <stdexcept>

using namespace eosio::chain;
using namespace eosio::interop_relayer_detail;

namespace {
void require(bool condition, const char* message) {
   if (!condition)
      throw std::runtime_error(message);
}

template<typename F>
void expect_status(int status, F&& f) {
   try {
      f();
   } catch (const request_error& e) {
      require(e.status == status, "unexpected rejection status");
      return;
   }
   throw std::runtime_error("expected rejection");
}
}

int main() {
   pending_submissions pending;
   pending.limit = 1;
   const pending_key key{"interopowner"_n, false, "address-a", 1};
   const pending_key other{"interopowner"_n, false, "address-a", 2};
   const auto now = pending_submissions::clock::now();
   const auto later = now + std::chrono::seconds(30);
   using namespace std::chrono_literals;
   require(relay_deadline(now + 5s, later) == now + 3s, "short HTTP deadline must leave response headroom");
   require(relay_deadline(now + 60s, later) == later, "transaction deadline must still cap a long request");
   require(relay_deadline(pending_submissions::clock::time_point::max(), later) == later,
           "disabled HTTP deadline must retain the transaction limit");
   require(relay_deadline(now + 1s, later) < now, "queued request has already consumed the response budget");
   const auto fingerprint = digest_type::hash(std::string("same request"));
   const auto different = digest_type::hash(std::string("different request"));
   const auto txid = digest_type::hash(std::string("transaction"));
   int timeouts = 0;
   const submission_callback timed_out = [&](int status, const std::optional<fc::variant>& response) {
      require(status == 504 && response.has_value(), "missing timeout response");
      require(response->get_object()["transaction_id"].as<transaction_id_type>() == txid, "timeout lost transaction ID");
      ++timeouts;
   };

   const auto old = pending.reserve(key, fingerprint, txid, now, timed_out);
   require(old.has_value(), "initial reservation failed");
   require(!pending.reserve(key, fingerprint, txid, later, timed_out), "duplicate should join existing request");
   expect_status(409, [&] { pending.reserve(key, different, txid, later, timed_out); });
   expect_status(429, [&] { pending.reserve(other, fingerprint, txid, later, timed_out); });
   require(!pending.active(key, *old), "expired request must not be signed");
   pending.expire();
   require(timeouts == 2, "timeout did not reply to all waiters");

   int completions = 0;
   const auto replacement = pending.reserve(key, fingerprint, txid, later, [&](int status, auto) {
      require(status == 202, "new reservation received stale result");
      ++completions;
   });
   require(replacement.has_value() && *replacement != *old, "reused request generation");
   pending.finish(key, *old, 500, std::nullopt);
   require(pending.active(key, *replacement), "late callback removed replacement");
   pending.finish(key, *replacement, 202, std::nullopt);
   pending.finish(key, *replacement, 202, std::nullopt);
   require(completions == 1, "completion was not delivered exactly once");
}
