#include <eosio/chain/abi_serializer.hpp>
#include <eosio/testing/tester.hpp>
#include <eosio/chain/contract_table_objects.hpp>

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace eosio::testing;
using namespace eosio;
using namespace eosio::chain;
using namespace eosio::testing;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

// only check on error
#define CHECK_REQUIRE_ERROR(exp) if (!(exp)) { BOOST_ERROR(BOOST_TEST_STRINGIZE(exp)); }

template<typename T>
T& mutable_ref(const T& t) {
   return const_cast<T&>(t);
}

template<typename T>
class contract_table_tester : public T {
public:

   contract_table_tester() {
      T::produce_block();

      T::create_accounts( { "alice"_n, "bob"_n, "carol"_n, config::token_account_name } );
      T::produce_block();

      T::set_code( config::token_account_name, test_contracts::eosio_token_wasm() );
      T::set_abi( config::token_account_name, test_contracts::eosio_token_abi() );

      T::produce_block();

      const auto& accnt = T::control->db().template get<account_object,by_name>( config::token_account_name );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
      abi_ser.set_abi(std::move(abi), abi_serializer::create_yield_function( T::abi_serializer_max_time ));
   }

   T::action_result push_action( const account_name& signer, const action_name &name, const variant_object &data ) {
      string action_type_name = abi_ser.get_action_type(name);

      action act;
      act.account = config::token_account_name;
      act.name    = name;
      act.data    = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function( T::abi_serializer_max_time ) );

      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   fc::variant get_stats( const string& symbolname )
   {
      auto symb = eosio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;
      vector<char> data = T::get_row_by_account( config::token_account_name, name(symbol_code), "stat"_n, name(symbol_code) );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "currency_stats", data, abi_serializer::create_yield_function( T::abi_serializer_max_time ) );
   }

   fc::variant get_account( account_name acc, const string& symbolname)
   {
      auto symb = eosio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;
      vector<char> data = T::get_row_by_account( config::token_account_name, acc, "accounts"_n, name(symbol_code) );
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant( "account", data, abi_serializer::create_yield_function( T::abi_serializer_max_time ) );
   }

   asset get_balance( account_name acc, const symbol& symb) {
      const chainbase::database& db = T::control->db();

      const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::token_account_name, "alice"_n, "accounts"_n ));
      CHECK_REQUIRE_ERROR(t_id != nullptr);
      const auto &idx = db.get_index<key_value_index, by_scope_primary>();
      auto it = idx.find(boost::make_tuple( t_id->id, symb.to_symbol_code() ));
      CHECK_REQUIRE_ERROR( it != idx.end() );

      asset balance;
      CHECK_REQUIRE_ERROR( it->value.size() >= fc::raw::pack_size(balance) );
      fc::datastream<const char *> ds(it->value.data(), it->value.size());
      fc::raw::unpack(ds, balance);
      return balance;
   }

   T::action_result create( account_name issuer,
                asset        maximum_supply ) {

      return push_action( config::token_account_name, "create"_n, mvo()
           ( "issuer", issuer)
           ( "maximum_supply", maximum_supply)
      );
   }

   T::action_result issue( account_name issuer, account_name to, asset quantity, string memo ) {
      return push_action( issuer, "issue"_n, mvo()
           ( "to", to)
           ( "quantity", quantity)
           ( "memo", memo)
      );
   }

   T::action_result transfer( account_name from,
                  account_name to,
                  asset        quantity,
                  string       memo ) {
      return push_action( from, "transfer"_n, mvo()
           ( "from", from)
           ( "to", to)
           ( "quantity", quantity)
           ( "memo", memo)
      );
   }

   abi_serializer abi_ser;
};

using contract_table_testers = boost::mpl::list<contract_table_tester<legacy_tester>,
                                             contract_table_tester<savanna_tester>>;

BOOST_AUTO_TEST_SUITE(contract_table_tests)


BOOST_AUTO_TEST_CASE_TEMPLATE( transfer_tests, T, contract_table_testers ) try {
   T chain;

   auto token = chain.create( "alice"_n, asset::from_string("1000 CERO"));
   chain.produce_block();

   chain.issue( "alice"_n, "alice"_n, asset::from_string("1000 CERO"), "hola" );

   auto stats = chain.get_stats("0,CERO");
   REQUIRE_MATCHING_OBJECT( stats, mvo()
      ("supply", "1000 CERO")
      ("max_supply", "1000 CERO")
      ("issuer", "alice")
   );

   auto symb = eosio::chain::symbol::from_string("0,CERO");
   auto alice_balance = chain.get_account("alice"_n, "0,CERO");
   REQUIRE_MATCHING_OBJECT( alice_balance, mvo()
      ("balance", "1000 CERO")
   );

   chainbase::database& db = (chainbase::database&)chain.control->db();

   const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>(boost::make_tuple( config::token_account_name, "alice"_n, "accounts"_n ));
   BOOST_REQUIRE( t_id != nullptr );

   const auto &idx = db.get_index<key_value_index, by_scope_primary>();
   auto it = idx.find(boost::make_tuple( t_id->id, symb.to_symbol_code() ));
   BOOST_REQUIRE( it != idx.end() );

   asset balance;
   BOOST_REQUIRE_GE( it->value.size(), fc::raw::pack_size(balance) );
   fc::datastream<const char *> ds(it->value.data(), it->value.size());
   fc::raw::unpack(ds, balance);
   BOOST_REQUIRE( balance == asset::from_string("1000 CERO") );

   // auto& mutable_idx = mutable_ref(idx);
   // mutable_idx.modify(*it, [&](auto& a) {
   db.modify(*it, [&](auto& a) {
      auto new_value = fc::raw::pack(asset::from_string("999 CERO"));
      a.value.resize_and_fill(new_value.size(), [&](char* data, std::size_t size) {
         memcpy(data, new_value.data(), size);
      });
   });

   fc::datastream<const char *> ds2(it->value.data(), it->value.size());
   fc::raw::unpack(ds2, balance);
   BOOST_REQUIRE( balance == asset::from_string("999 CERO") );

   BOOST_REQUIRE( balance == chain.get_balance("alice"_n, symb) );

   auto new_balance = chain.get_account("alice"_n, "0,CERO");
   REQUIRE_MATCHING_OBJECT( new_balance, mvo()
      ("balance", "999 CERO")
   );

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
