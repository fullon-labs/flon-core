#define BOOST_TEST_MODULE chain safety
#include <boost/test/unit_test.hpp>
#include <eosio/chain/vote_admission.hpp>
#include <eosio/chain/finality/finalizer.hpp>
#include <fc/filesystem.hpp>
#include <future>
#include <vector>

using namespace eosio::chain;

BOOST_AUTO_TEST_CASE(vote_reservations_bound_peers_global_and_reserve_local_capacity) {
   vote_admission limits(3, 2, 1);
   auto first = limits.acquire(1), second = limits.acquire(1);
   BOOST_REQUIRE(first && second);
   BOOST_CHECK(!limits.acquire(1));
   auto third = limits.acquire(2);
   BOOST_REQUIRE(third);
   BOOST_CHECK(!limits.acquire(3));
   auto local = limits.acquire(0);
   BOOST_REQUIRE(local);
   BOOST_CHECK(!limits.acquire(0));
   auto deferred = first;
   first.reset();
   BOOST_CHECK(!limits.acquire(1)); // deferred votes still own their capacity
   deferred.reset();
   BOOST_REQUIRE(limits.acquire(3));
   second.reset(); third.reset(); local.reset();
   BOOST_CHECK_EQUAL(limits.size(), 0u);
}

BOOST_AUTO_TEST_CASE(vote_reservations_release_on_throw_cancel_and_owner_destruction) {
   vote_admission limits(2, 2, 1);
   try {
      auto ticket = limits.acquire(1);
      throw std::runtime_error("post/handler failure");
   } catch (const std::runtime_error&) {}
   BOOST_CHECK_EQUAL(limits.size(), 0u);
   {
      std::function<void()> never_run = [ticket = limits.acquire(1)] {};
      BOOST_CHECK_EQUAL(limits.size(), 1u);
   }
   BOOST_CHECK_EQUAL(limits.size(), 0u);
   vote_admission::ticket retained;
   { vote_admission temporary; retained = temporary.acquire(7); }
   retained.reset(); // counts outlive their original owner when handlers do
}

BOOST_AUTO_TEST_CASE(vote_reservations_are_bounded_under_concurrent_admission) {
   vote_admission limits(32, 8, 2);
   std::vector<std::future<void>> threads;
   for (uint32_t peer = 1; peer <= 8; ++peer) {
      threads.push_back(std::async(std::launch::async, [&, peer] {
         for (size_t i = 0; i < 1000; ++i) {
            std::vector<vote_admission::ticket> held;
            for (size_t j = 0; j < 10; ++j) held.push_back(limits.acquire(peer));
            if (limits.size() > 32) throw std::runtime_error("global budget exceeded");
         }
      }));
   }
   for (auto& thread : threads) BOOST_CHECK_NO_THROW(thread.get());
   BOOST_CHECK_EQUAL(limits.size(), 0u);
}

BOOST_AUTO_TEST_CASE(finalizer_failed_replace_is_sticky_and_keeps_previous_safety_record) {
   fc::temp_directory temp;
   const auto directory = temp.path() / "finalizers";
   const auto file = directory / "safety.dat";
   const auto key = bls_private_key::generate();
   my_finalizers_t finalizers(file);
   finalizers.set_keys({{key.get_public_key().to_string(), key.to_string()}});
   BOOST_REQUIRE(finalizers.save_finalizer_safety_info());
   const auto backup = temp.path() / "previous";
   std::filesystem::rename(directory, backup);
   std::filesystem::create_directories(file); // destination is a directory: promotion must fail
   BOOST_CHECK(!finalizers.save_finalizer_safety_info());
   std::filesystem::remove(file);
   std::filesystem::remove(directory);
   std::filesystem::rename(backup, directory);
   BOOST_CHECK(!finalizers.save_finalizer_safety_info()); // cannot silently resume voting
   my_finalizers_t reader(file);
   BOOST_CHECK_NO_THROW(reader.set_keys({{key.get_public_key().to_string(), key.to_string()}}));
   BOOST_CHECK_EQUAL(reader.size(), 1u);
}
