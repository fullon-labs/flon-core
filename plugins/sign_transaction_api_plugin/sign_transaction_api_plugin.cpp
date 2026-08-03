#include <eosio/sign_transaction_api_plugin/sign_transaction_api_plugin.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/http_plugin/macros.hpp>
#include <fc/time.hpp>
#include <fc/io/json.hpp>


namespace eosio {

namespace sign_transaction_apis = eosio::sign_transaction::chain_apis;

static auto _sign_transaction_api_plugin = application::register_plugin<sign_transaction_api_plugin>();

using namespace eosio;

namespace {
api_entry unix_socket_only(api_entry entry) {
   entry.unix_socket_only = true;
   return entry;
}
}


sign_transaction_api_plugin::sign_transaction_api_plugin() = default;
sign_transaction_api_plugin::~sign_transaction_api_plugin() = default;

void sign_transaction_api_plugin::set_program_options(options_description&, options_description&) {}
void sign_transaction_api_plugin::plugin_initialize(const variables_map&) {}

#define CALL_WITH_400(api_name, category, api_handle, api_namespace, call_name, http_response_code, params_type) \
{std::string("/v1/" #api_name "/" #call_name), \
   api_category::category,\
   [api_handle](string&&, string&& body, url_response_callback&& cb) mutable { \
          auto deadline = api_handle.start(); \
          try { \
             auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);\
             fc::variant result( api_handle.call_name( std::move(params), deadline ) ); \
             cb(http_response_code, std::move(result)); \
          } catch (...) { \
             http_plugin::handle_exception(#api_name, #call_name, body, cb); \
          } \
       }}

#define CHAIN_RW_CALL(call_name, http_response_code, params_type) CALL_WITH_400(sign_transaction, chain_rw, rw_api, sign_transaction_apis::read_write, call_name, http_response_code, params_type)
#define CHAIN_RW_CALL_ASYNC(call_name, call_result, http_response_code, params_type) CALL_ASYNC_WITH_400(sign_transaction, chain_rw, rw_api, sign_transaction_apis::read_write, call_name, call_result, http_response_code, params_type)

void sign_transaction_api_plugin::plugin_startup() {
   ilog( "starting sign_transaction_api_plugin" );
   auto& sign_trx_plug = app().get_plugin<sign_transaction_plugin>();
   auto& _http_plugin = app().get_plugin<http_plugin>();
   fc::microseconds max_response_time = _http_plugin.get_max_response_time();

   auto rw_api = sign_trx_plug.get_read_write_api(max_response_time);
   _http_plugin.add_api({
      // transaction related APIs will be posted to read_write queue after keys are recovered, they are safe to run in parallel until they post to the read_write queue
      unix_socket_only(CHAIN_RW_CALL_ASYNC(push_transaction, sign_transaction_apis::read_write::push_transaction_results, 202, http_params_types::params_required)),
      unix_socket_only(CHAIN_RW_CALL_ASYNC(push_transactions, sign_transaction_apis::read_write::push_transactions_results, 202, http_params_types::params_required)),
      unix_socket_only(CHAIN_RW_CALL_ASYNC(send_transaction, sign_transaction_apis::read_write::send_transaction_results, 202, http_params_types::params_required)),
      unix_socket_only(CHAIN_RW_CALL_ASYNC(send_transaction2, sign_transaction_apis::read_write::send_transaction_results, 202, http_params_types::params_required))
   }, appbase::exec_queue::read_only);

}

void sign_transaction_api_plugin::plugin_shutdown() {}

}
