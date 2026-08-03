#pragma once
#include <eosio/transaction_history_plugin/transaction_history_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>

namespace eosio {

using namespace appbase;

/**
 * @brief Transaction History API Plugin
 *
 * Provides HTTP API endpoints for querying transaction history data
 * stored by the transaction_history_plugin.
 */
class transaction_history_api_plugin : public appbase::plugin<transaction_history_api_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((transaction_history_plugin)(http_plugin))

   transaction_history_api_plugin();
   virtual ~transaction_history_api_plugin();

   virtual void set_program_options(options_description&, options_description&) override;
   void plugin_initialize(const variables_map&);
   void plugin_startup();
   void plugin_shutdown();

   static void register_history_routes(http_plugin& http, const std::string& api_name,
                                       const transaction_history_apis::read_only& history_mgr);

private:
};

} // namespace eosio
