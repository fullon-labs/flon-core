#include <eosio/history_api_plugin/history_api_plugin.hpp>
#include <eosio/chain/exceptions.hpp>

#include <fc/io/json.hpp>

namespace eosio {

using namespace eosio;

static auto _history_api_plugin = application::register_plugin<history_api_plugin>();

history_api_plugin::history_api_plugin(){}
history_api_plugin::~history_api_plugin(){}

void history_api_plugin::set_program_options(options_description&, options_description&) {}
void history_api_plugin::plugin_initialize(const variables_map&) {}

#define CALL(api_name, api_handle, api_namespace, call_name) \
{std::string("/v1/" #api_name "/" #call_name), \
   [api_handle](string, string body, url_response_callback cb) mutable { \
          try { \
             if (body.empty()) body = "{}"; \
             fc::variant result( api_handle.call_name(fc::json::from_string(body).as<api_namespace::call_name ## _params>()) ); \
             cb(200, std::move(result)); \
          } catch (...) { \
             http_plugin::handle_exception(#api_name, #call_name, body, cb); \
          } \
       }}


   template<>
   history_apis::read_only::get_transaction_params
   parse_params<history_apis::read_only::get_transaction_params, http_params_types::params_required>(const std::string& body) {
      if (body.empty()) {
         EOS_THROW(chain::invalid_http_request, "A Request body is required");
      }
   
      try {
         auto v = fc::json::from_string( body ).as<history_apis::read_only::get_transaction_params>();
         return v;
      } catch( ... ) {
         EOS_THROW(chain::invalid_http_request, "Invalid transaction id");
      }
   }
   
// #define CHAIN_RO_CALL(call_name) CALL(history, ro_api, history_apis::read_only, call_name)
//#define CHAIN_RW_CALL(call_name) CALL(history, rw_api, history_apis::read_write, call_name)

#define INVOKE_V_R(api_handle, call_name, in_param) \
     auto params = parse_params<in_param, http_params_types::params_required>(body);\
     api_handle.call_name(std::move(params)); \
     eosio::detail::producer_api_plugin_response result{"ok"};


#define INVOKE(macro) macro ARGS

#define CALL_WITH_400(api_name, api_handle, call_name, INVOKE, http_response_code) \
{std::string("/v1/" #api_name "/" #call_name), \
   api_category::history_ro, \
   [api_handle](string&&, string&& body, url_response_callback&& cb) mutable { \
            try { \
               INVOKE \
               cb(http_response_code, fc::variant(result)); \
            } catch (...) { \
               http_plugin::handle_exception(#api_name, #call_name, body, cb); \
            } \
       }}

#define INVOKE_R_V(api_handle, call_name) \
     auto result = api_handle->call_name();


#define INVOKE_R_R(api_handle, call_name, in_param) \
     auto params = parse_params<in_param, http_params_types::params_required>(body);\
     auto result = api_handle.call_name( std::move(params) );

#define INVOKE_R_R_R(api_handle, call_name, in_param0, in_param1) \
     const auto& params = parse_params<fc::variants, http_params_types::params_required>(body);\
     if (params.size() != 2) { \
        EOS_THROW(chain::invalid_http_request, "Missing valid input from POST body"); \
     } \
     auto result = api_handle.call_name(params.at(0).as<in_param0>(), params.at(1).as<in_param1>());

// chain_id_type does not have default constructor, keep it unchanged
#define INVOKE_R_R_R_R(api_handle, call_name, in_param0, in_param1, in_param2) \
     const auto& params = parse_params<fc::variants, http_params_types::params_required>(body);\
     if (params.size() != 3) { \
        EOS_THROW(chain::invalid_http_request, "Missing valid input from POST body"); \
     } \
     auto result = api_handle.call_name(params.at(0).as<in_param0>(), params.at(1).as<in_param1>(), params.at(2).as<in_param2>());

#define INVOKE_R_V(api_handle, call_name) \
     body = parse_params<std::string, http_params_types::no_params>(body); \
     auto result = api_handle.call_name();

#define INVOKE_V_R_R(api_handle, call_name, in_param0, in_param1) \
     const auto& params = parse_params<fc::variants, http_params_types::params_required>(body);\
     if (params.size() != 2) { \
        EOS_THROW(chain::invalid_http_request, "Missing valid input from POST body"); \
     } \
     api_handle.call_name(params.at(0).as<in_param0>(), params.at(1).as<in_param1>()); \
     eosio::detail::wallet_api_plugin_empty result;

#define INVOKE_V_R_R_R(api_handle, call_name, in_param0, in_param1, in_param2) \
     const auto& params = parse_params<fc::variants, http_params_types::params_required>(body);\
     if (params.size() != 3) { \
        EOS_THROW(chain::invalid_http_request, "Missing valid input from POST body"); \
     } \
     api_handle.call_name(params.at(0).as<in_param0>(), params.at(1).as<in_param1>(), params.at(2).as<in_param2>()); \
     eosio::detail::wallet_api_plugin_empty result;

#define INVOKE_V_V(api_handle, call_name) \
     body = parse_params<std::string, http_params_types::no_params>(body); \
     api_handle.call_name(); \
     eosio::detail::wallet_api_plugin_empty result;



void history_api_plugin::plugin_startup() {
   ilog( "starting history_api_plugin" );
   auto history_mgr = app().get_plugin<history_plugin>().get_read_only_api();
   

   app().get_plugin<http_plugin>().add_api({

      CALL_WITH_400(history, history_mgr, get_actions,
         INVOKE_R_R(history_mgr, get_actions, history_apis::read_only::get_actions_params), 200),
      
      CALL_WITH_400(history, history_mgr, get_transaction,
         INVOKE_R_R(history_mgr, get_transaction,history_apis::read_only::get_transaction_params), 200),
      
      CALL_WITH_400(history, history_mgr, get_key_accounts,
         INVOKE_R_R(history_mgr, get_key_accounts, history_apis::read_only::get_key_accounts_params), 200),
      
      CALL_WITH_400(history, history_mgr, get_controlled_accounts,
         INVOKE_R_R(history_mgr, get_controlled_accounts, history_apis::read_only::get_controlled_accounts_params), 200),
     
   }, appbase::exec_queue::read_only);

   ilog( "starting history_api_plugin end" );
}

void history_api_plugin::plugin_shutdown() {
   ilog( "starting plugin_shutdown end" );

}

}
#undef INVOKE_R_R
#undef INVOKE_R_V
#undef INVOKE_V_R
#undef INVOKE_V_V
#undef CALL