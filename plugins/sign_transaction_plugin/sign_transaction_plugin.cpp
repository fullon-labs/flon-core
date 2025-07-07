#include <eosio/sign_transaction_plugin/sign_transaction_plugin.hpp>

#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/controller.hpp>

#include <fc/variant.hpp>
#include <cstdlib>

namespace eosio {

using namespace eosio;
using namespace eosio::chain;
using namespace eosio::chain::config;
using namespace eosio::chain::plugin_interface;
using vm_type = wasm_interface::vm_type;
using fc::flat_map;

using eosio::chain_apis::trx_retry_db;

static auto _sign_transaction_plugin = application::register_plugin<sign_transaction_plugin>();

class sign_transaction_plugin_impl {
public:
   sign_transaction_plugin_impl()
   {}

   chain_plugin* chain_plug = nullptr;
   producer_plugin* producer_plug = nullptr;
   const std::map<chain::public_key_type, signature_provider_type>* signature_providers = nullptr;

   flat_set<public_key_type> provider_keys;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

private:
   static void log_guard_exception(const chain::guard_exception& e);
};

sign_transaction_plugin::sign_transaction_plugin()
:my(new sign_transaction_plugin_impl()) {
}

sign_transaction_plugin::~sign_transaction_plugin() = default;

void sign_transaction_plugin::set_program_options(options_description& cli, options_description& cfg)
{}

void sign_transaction_plugin_impl::plugin_initialize(const variables_map& options) {
   try {
      ilog("initializing sign transaction plugin");

      chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT(chain_plug, plugin_config_exception, "chain_plugin not found" );

      producer_plug = app().find_plugin<producer_plugin>();
      EOS_ASSERT(producer_plug, plugin_config_exception, "producer_plugin not found" );

      signature_providers = &producer_plug->get_signature_providers();
      for (const auto& sp : *signature_providers) {
         provider_keys.insert(sp.first);
      }

   } FC_LOG_AND_RETHROW()
}

void sign_transaction_plugin::plugin_initialize(const variables_map& options) {
   my->plugin_initialize(options);
}

void sign_transaction_plugin_impl::plugin_startup()
{}

void sign_transaction_plugin::plugin_startup() {
   my->plugin_startup();
}

void sign_transaction_plugin_impl::plugin_shutdown() {
}

void sign_transaction_plugin::plugin_shutdown() {
   my->plugin_shutdown();
}

sign_transaction::chain_apis::read_write sign_transaction_plugin::get_read_write_api(const fc::microseconds& http_max_response_time) {
   return sign_transaction::chain_apis::read_write(
      *my, chain(),
      my->chain_plug->get_trx_retry_db(),
      get_abi_serializer_max_time(),
      http_max_response_time,
      api_accept_transactions()
   );
}


controller& sign_transaction_plugin::chain() { return my->chain_plug->chain(); }
const controller& sign_transaction_plugin::chain() const { return my->chain_plug->chain(); }

fc::microseconds sign_transaction_plugin::get_abi_serializer_max_time() const {
   return my->chain_plug->get_abi_serializer_max_time();
}

bool sign_transaction_plugin::api_accept_transactions() const{
   return my->chain_plug->api_accept_transactions();
}

bool sign_transaction_plugin::accept_transactions() const {
   return my->chain_plug->accept_transactions();
}


void sign_transaction_plugin_impl::log_guard_exception(const chain::guard_exception&e ) {
   if (e.code() == chain::database_guard_exception::code_value) {
      elog("Database has reached an unsafe level of usage, shutting down to avoid corrupting the database.  "
           "Please increase the value set for \"chain-state-db-size-mb\" and restart the process!");
   }

   dlog("Details: ${details}", ("details", e.to_detail_string()));
}

namespace sign_transaction {
namespace chain_apis {

read_write::read_write(sign_transaction_plugin_impl& my,
                                   controller& chain,
                                   std::optional<trx_retry_db>& trx_retry,
                                   const fc::microseconds& abi_serializer_max_time,
                                   const fc::microseconds& http_max_response_time,
                                   bool api_accept_transactions)
: my(my)
, chain(chain)
, trx_retry(trx_retry)
, abi_serializer_max_time(abi_serializer_max_time)
, http_max_response_time(http_max_response_time)
, api_accept_transactions(api_accept_transactions)
{
}

void read_write::validate() const {
   EOS_ASSERT( api_accept_transactions, missing_chain_api_plugin_exception,
               "Not allowed, node has api-accept-transactions = false" );
}

void read_write::push_transaction(const read_write::push_transaction_params& params, next_function<read_write::push_transaction_results> next) {
   try {
      auto pretty_input = std::make_shared<packed_transaction>();
      auto resolver = caching_resolver(make_resolver(chain, abi_serializer_max_time, throw_on_yield::yes));
      try {
         abi_serializer::from_variant(params, *pretty_input, resolver, abi_serializer_max_time);
      } EOS_RETHROW_EXCEPTIONS(chain::packed_transaction_type_exception, "Invalid packed transaction")

      const signed_transaction& strx = pretty_input->get_signed_transaction();
      auto required_keys_set = chain.get_authorization_manager().get_required_keys( strx, my.provider_keys, fc::seconds( strx.delay_sec ));
      if (required_keys_set.size() > 0) {
         const auto& chain_id = chain.get_chain_id();
         auto digest = strx.sig_digest(chain_id, strx.context_free_data);
         signed_transaction new_strx(strx);

         for (const auto& pk : required_keys_set) {
            auto itr = my.signature_providers->find( pk );
            EOS_ASSERT( itr != my.signature_providers->end(), producer_priv_key_not_found, "Private key not found ${k}", ("k", pk));
            auto sig = itr->second(digest);
            new_strx.signatures.push_back(sig);
         }

         pretty_input = std::make_shared<packed_transaction>(std::move(new_strx));
      }

      app().get_method<incoming::methods::transaction_async>()(pretty_input, true, transaction_metadata::trx_type::input, false,
            [this, next](const next_function_variant<transaction_trace_ptr>& result) -> void {
         if (std::holds_alternative<fc::exception_ptr>(result)) {
            next(std::get<fc::exception_ptr>(result));
         } else {
            auto trx_trace_ptr = std::get<transaction_trace_ptr>(result);

            try {
               fc::variant output;
               try {
                  auto resolver = get_serializers_cache(chain, trx_trace_ptr, abi_serializer_max_time);
                  abi_serializer::to_variant(*trx_trace_ptr, output, resolver, abi_serializer_max_time);

                  // Create map of (closest_unnotified_ancestor_action_ordinal, global_sequence) with action trace
                  std::map< std::pair<uint32_t, uint64_t>, fc::mutable_variant_object > act_traces_map;
                  for( const auto& act_trace : output["action_traces"].get_array() ) {
                     if (act_trace["receipt"].is_null() && act_trace["except"].is_null()) continue;
                     auto closest_unnotified_ancestor_action_ordinal =
                           act_trace["closest_unnotified_ancestor_action_ordinal"].as<fc::unsigned_int>().value;
                     auto global_sequence = act_trace["receipt"].is_null() ?
                                                std::numeric_limits<uint64_t>::max() :
                                                act_trace["receipt"]["global_sequence"].as<uint64_t>();
                     act_traces_map.emplace( std::make_pair( closest_unnotified_ancestor_action_ordinal,
                                                             global_sequence ),
                                             act_trace.get_object() );
                  }

                  std::function<vector<fc::variant>(uint32_t)> convert_act_trace_to_tree_struct =
                  [&](uint32_t closest_unnotified_ancestor_action_ordinal) {
                     vector<fc::variant> restructured_act_traces;
                     auto it = act_traces_map.lower_bound(
                                 std::make_pair( closest_unnotified_ancestor_action_ordinal, 0)
                     );
                     for( ;
                        it != act_traces_map.end() && it->first.first == closest_unnotified_ancestor_action_ordinal; ++it )
                     {
                        auto& act_trace_mvo = it->second;

                        auto action_ordinal = act_trace_mvo["action_ordinal"].as<fc::unsigned_int>().value;
                        act_trace_mvo["inline_traces"] = convert_act_trace_to_tree_struct(action_ordinal);
                        if (act_trace_mvo["receipt"].is_null()) {
                           act_trace_mvo["receipt"] = fc::mutable_variant_object()
                              ("abi_sequence", 0)
                              ("act_digest", digest_type::hash(trx_trace_ptr->action_traces[action_ordinal-1].act))
                              ("auth_sequence", flat_map<account_name,uint64_t>())
                              ("code_sequence", 0)
                              ("global_sequence", 0)
                              ("receiver", act_trace_mvo["receiver"])
                              ("recv_sequence", 0);
                        }
                        restructured_act_traces.push_back( std::move(act_trace_mvo) );
                     }
                     return restructured_act_traces;
                  };

                  fc::mutable_variant_object output_mvo(std::move(output.get_object()));
                  output_mvo["action_traces"] = convert_act_trace_to_tree_struct(0);

                  output = std::move(output_mvo);
               } catch( chain::abi_exception& ) {
                  output = *trx_trace_ptr;
               }

               const chain::transaction_id_type& id = trx_trace_ptr->id;
               next(read_write::push_transaction_results{id, output});
            } CATCH_AND_CALL(next);
         }
      });
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

static void push_recurse(read_write* rw, int index, const std::shared_ptr<read_write::push_transactions_params>& params, const std::shared_ptr<read_write::push_transactions_results>& results, const next_function<read_write::push_transactions_results>& next) {
   auto wrapped_next = [=](const next_function_variant<read_write::push_transaction_results>& result) {
      if (std::holds_alternative<fc::exception_ptr>(result)) {
         const auto& e = std::get<fc::exception_ptr>(result);
         results->emplace_back( read_write::push_transaction_results{ transaction_id_type(), fc::mutable_variant_object( "error", e->to_detail_string() ) } );
      } else if (std::holds_alternative<read_write::push_transaction_results>(result)) {
         const auto& r = std::get<read_write::push_transaction_results>(result);
         results->emplace_back( r );
      } else {
         assert(0);
      }

      size_t next_index = index + 1;
      if (next_index < params->size()) {
         push_recurse(rw, next_index, params, results, next );
      } else {
         next(*results);
      }
   };

   rw->push_transaction(params->at(index), wrapped_next);
}

void read_write::push_transactions(const read_write::push_transactions_params& params, next_function<read_write::push_transactions_results> next) {
   try {
      EOS_ASSERT( params.size() <= 1000, too_many_tx_at_once, "Attempt to push too many transactions at once" );
      auto params_copy = std::make_shared<read_write::push_transactions_params>(params.begin(), params.end());
      auto result = std::make_shared<read_write::push_transactions_results>();
      result->reserve(params.size());

      push_recurse(this, 0, params_copy, result, next);
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

// called from read-exclusive thread for read-only
template<class API, class Result>
void api_base::send_transaction_gen(API &api, send_transaction_params_t params, next_function<Result> next) {
   try {
      auto ptrx = std::make_shared<packed_transaction>();
      auto resolver = caching_resolver(make_resolver(api.chain, api.abi_serializer_max_time, throw_on_yield::yes));
      try {
         abi_serializer::from_variant(params.transaction, *ptrx, resolver, api.abi_serializer_max_time);
      } EOS_RETHROW_EXCEPTIONS(packed_transaction_type_exception, "Invalid packed transaction")

      bool retry = false;
      std::optional<uint16_t> retry_num_blocks;

      if constexpr (std::is_same_v<API, read_write>) {
         retry = params.retry_trx;
         retry_num_blocks = params.retry_trx_num_blocks;

         EOS_ASSERT( !retry || api.trx_retry.has_value(), unsupported_feature, "Transaction retry not enabled on node. transaction-retry-max-storage-size-gb is 0" );
         EOS_ASSERT( !retry || (ptrx->expiration() <= api.trx_retry->get_max_expiration_time()), tx_exp_too_far_exception,
                     "retry transaction expiration ${e} larger than allowed ${m}",
                     ("e", ptrx->expiration())("m", api.trx_retry->get_max_expiration_time()) );
      }

      const signed_transaction& strx = ptrx->get_signed_transaction();
      auto required_keys_set = api.chain.get_authorization_manager().get_required_keys( strx, api.my.provider_keys, fc::seconds( strx.delay_sec ));
      if (required_keys_set.size() > 0) {
         const auto& chain_id = api.chain.get_chain_id();
         auto digest = strx.sig_digest(chain_id, strx.context_free_data);
         signed_transaction new_strx(strx);

         for (const auto& pk : required_keys_set) {
            auto itr = api.my.signature_providers->find( pk );
            EOS_ASSERT( itr != api.my.signature_providers->end(), producer_priv_key_not_found, "Private key not found ${k}", ("k", pk));
            auto sig = itr->second(digest);
            new_strx.signatures.push_back(sig);
         }

         ptrx = std::make_shared<packed_transaction>(new_strx);
      }

      app().get_method<incoming::methods::transaction_async>()(ptrx, true, params.trx_type, params.return_failure_trace,
            [&api, ptrx, next, retry, retry_num_blocks](const next_function_variant<transaction_trace_ptr>& result) -> void {
            if( std::holds_alternative<fc::exception_ptr>( result ) ) {
               next( std::get<fc::exception_ptr>( result ) );
            } else {
               try {
                  auto trx_trace_ptr = std::get<transaction_trace_ptr>( result );
                  bool retried = false;
                  if constexpr (std::is_same_v<API, read_write>) {
                     if( retry && api.trx_retry.has_value() && !trx_trace_ptr->except) {
                        // will be ack'ed via next later
                        api.trx_retry->track_transaction( ptrx, retry_num_blocks,
                             [ptrx, next](const next_function_variant<std::unique_ptr<fc::variant>>& result ) {
                                if( std::holds_alternative<fc::exception_ptr>( result ) ) {
                                   next( std::get<fc::exception_ptr>( result ) );
                                } else {
                                   fc::variant& output = *std::get<std::unique_ptr<fc::variant>>( result );
                                   next( Result{ptrx->id(), std::move( output )} );
                                }
                             } );
                        retried = true;
                     }
                  }
                  else {
                     (void)retry; // ref variable to avoid compilation warning
                     (void)retry_num_blocks; // ref variable to avoid compilation warning
                  }
                  if (!retried) {
                     // we are still on main thread here. The lambda passed to `next()` below will be executed on the http thread pool
                     using return_type = t_or_exception<Result>;
                     next([&api,
                           trx_trace_ptr,
                           resolver = get_serializers_cache(api.chain, trx_trace_ptr, api.abi_serializer_max_time)]() mutable {
                        try {
                           fc::variant output;
                           try {
                              abi_serializer::to_variant(*trx_trace_ptr, output, resolver, api.abi_serializer_max_time);
                           } catch( abi_exception& ) {
                              output = *trx_trace_ptr;
                           }
                           const transaction_id_type& id = trx_trace_ptr->id;
                           return return_type(Result{id, std::move( output )});
                        } CATCH_AND_RETURN(return_type);
                     });
                  }
               } CATCH_AND_CALL( next );
            }
         });
   } catch ( boost::interprocess::bad_alloc& ) {
      handle_db_exhaustion();
   } catch ( const std::bad_alloc& ) {
      handle_bad_alloc();
   } CATCH_AND_CALL(next);
}

void read_write::send_transaction(read_write::send_transaction_params params, next_function<read_write::send_transaction_results> next) {
   send_transaction_params_t gen_params { .return_failure_trace = false,
                                          .retry_trx            = false,
                                          .retry_trx_num_blocks = std::nullopt,
                                          .trx_type             = transaction_metadata::trx_type::input,
                                          .transaction          = std::move(params) };
   return send_transaction_gen(*this, std::move(gen_params), std::move(next));
}

void read_write::send_transaction2(read_write::send_transaction2_params params, next_function<read_write::send_transaction_results> next) {
   send_transaction_params_t gen_params  { .return_failure_trace = params.return_failure_trace,
                                           .retry_trx            = params.retry_trx,
                                           .retry_trx_num_blocks = std::move(params.retry_trx_num_blocks),
                                           .trx_type             = transaction_metadata::trx_type::input,
                                           .transaction          = std::move(params.transaction) };
   return send_transaction_gen(*this, std::move(gen_params), std::move(next));
}

} // namespace chain_apis
} // namespace sign_transaction

} // namespace eosio::sign_transaction
