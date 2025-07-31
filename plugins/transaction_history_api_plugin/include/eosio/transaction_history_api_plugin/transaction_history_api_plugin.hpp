#pragma once
#include <eosio/transaction_history_plugin/transaction_history_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>
#include <appbase/application.hpp>

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
   virtual void plugin_initialize(const variables_map&) override;
   virtual void plugin_startup() override;
   virtual void plugin_shutdown() override;

private:
};

} // namespace eosio
