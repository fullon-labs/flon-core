#include <boost/test/unit_test.hpp>
#include <eosio/net_plugin/sync_range.hpp>
#include <limits>
#include <memory>
#include <fcntl.h>
#include <unistd.h>

BOOST_AUTO_TEST_CASE(sync_range_missing_middle_keeps_timeout) {
   eosio::detail::sync_range range(10, 12);
   BOOST_CHECK(range.receive(10));
   BOOST_CHECK(!range.receive(12));
   BOOST_CHECK(!range.complete());
   BOOST_CHECK(!range.receive(10)); // duplicate must not renew the timer
   BOOST_CHECK(range.receive(11));
   BOOST_CHECK(range.receive(12));
   BOOST_CHECK(range.complete());
}

BOOST_AUTO_TEST_CASE(sync_ranges_complete_independently) {
   eosio::detail::sync_range early(10, 11), later(12, 13);
   BOOST_CHECK(later.receive(12));
   BOOST_CHECK(later.receive(13));
   BOOST_CHECK(later.complete());
   BOOST_CHECK(!early.complete());
   BOOST_CHECK(!early.receive(9));
   BOOST_CHECK(!early.receive(12));
   BOOST_CHECK(early.receive(10));
   BOOST_CHECK(early.receive(11));
   BOOST_CHECK(early.complete());
   const auto last = std::numeric_limits<uint32_t>::max();
   eosio::detail::sync_range final_block(last, last);
   BOOST_CHECK(final_block.receive(last));
   BOOST_CHECK(final_block.complete());
}

BOOST_AUTO_TEST_CASE(sync_timeout_excludes_peer_before_reassignment_and_preserves_stdout) {
   struct connection {
      bool closing = false;
      bool reconnect = false;
      void close(bool retry) { closing = true; reconnect = retry; }
   };
   const int stdout_flags = fcntl(STDOUT_FILENO, F_GETFD);
   auto source = std::make_shared<connection>();
   bool reassigned = false;
   eosio::detail::handle_sync_timeout(source, [&](const auto& peer) {
      BOOST_CHECK(peer->closing);
      BOOST_CHECK(peer->reconnect);
      reassigned = true;
   });
   BOOST_CHECK(reassigned);
   BOOST_CHECK_EQUAL(fcntl(STDOUT_FILENO, F_GETFD), stdout_flags);
}
