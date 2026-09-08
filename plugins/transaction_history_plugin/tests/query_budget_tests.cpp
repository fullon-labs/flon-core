#include <boost/test/unit_test.hpp>
#include <eosio/transaction_history_plugin/query_budget.hpp>
#include <eosio/transaction_history_plugin/history_json.hpp>
#include <eosio/transaction_history_plugin/transaction_history_plugin.hpp>

BOOST_AUTO_TEST_CASE(history_budget_checks_deadline_during_abi_and_serialization) {
   eosio::history_query_budget expired(fc::time_point::now() - fc::seconds(1), 1024);
   BOOST_CHECK_THROW(expired.parse("{}"), eosio::chain::deadline_exception);
   BOOST_CHECK_THROW(expired.serialized_size(fc::variant("x")), eosio::chain::deadline_exception);
   auto yield = expired.abi_yield(fc::seconds(1));
   BOOST_CHECK_THROW(yield(0), eosio::chain::deadline_exception);
}

BOOST_AUTO_TEST_CASE(history_budget_counts_json_escaping_and_envelope) {
   eosio::history_query_budget budget(fc::time_point::now() + fc::seconds(5), 16);
   BOOST_CHECK_EQUAL(budget.serialized_size(fc::variant("x")), 3u);
   BOOST_CHECK_THROW(budget.serialized_size(fc::variant(std::string(16, '\n'))),
                     eosio::chain::plugin_exception);
   BOOST_CHECK_THROW(budget.parse(std::string(17, ' ')), eosio::chain::plugin_exception);
   BOOST_CHECK_THROW(budget.serialized_size(fc::mutable_variant_object("traces", "123456789")),
                     eosio::chain::plugin_exception);
}

BOOST_AUTO_TEST_CASE(history_legacy_records_keep_unknown_index_completeness) {
   using result_type = eosio::transaction_history_apis::read_only::get_transaction_result;
   result_type legacy;
   fc::mutable_variant_object old_fields(fc::variant(legacy).get_object());
   old_fields.erase("account_index_complete");
   old_fields.erase("history_status");
   auto restored = fc::variant(old_fields).as<result_type>();
   BOOST_CHECK(!restored.account_index_complete.has_value());
   restored.account_index_complete = false;
   auto roundtrip = fc::variant(restored).as<result_type>();
   BOOST_REQUIRE(roundtrip.account_index_complete.has_value());
   BOOST_CHECK(!*roundtrip.account_index_complete);
}

BOOST_AUTO_TEST_CASE(history_json_accepts_legacy_map_arrays_and_new_objects) {
   const std::map<std::string, fc::variant> legacy{{"block_num", 42}, {"action_ref", "act:42"}};
   const fc::variant legacy_value(legacy);
   BOOST_REQUIRE(legacy_value.is_array());
   const fc::variant object(eosio::history_record_object(legacy_value));
   BOOST_REQUIRE(object.is_object());
   BOOST_CHECK_EQUAL(object.get_object()["block_num"].as_uint64(), 42u);
   BOOST_CHECK_EQUAL(eosio::history_record_fields(object).at("action_ref").as_string(), "act:42");
   BOOST_CHECK_EQUAL(eosio::history_record_fields(legacy_value).at("block_num").as_uint64(), 42u);
}
