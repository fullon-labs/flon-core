#include <eosio/transaction_history_api_plugin/transaction_history_api_plugin.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/http_plugin/macros.hpp>
#include <fc/io/json.hpp>

namespace eosio {

using namespace eosio;

static auto _transaction_history_api_plugin = application::register_plugin<transaction_history_api_plugin>();

transaction_history_api_plugin::transaction_history_api_plugin() {}
transaction_history_api_plugin::~transaction_history_api_plugin() {}

void transaction_history_api_plugin::set_program_options(options_description&, options_description&) {}
void transaction_history_api_plugin::plugin_initialize(const variables_map&) {}

// Template specializations for parameter parsing
template<>
transaction_history_apis::read_only::get_transaction_params
parse_params<transaction_history_apis::read_only::get_transaction_params, http_params_types::params_required>(const std::string& body) {
   if (body.empty()) {
      EOS_THROW(chain::invalid_http_request, "A Request body is required");
   }

   try {
      auto v = fc::json::from_string(body).as<transaction_history_apis::read_only::get_transaction_params>();
      return v;
   } catch(...) {
      EOS_THROW(chain::invalid_http_request, "Invalid transaction id");
   }
}

template<>
transaction_history_apis::read_only::get_actions_params
parse_params<transaction_history_apis::read_only::get_actions_params, http_params_types::params_required>(const std::string& body) {
   if (body.empty()) {
      EOS_THROW(chain::invalid_http_request, "A Request body is required");
   }

   try {
      auto v = fc::json::from_string(body).as<transaction_history_apis::read_only::get_actions_params>();
      return v;
   } catch(...) {
      EOS_THROW(chain::invalid_http_request, "Invalid get_actions parameters");
   }
}

template<>
transaction_history_apis::read_only::get_transaction_count_params
parse_params<transaction_history_apis::read_only::get_transaction_count_params, http_params_types::possible_no_params>(const std::string& body) {
   if (body.empty()) {
      return transaction_history_apis::read_only::get_transaction_count_params{};
   }

   try {
      auto v = fc::json::from_string(body).as<transaction_history_apis::read_only::get_transaction_count_params>();
      return v;
   } catch(...) {
      EOS_THROW(chain::invalid_http_request, "Invalid get_transaction_count parameters");
   }
}

template<>
transaction_history_apis::read_only::get_key_accounts_params
parse_params<transaction_history_apis::read_only::get_key_accounts_params, http_params_types::params_required>(const std::string& body) {
   if (body.empty()) {
      EOS_THROW(chain::invalid_http_request, "A Request body is required");
   }

   try {
      auto v = fc::json::from_string(body).as<transaction_history_apis::read_only::get_key_accounts_params>();
      return v;
   } catch(...) {
      EOS_THROW(chain::invalid_http_request, "Invalid get_key_accounts parameters");
   }
}

template<>
transaction_history_apis::read_only::get_controlled_accounts_params
parse_params<transaction_history_apis::read_only::get_controlled_accounts_params, http_params_types::params_required>(const std::string& body) {
   if (body.empty()) {
      EOS_THROW(chain::invalid_http_request, "A Request body is required");
   }

   try {
      auto v = fc::json::from_string(body).as<transaction_history_apis::read_only::get_controlled_accounts_params>();
      return v;
   } catch(...) {
      EOS_THROW(chain::invalid_http_request, "Invalid get_controlled_accounts parameters");
   }
}

#define INVOKE_R_R(api_handle, call_name, in_param) \
     auto params = parse_params<in_param, http_params_types::params_required>(body);\
     auto result = api_handle.call_name( std::move(params) );

#define INVOKE_R_R_OPTIONAL(api_handle, call_name, in_param) \
     auto params = parse_params<in_param, http_params_types::possible_no_params>(body);\
     auto result = api_handle.call_name( std::move(params) );

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

void transaction_history_api_plugin::plugin_startup() {
   ilog("Starting transaction_history_api_plugin");

   auto history_mgr = app().get_plugin<transaction_history_plugin>().get_read_only_api();
   auto& _http_plugin = app().get_plugin<http_plugin>();

   _http_plugin.add_api({
      CALL_WITH_400(transaction_history, history_mgr, get_transaction,
         INVOKE_R_R(history_mgr, get_transaction, transaction_history_apis::read_only::get_transaction_params), 200),

      CALL_WITH_400(transaction_history, history_mgr, get_actions,
         INVOKE_R_R(history_mgr, get_actions, transaction_history_apis::read_only::get_actions_params), 200),

      CALL_WITH_400(transaction_history, history_mgr, get_transaction_count,
         INVOKE_R_R_OPTIONAL(history_mgr, get_transaction_count, transaction_history_apis::read_only::get_transaction_count_params), 200),

      CALL_WITH_400(transaction_history, history_mgr, get_key_accounts,
         INVOKE_R_R(history_mgr, get_key_accounts, transaction_history_apis::read_only::get_key_accounts_params), 200),

      CALL_WITH_400(transaction_history, history_mgr, get_controlled_accounts,
         INVOKE_R_R(history_mgr, get_controlled_accounts, transaction_history_apis::read_only::get_controlled_accounts_params), 200),

   }, appbase::exec_queue::read_only);

   ilog("Transaction history API plugin started successfully");
}

void transaction_history_api_plugin::plugin_shutdown() {
   ilog("Transaction history API plugin shutdown");
}

} // namespace eosio

#undef INVOKE_R_R
#undef INVOKE_R_R_OPTIONAL
#undef CALL_WITH_400
