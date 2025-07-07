#pragma once
#include <eosio/sign_transaction_plugin/sign_transaction_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>

#include <eosio/chain/application.hpp>
#include <eosio/chain/controller.hpp>

namespace eosio {
   using eosio::chain::controller;
   using std::unique_ptr;
   using namespace appbase;

   class sign_transaction_api_plugin : public plugin<sign_transaction_api_plugin> {
      public:
        APPBASE_PLUGIN_REQUIRES((sign_transaction_plugin)(http_plugin))

        sign_transaction_api_plugin();
        virtual ~sign_transaction_api_plugin();

        virtual void set_program_options(options_description&, options_description&) override;

        void plugin_initialize(const variables_map&);
        void plugin_startup();
        void plugin_shutdown();
   };

}
