#define BOOST_TEST_MODULE snapshot file persistence
#include <boost/test/unit_test.hpp>
#include <eosio/chain/snapshot_file.hpp>
#include <eosio/chain/durable_file.hpp>
#include <fc/filesystem.hpp>
#include <fstream>

namespace {
std::string contents(const std::filesystem::path& path) {
   std::ifstream in(path, std::ios::binary);
   return {std::istreambuf_iterator<char>(in), {}};
}
struct fail_sync : eosio::chain::durable_file::sync_operations {
   static void sync(const std::filesystem::path&) { throw std::runtime_error("injected sync failure"); }
};
struct fail_rename : eosio::chain::durable_file::sync_operations {
   static void promote(const std::filesystem::path&, const std::filesystem::path&) {
      throw std::runtime_error("injected rename failure");
   }
};
struct fail_directory_sync : eosio::chain::durable_file::sync_operations {
   static void promote(const std::filesystem::path& from, const std::filesystem::path& to) {
      std::filesystem::rename(from, to);
      throw std::runtime_error("injected directory sync failure");
   }
};
}

BOOST_AUTO_TEST_CASE(durable_replace_preserves_last_generation_and_reports_all_failures) {
   using namespace eosio::chain;
   fc::temp_directory temp;
   const auto path = temp.path() / "metadata.dat";
   const auto write_new = [](std::ofstream& out) { out << "new"; };
   durable_file::replace(path, [](std::ofstream& out) { out << "old"; });
   BOOST_CHECK_THROW(durable_file::replace(path, [](std::ofstream& out) {
      out << "partial";
      throw std::runtime_error("serialization failed");
   }), std::runtime_error);
   BOOST_CHECK_EQUAL(contents(path), "old");
   BOOST_CHECK_THROW(durable_file::replace(path, [](std::ofstream& out) {
      out.setstate(std::ios::badbit);
   }), std::ios_base::failure);
   BOOST_CHECK_EQUAL(contents(path), "old");
   BOOST_CHECK_THROW(durable_file::replace<fail_sync>(path, write_new), std::runtime_error);
   BOOST_CHECK_EQUAL(contents(path), "old");
   BOOST_CHECK_THROW(durable_file::replace<fail_rename>(path, write_new), std::runtime_error);
   BOOST_CHECK_EQUAL(contents(path), "old");
   BOOST_CHECK_THROW(durable_file::replace<fail_directory_sync>(path, write_new), std::runtime_error);
   BOOST_CHECK_EQUAL(contents(path), "new"); // renamed != confirmed durable success
   BOOST_CHECK_EQUAL(std::distance(std::filesystem::directory_iterator(temp.path()),
                                  std::filesystem::directory_iterator()), 1);
}

BOOST_AUTO_TEST_CASE(sync_then_promote_snapshot) {
   fc::temp_directory temp;
   const auto incomplete = temp.path() / ".incomplete.bin";
   const auto pending = temp.path() / ".pending.bin";
   const auto final = temp.path() / "snapshot.bin";
   {
      std::ofstream output;
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output.open(incomplete, std::ios::binary);
      output << "test snapshot";
      output.close();
   }
   BOOST_CHECK_NO_THROW(eosio::chain::snapshot_file::sync(incomplete));
   BOOST_CHECK_NO_THROW(eosio::chain::snapshot_file::rename(incomplete, pending));
   BOOST_CHECK_NO_THROW(eosio::chain::snapshot_file::rename(pending, final));
   BOOST_CHECK(!std::filesystem::exists(incomplete));
   BOOST_CHECK(!std::filesystem::exists(pending));
   BOOST_CHECK_EQUAL(std::filesystem::file_size(final), 13u);
}

BOOST_AUTO_TEST_CASE(missing_snapshot_or_directory_is_not_reported_as_durable) {
   fc::temp_directory temp;
   BOOST_CHECK_THROW(eosio::chain::snapshot_file::sync(temp.path() / "missing"), std::ios_base::failure);
   BOOST_CHECK_THROW(eosio::chain::snapshot_file::rename(temp.path() / "missing", temp.path() / "final"),
                     std::filesystem::filesystem_error);
   BOOST_CHECK(!std::filesystem::exists(temp.path() / "final"));
#ifndef _WIN32
   BOOST_CHECK_THROW(eosio::chain::snapshot_file::sync_directory(temp.path() / "missing"), std::system_error);
#endif
}
