#include <eosio/eosio.hpp>
#include "config.hpp"

using namespace eosio;

static const name system_account_name = eosio::name(SYSTEM_ACCOUNT_NAME);

extern "C" {
   void apply( uint64_t receiver, uint64_t first_receiver, uint64_t action ) {
      check( receiver == first_receiver, "rejecting all notifications" );

      // reject all actions with only the following exceptions:
      //   * do not reject an eosio::setcode that sets code on the eosio account unless the rejectall account exists;
      //   * do not reject an eosio::newaccount that creates the rejectall account.

      if( first_receiver == system_account_name.value ) {
         if( action == "setcode"_n.value ) {
            auto accnt = unpack_action_data<name>();
            if( accnt == system_account_name && !is_account("rejectall"_n) )
               return;
         } else if( action == "newaccount"_n.value ) {
            auto accnts = unpack_action_data< std::pair<name, name> >();
            if( accnts.second == "rejectall"_n )
               return;
         }
      }

      check( false , "rejecting all actions" );
   }
}
