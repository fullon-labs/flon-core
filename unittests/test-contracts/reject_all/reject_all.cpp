#include <eosio/eosio.hpp>

using namespace eosio;

#define TO_STRING(x) #x
#define TO_NAME(x) eosio::name{TO_STRING(X)}

#ifdef SYSTEM_ACCOUNT_NAME
   static const name system_account_name = TO_NAME(SYSTEM_ACCOUNT_NAME);
#else
#error "SYSTEM_ACCOUNT_NAME not defined"
#endif

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
