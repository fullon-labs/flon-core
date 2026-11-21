#include "pending_submissions.hpp"
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/account_object.hpp>
#include <eosio/chain/asset.hpp>
#include <eosio/chain/trace.hpp>
#include <eosio/http_plugin/http_plugin.hpp>
#include <eosio/http_plugin/macros.hpp>
#include <eosio/signature_provider_plugin/signature_provider_plugin.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <optional>
#include <set>

namespace eosio {
using namespace chain;

struct interop_action {
   name contract;
   name action;
   bytes data;
};
}

FC_REFLECT(eosio::interop_action, (contract)(action)(data))

namespace eosio {
namespace {
using namespace interop_relayer_detail;

constexpr uint32_t signing_lifetime = 120;
constexpr size_t max_request_size = 128 * 1024;

struct operation_route {
   name action;
   std::string address_field;
   std::string signature_field;
   std::vector<std::string> wallet_fields;
};

struct route {
   name source;
   name signature_type;
   std::string wallet_signing;
   operation_route bind;
   std::optional<operation_route> execute;
};

const std::vector<route> default_routes = {
   {"eth"_n, "secp"_n, "eth_personal", {"bindeth"_n, "l1_address", "signature", {"msg_header","l1_pubkey","l2_pubkey"}}, operation_route{"pusheth"_n, "address", "signature", {"msg_header"}}},
   {"bsc"_n, "secp"_n, "eth_personal", {"bindbsc"_n, "l1_address", "signature", {"msg_header","l1_pubkey","l2_pubkey"}}, operation_route{"pushbsc"_n, "address", "signature", {"msg_header"}}},
   {"arb"_n, "secp"_n, "eth_personal", {"bindarb"_n, "l1_address", "signature", {"msg_header","l1_pubkey","l2_pubkey"}}, operation_route{"pusharb"_n, "address", "signature", {"msg_header"}}},
   {"tron"_n, "secp"_n, "tron_signed_message", {"bindtron"_n, "l1_address", "signature", {"msg_header","l1_pubkey","l2_pubkey"}}, operation_route{"pushtron"_n, "address", "signature", {"msg_header"}}},
   {"btc"_n, "secp"_n, "bitcoin_signed_message", {"bindbtc"_n, "l1_address", "signature", {"msg_header","l1_pubkey","l2_pubkey"}}, std::nullopt},
   {"solana"_n, "ed"_n, "solana_sign_message", {"bindsol"_n, "address", "signature", {}}, operation_route{"pushsol"_n, "address", "signature", {}}},
   {"ton"_n, "ed"_n, "ton_proof", {"bindton"_n, "l1_address", "l1_signature", {"msg_header1","msg_header2","l1_pubkey","l1_chain_id","l1_version","l1_revision","l1_bounce","l1_testnet"}}, std::nullopt},
   {"statter"_n, "rsa"_n, "statter_rsa_md5", {"bindstatter"_n, "l1_address", "l1_signature", {"l1_pubkey"}}, operation_route{"pushstatter"_n, "l1_address", "l1_signature", {}}}
};

bool valid_field(const std::string& field) {
   return !field.empty() && field.size() <= 32 &&
          field.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}

operation_route load_operation(const fc::variant& value, const std::string& prefix) {
   const auto& obj = value.get_object();
   const auto& action = obj["action"].get_string();
   EOS_ASSERT(obj.size() == 4 && action.size() > prefix.size() && action.rfind(prefix, 0) == 0 &&
              name(action).to_string() == action,
              plugin_config_exception, "Invalid interop action for operation");
   operation_route result{name(action), obj["address_field"].get_string(), obj["signature_field"].get_string(), {}};
   std::set<std::string> occupied{"miner", "account", "expires_at", "nonce", "txns"};
   const auto add_field = [&](const std::string& field) {
      EOS_ASSERT(valid_field(field) && occupied.insert(field).second, plugin_config_exception,
                 "Invalid, duplicate, or reserved interop argument field ${field}", ("field", field));
   };
   add_field(result.address_field);
   add_field(result.signature_field);
   const auto& fields = obj["wallet_fields"].get_array();
   EOS_ASSERT(fields.size() <= 32, plugin_config_exception, "Too many interop wallet fields");
   for (const auto& field : fields) {
      add_field(field.get_string());
      result.wallet_fields.push_back(field.get_string());
   }
   return result;
}

struct route_config {
   std::vector<route> routes;
};

std::string protocol_name(name protocol) {
   return "fullon-interop-" + protocol.to_string();
}

route_config load_routes(const std::filesystem::path& path) {
   // A missing optional file retains compatibility with existing deployments.
   if (!std::filesystem::exists(std::filesystem::symlink_status(path)))
      return {default_routes};

   EOS_ASSERT(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) <= max_request_size,
              plugin_config_exception, "Interop route config is not a regular JSON file of at most 128 KiB");
   const auto root = fc::json::from_file(path, fc::json::parse_type::strict_parser);
   const auto& config = root.get_object();
   const auto& version = config["schema_version"];
   // Accept the retired JSON field so existing route files still load.
   EOS_ASSERT(config.size() == (config.contains("signing_protocol") ? 3 : 2) &&
              (version.get_type() == fc::variant::uint64_type || version.get_type() == fc::variant::int64_type) &&
              version.as_uint64() == 1,
              plugin_config_exception, "Unsupported interop route schema");
   const auto& configured = config["routes"].get_object();
   EOS_ASSERT(configured.size() > 0 && configured.size() <= 64, plugin_config_exception, "Expected 1 to 64 interop routes");
   std::vector<route> result;
   std::set<name> actions;
   for (const auto& entry : configured) {
      const auto source = name(entry.key());
      const auto& obj = entry.value().get_object();
      auto kind = name(obj["signature_type"].get_string());
      auto signing = obj["wallet_signing"].get_string();
      EOS_ASSERT(obj.size() == 4 && source != name{} && source.to_string() == entry.key() &&
                 (kind == "secp"_n || kind == "ed"_n || kind == "rsa"_n) && !signing.empty() && signing.size() <= 64 &&
                 std::none_of(result.begin(), result.end(), [&](const auto& r) { return r.source == source; }),
                 plugin_config_exception, "Invalid or duplicate interop source: ${source}", ("source", entry.key()));
      auto bind = load_operation(obj["bind"], "bind");
      std::optional<operation_route> execute;
      if (!obj["execute"].is_null())
         execute = load_operation(obj["execute"], "push");
      EOS_ASSERT(actions.insert(bind.action).second && (!execute || actions.insert(execute->action).second),
                 plugin_config_exception, "Interop actions cannot be shared by multiple routes");
      result.push_back({source, kind, std::move(signing), std::move(bind), std::move(execute)});
   }
   return {std::move(result)};
}

template<typename F>
decltype(auto) parse_input(F&& parse) {
   try {
      return parse();
   } catch (const invalid_http_request&) {
      throw;
   } catch (const fc::exception&) {
      EOS_THROW(invalid_http_request, "Missing or malformed interop request field");
   }
}

fc::variant_object parse_request(const std::string& body) {
   EOS_ASSERT(!body.empty() && body.size() <= max_request_size, invalid_http_request, "Invalid interop request size");
   return parse_input([&] { return fc::variant_object(fc::json::from_string(body).get_object()); });
}

uint64_t unsigned_field(const fc::variant& value) {
   if (value.is_string()) {
      const auto& text = value.get_string();
      uint64_t result = 0;
      const auto end = text.data() + text.size();
      const auto parsed = std::from_chars(text.data(), end, result);
      EOS_ASSERT(parsed.ec == std::errc{} && parsed.ptr == end, invalid_http_request, "Invalid unsigned integer");
      return result;
   }
   EOS_ASSERT(value.get_type() == fc::variant::uint64_type ||
              (value.get_type() == fc::variant::int64_type && value.as_int64() >= 0),
              invalid_http_request, "Expected an unsigned integer");
   return value.as_uint64();
}

struct request_route {
   const route& selected;
   bool bind;
   std::string address;

   const operation_route& operation() const { return bind ? selected.bind : *selected.execute; }
};

request_route select_route(const std::vector<route>& routes, const fc::variant_object& request) {
   const auto operation = request["operation"].get_string();
   EOS_ASSERT(operation == "bind" || operation == "execute", invalid_http_request, "Invalid interop operation");
   const bool bind = operation == "bind";
   const auto source = name(request["source"].get_string());
   const auto address = request["address"].get_string();
   EOS_ASSERT(!address.empty() && address.size() <= 128, invalid_http_request, "Invalid external address length");
   for (const auto& entry : routes) {
      if (entry.source == source && (bind || entry.execute.has_value()))
         return {entry, bind, address};
   }
   EOS_THROW(invalid_http_request, "Unsupported interop route");
}

struct proxy_config {
   digest_type chain_id;
   name owner;
   name ton_verifier;
   name signing_protocol;
};

struct binding {
   name account;
   digest_type id;
   uint64_t nonce;
   name sig_type;
   name status;
};

void validate_binding(const request_route& r, const std::optional<binding>& current) {
   if (r.bind && current)
      throw request_error{409, "already_bound", "Address is already bound"};
   if (!r.bind && !current)
      throw request_error{409, "not_bound", "Address is not bound"};
   if (!r.bind) {
      if (current->status != "active"_n)
         throw request_error{409, "binding_inactive", "Binding is not active"};
      EOS_ASSERT(current->sig_type == r.selected.signature_type, invalid_http_request, "Route does not match the binding signature type");
   }
}

void schedule_expiry(const std::shared_ptr<boost::asio::steady_timer>& timer,
                     const std::weak_ptr<pending_submissions>& pending) {
   timer->expires_after(std::chrono::milliseconds(250));
   timer->async_wait([timer, pending](const boost::system::error_code& error) {
      if (!error) {
         if (const auto state = pending.lock()) {
            state->expire();
            schedule_expiry(timer, pending);
         }
      }
   });
}

} // namespace

class interop_relayer_api_plugin : public appbase::plugin<interop_relayer_api_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin)(http_plugin)(signature_provider_plugin))

   void set_program_options(options_description&, options_description& cfg) override {
      cfg.add_options()
         ("interop-contract", boost::program_options::value<std::string>()->default_value("interopproxy"), "Interop proxy contract account")
         ("interop-signing-protocol", boost::program_options::value<std::string>()->default_value("v1"), "Interop signing protocol name; must match the proxy contract")
         ("interop-relayer-permission", boost::program_options::value<std::string>(), "Dedicated relayer account@permission")
         ("interop-signature-provider", boost::program_options::value<std::string>(), "Signature provider for the relayer permission")
         ("interop-signing-threads", boost::program_options::value<uint32_t>()->default_value(2), "Dedicated interop signer threads (1–8)")
         ("interop-max-in-flight", boost::program_options::value<uint32_t>()->default_value(32), "Maximum pending interop submissions (1–256)")
         ("interop-submit-tcp", boost::program_options::bool_switch(), "Allow interop submission over TCP (use an authenticated, rate-limited gateway)");
   }

   void plugin_initialize(const variables_map& opts) {
      proxy = chain::name(opts.at("interop-contract").as<std::string>());
      const auto protocol = opts.at("interop-signing-protocol").as<std::string>();
      try {
         signing_protocol = chain::name(protocol);
      } catch (const fc::exception&) {
         EOS_THROW(plugin_config_exception, "interop-signing-protocol must be a valid FullOn name");
      }
      EOS_ASSERT(signing_protocol.value != 0 && signing_protocol.to_string() == protocol,
                 plugin_config_exception, "interop-signing-protocol must be a non-empty canonical FullOn name");
      const auto path = app().full_config_file_path().parent_path() / "interop-routes.json";
      try {
         routing = load_routes(path);
      } catch (const std::exception& error) {
         EOS_THROW(plugin_config_exception, "Cannot load interop routes from ${path}: ${error}",
                   ("path", path.string())("error", error.what()));
      }
      tcp_submit = opts.at("interop-submit-tcp").as<bool>();
      EOS_ASSERT(opts.count("interop-relayer-permission") == opts.count("interop-signature-provider"),
                 plugin_config_exception, "Configure both interop relayer permission and signature provider");
      if (!opts.count("interop-relayer-permission"))
         return;
      const auto value = opts.at("interop-relayer-permission").as<std::string>();
      const auto sep = value.find('@');
      EOS_ASSERT(sep != std::string::npos && sep > 0 && sep + 1 < value.size(),
                 plugin_config_exception, "Use account@permission for the interop relayer");
      permission = {chain::name(value.substr(0, sep)), chain::name(value.substr(sep + 1))};
      EOS_ASSERT(permission.actor != proxy && permission.permission != "owner"_n,
                 plugin_config_exception, "Use a separate relayer account and a non-owner permission");
      auto provider = app().get_plugin<signature_provider_plugin>().signature_provider_for_specification(
         opts.at("interop-signature-provider").as<std::string>());
      EOS_ASSERT(provider, plugin_config_exception, "Interop requires a transaction signature provider");
      signer = std::move(provider->second);
      const auto threads = opts.at("interop-signing-threads").as<uint32_t>();
      const auto limit = opts.at("interop-max-in-flight").as<uint32_t>();
      EOS_ASSERT(threads >= 1 && threads <= 8 && limit >= 1 && limit <= 256,
                 plugin_config_exception, "Interop signer threads or pending submission limit out of range");
      const auto& provider_spec = opts.at("interop-signature-provider").as<std::string>();
      if (std::get<1>(signature_provider_plugin::parse_signature_provider_spec(provider_spec)) == "WALLET") {
         const auto timeout = opts.at("wallet-provider-timeout").as<int32_t>();
         EOS_ASSERT(timeout > 0 && timeout <= 30000, plugin_config_exception, "Interop wallet signer requires a timeout of 1–30000 ms");
      }
      pending = std::make_shared<pending_submissions>();
      pending->limit = limit;
      signing_pool = std::make_unique<boost::asio::thread_pool>(threads);
   }

   void plugin_startup() {
      auto& http = app().get_plugin<http_plugin>();
      node = &app().get_plugin<chain_plugin>();
      response_time = http.get_max_response_time();
      if (pending) {
         expiry_timer = std::make_shared<boost::asio::steady_timer>(app().get_io_service());
         schedule_expiry(expiry_timer, pending);
      }
      http.add_api({
         {"/v1/interop/get_routes", api_category::chain_ro,
          [this](auto&&, auto&&, auto&& cb) {
             try {
                cb(200, get_routes());
             } catch (const request_error& error) {
                send_error(cb, error);
             } catch (...) {
                http_plugin::handle_exception("interop", "get_routes", "", cb);
             }
          }},
         {"/v1/interop/prepare", api_category::chain_ro,
          [this](auto&&, auto&& body, auto&& cb) {
             try {
                cb(200, prepare(parse_request(body)));
             } catch (const request_error& error) {
                send_error(cb, error);
             } catch (const invalid_action_args_exception&) {
                send_error(cb, {400, "invalid_action_args", "Invalid arguments for the selected action"});
             } catch (...) {
                http_plugin::handle_exception("interop", "prepare", "", cb);
             }
          }},
         {"/v1/interop/submit", api_category::chain_rw, {}, !tcp_submit,
          [this](auto&&, auto&& body, auto&& cb, http_response_deadline transport_deadline) {
             try {
                submit(parse_request(body), cb, transport_deadline);
             } catch (const request_error& error) {
                send_error(cb, error);
             } catch (const invalid_action_args_exception&) {
                send_error(cb, {400, "invalid_action_args", "Invalid arguments for the selected action"});
             } catch (...) {
                http_plugin::handle_exception("interop", "submit", "", cb);
             }
          }}
      }, appbase::exec_queue::read_write);
   }

   void plugin_shutdown() {
      if (signing_pool) {
         if (expiry_timer)
            expiry_timer->cancel();
         signing_pool->stop();
         signing_pool->join();
         pending->shutdown();
      }
   }

private:
   chain::name proxy;
   chain::name signing_protocol;
   route_config routing;
   permission_level permission;
   bool tcp_submit = false;
   chain_plugin* node = nullptr;
   fc::microseconds response_time;
   signature_provider_plugin::signature_provider_type signer;
   std::shared_ptr<pending_submissions> pending;
   std::unique_ptr<boost::asio::thread_pool> signing_pool;
   std::shared_ptr<boost::asio::steady_timer> expiry_timer;

   struct route_validation_cache {
      digest_type abi_hash;
      digest_type code_hash;
      std::set<chain::name> enabled_actions;
      bool requires_verifier_pin = false;
   };
   mutable std::optional<route_validation_cache> validation_cache;

   static bool action_matches_abi(const abi_def& abi, const route& r, const operation_route& selected,
                                  bool bind) {
      const auto action = std::find_if(abi.actions.begin(), abi.actions.end(),
                                      [&](const auto& a) { return a.name == selected.action; });
      if (action == abi.actions.end())
         return false;
      const auto type = std::find_if(abi.structs.begin(), abi.structs.end(),
                                    [&](const auto& t) { return t.name == action->type; });
      std::map<std::string, std::string> expected{
         {"miner", "name"}, {"expires_at", "uint32"}, {selected.address_field, "string"},
         {selected.signature_field, r.signature_type == "secp"_n ? "signature" : "bytes"}
      };
      if (bind) {
         expected.emplace("account", "name");
      } else {
         expected.emplace("txns", "l2_tx_t[]");
         expected.emplace("nonce", "uint64");
      }
      for (const auto& field : selected.wallet_fields)
         expected.emplace(field, ""); // The ABI serializer validates the route-specific wallet fields.
      if (type == abi.structs.end() || !type->base.empty() || type->fields.size() != expected.size())
         return false;
      for (const auto& f : type->fields) {
         const auto it = expected.find(f.name);
         if (it == expected.end() || (!it->second.empty() && it->second != f.type))
            return false;
         expected.erase(it);
      }
      if (!bind) {
         const auto tx = std::find_if(abi.structs.begin(), abi.structs.end(),
                                      [](const auto& t) { return t.name == "l2_tx_t"; });
         if (tx == abi.structs.end() || !tx->base.empty() ||
             tx->fields != std::vector<field_def>{{"contract","name"},{"action","name"},{"data","bytes"}})
            return false;
      }
      return true;
   }

   struct contract_route {
      chain::name source;
      chain::name signature_type;
      chain::name bind;
      chain::name execute;
   };

   std::vector<contract_route> manifest(const chain_apis::read_only& api, fc::time_point deadline,
                                        const abi_def& abi, const digest_type& code_hash) const {
      const auto table = std::find_if(abi.tables.begin(), abi.tables.end(),
                                      [](const auto& t) { return t.name == "routemap"_n; });
      std::vector<contract_route> result;
      if (table == abi.tables.end())
         throw request_error{503, "route_manifest_missing", "The proxy does not publish an interop route manifest"};

      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = proxy;
      p.scope = proxy.to_string();
      p.table = "routemap"_n;
      p.limit = 1;
      const auto rows = table_rows(api, p, deadline);
      if (rows.size() != 1)
         throw request_error{503, "route_manifest_missing", "Publish the deployed proxy routes with syncroutes"};
      const auto& row = rows.front().get_object();
      if (row["code_hash"].as<digest_type>() != code_hash)
         throw request_error{503, "route_manifest_stale", "Publish the route manifest for the current proxy code"};
      for (const auto& item : row["routes"].get_array()) {
         const auto& entry = item.get_object();
         result.push_back({entry["source"].as<chain::name>(), entry["signature_type"].as<chain::name>(),
                           entry["bind_action"].as<chain::name>(), entry["execute_action"].as<chain::name>()});
      }
      return result;
   }

   const std::set<chain::name>& validated_actions(const chain_apis::read_only& api, fc::time_point deadline) const {
      const auto raw = api.get_raw_abi({proxy, validation_cache ? std::optional{validation_cache->abi_hash} : std::nullopt}, deadline);
      if (validation_cache && raw.abi_hash == validation_cache->abi_hash && raw.code_hash == validation_cache->code_hash)
         return validation_cache->enabled_actions;
      const auto parsed = api.get_abi({proxy}, deadline);
      if (!parsed.abi)
         throw request_error{503, "route_abi_mismatch", "The proxy contract ABI is missing"};
      const auto deployed = manifest(api, deadline, *parsed.abi, raw.code_hash);

      route_validation_cache verified{raw.abi_hash, raw.code_hash, {},
         std::any_of(parsed.abi->tables.begin(), parsed.abi->tables.end(),
                     [](const auto& table) { return table.name == "verifierpins"_n; })};
      for (const auto& r : routing.routes) {
         const auto found = std::find_if(deployed.begin(), deployed.end(), [&](const auto& entry) {
            return entry.source == r.source && entry.signature_type == r.signature_type;
         });
         if (found == deployed.end())
            continue;
         if (r.bind.action == found->bind && action_matches_abi(*parsed.abi, r, r.bind, true))
            verified.enabled_actions.insert(r.bind.action);
         if (r.execute && r.execute->action == found->execute && action_matches_abi(*parsed.abi, r, *r.execute, false))
            verified.enabled_actions.insert(r.execute->action);
      }
      validation_cache = std::move(verified);
      return validation_cache->enabled_actions;
   }

   void validate_action_abi(const chain_apis::read_only& api, fc::time_point deadline,
                            const request_route& request) const {
      if (!validated_actions(api, deadline).contains(request.operation().action))
         throw request_error{503, "route_abi_mismatch", "Interop route does not match the deployed proxy manifest and ABI"};
   }

   static fc::variants scoped_table(const chain_apis::read_only& api, fc::time_point deadline,
                                    chain::name account, chain::name table, chain::name key = {}) {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = account;
      p.scope = account.to_string();
      p.table = table;
      p.limit = 1;
      if (key != chain::name{}) {
         p.key_type = "name";
         p.lower_bound = p.upper_bound = key.to_string();
      }
      return table_rows(api, p, deadline);
   }

   std::optional<request_error> binding_policy(const chain_apis::read_only& api, fc::time_point deadline,
                                               chain::name owner) const {
      const auto owner_abi = api.get_abi({owner}, deadline);
      const auto missing = request_error{503, "bind_policy_unconfigured", "Owner account creation policy is not configured"};
      if (!owner_abi.abi)
         return missing;
      const bool has_policy = std::any_of(owner_abi.abi->tables.begin(), owner_abi.abi->tables.end(),
                                         [](const auto& t) { return t.name == "bindpolicy"_n; });
      if (!has_policy) {
         // The verified original testnet interopowner predates the subsidy policy.
         static const digest_type legacy_owner(std::string("3d68c3dd55fc4f728c2fc50ace17db5d28ff9943232c0a3cf633120cf5a80b06"));
         if (api.get_code_hash({owner}, deadline).code_hash == legacy_owner)
            return std::nullopt;
         return missing;
      }
      const auto policy = scoped_table(api, deadline, owner, "bindpolicy"_n);
      if (policy.empty())
         return missing;
      const auto& row = policy.front().get_object();
      if (row["paused"].as_bool())
         return request_error{503, "bindings_paused", "New account bindings are paused"};
      const auto globals = scoped_table(api, deadline, owner, "global"_n);
      if (globals.empty())
         return missing;
      const auto subsidy = globals.front().get_object()["gas_quant"].as<asset>();
      const auto limit = row["daily_limit"].as<asset>();
      const auto spent = row["spent"].as<asset>();
      const auto& chain = node->chain();
      const auto block_time = chain.is_building_block() ? chain.pending_block_time() : chain.head().block_time();
      const auto day = fc::time_point_sec(block_time).sec_since_epoch() / 86400;
      const auto used = row["day"].as_uint64() == day ? spent.get_amount() : 0;
      const symbol expected(8, "FLON");
      if (subsidy.get_symbol() != expected || limit.get_symbol() != expected || spent.get_symbol() != expected ||
          subsidy.get_amount() <= 0 || limit.get_amount() < 0 || used < 0)
         return missing;
      if (used > limit.get_amount() || subsidy.get_amount() > limit.get_amount() - used)
         return request_error{503, "binding_subsidy_exhausted", "The daily account creation subsidy is exhausted"};
      return std::nullopt;
   }

   std::optional<request_error> verifier_readiness(const chain_apis::read_only& api, fc::time_point deadline,
                                                   const proxy_config& cfg, chain::name source) const {
      if (source != "ton"_n)
         return std::nullopt;
      const auto missing = request_error{503, "verifier_not_ready", "The TON verifier is missing or its code has not been approved"};
      if (cfg.ton_verifier == chain::name{} || !account_exists(cfg.ton_verifier))
         return missing;
      const auto hash = api.get_code_hash({cfg.ton_verifier}, deadline).code_hash;
      if (hash == digest_type{})
         return missing;
      if (!validation_cache->requires_verifier_pin)
         return std::nullopt;
      const auto pins = scoped_table(api, deadline, proxy, "verifierpins"_n, "ton"_n);
      if (pins.empty())
         return missing;
      const auto& pin = pins.front().get_object();
      if (pin["chain"].as<chain::name>() != "ton"_n || pin["contract"].as<chain::name>() != cfg.ton_verifier ||
          pin["code_hash"].as<digest_type>() != hash)
         return missing;
      return std::nullopt;
   }

   void validate_binding_readiness(const chain_apis::read_only& api, fc::time_point deadline,
                                   const proxy_config& cfg, const request_route& request) const {
      if (!request.bind)
         return;
      if (const auto error = binding_policy(api, deadline, cfg.owner))
         throw *error;
      if (const auto error = verifier_readiness(api, deadline, cfg, request.selected.source))
         throw *error;
   }

   fc::variant get_routes() const {
      auto api = node->get_read_only_api(response_time);
      const auto deadline = api.start();
      const auto cfg = config(api, deadline);
      const auto& enabled = validated_actions(api, deadline);
      fc::mutable_variant_object advertised;
      const bool has_bind = std::any_of(routing.routes.begin(), routing.routes.end(),
                                       [&](const auto& r) { return enabled.contains(r.bind.action); });
      const auto policy = has_bind ? binding_policy(api, deadline, cfg.owner) : std::nullopt;
      const auto describe = [&](const operation_route& op, std::optional<request_error> readiness = std::nullopt) -> fc::variant {
         if (!enabled.contains(op.action))
            return {};
         fc::mutable_variant_object operation;
         operation("action", op.action)("wallet_fields", op.wallet_fields)("ready", !readiness);
         if (readiness)
            operation("readiness_code", readiness->code)("readiness_message", readiness->message);
         return fc::variant(operation);
      };
      for (const auto& r : routing.routes) {
         auto readiness = policy;
         if (enabled.contains(r.bind.action) && !readiness)
            readiness = verifier_readiness(api, deadline, cfg, r.source);
         auto bind = describe(r.bind, readiness);
         auto execute = r.execute ? describe(*r.execute) : fc::variant{};
         if (!bind.is_null() || !execute.is_null())
            advertised(r.source.to_string(), fc::variant(fc::mutable_variant_object()
               ("signature_type", r.signature_type)("wallet_signing", r.wallet_signing)
               ("bind", std::move(bind))("execute", std::move(execute))));
      }
      return fc::variant(fc::mutable_variant_object()("chain_id", cfg.chain_id)("proxy_contract", proxy)
         ("owner_contract", cfg.owner)("signing_protocol", protocol_name(cfg.signing_protocol))
         ("submission_enabled", bool(signer) && node->api_accept_transactions())("routes", advertised));
   }

   proxy_config config(const chain_apis::read_only& api, fc::time_point deadline) const {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = proxy;
      p.scope = proxy.to_string();
      p.table = "global"_n;
      p.limit = 1;
      const auto rows = table_rows(api, p, deadline);
      EOS_ASSERT(rows.size() == 1, invalid_http_request, "Interop proxy is not initialized");
      const auto& row = rows.front().get_object();
      EOS_ASSERT(row.contains("chain_id"), invalid_http_request, "Interop proxy chain ID is not configured");
      auto chain_id = row["chain_id"].as<digest_type>();
      EOS_ASSERT(chain_id == node->get_chain_id(), invalid_http_request, "Interop proxy chain ID differs from this node");
      auto deployed_protocol = "v1"_n;
      if (row.contains("signing_protocol") && !row["signing_protocol"].is_null())
         deployed_protocol = row["signing_protocol"].as<chain::name>();
      if (deployed_protocol != signing_protocol)
         throw request_error{503, "signing_protocol_mismatch", "Configured signing protocol differs from the proxy contract"};
      chain::name verifier;
      const auto field = row.contains("address_verifier_contracts") ? "address_verifier_contracts" : "pubkey_check_contracts";
      if (row.contains(field)) {
         for (const auto& item : row[field].get_array()) {
            const auto& entry = item.get_object();
            if (entry["first"].as<chain::name>() == "ton"_n) {
               verifier = entry["second"].as<chain::name>();
               break;
            }
         }
      }
      return {chain_id, row["owner_contract"].as<chain::name>(), verifier, deployed_protocol};
   }

   static fc::variants table_rows(const chain_apis::read_only& api,
                                  const chain_apis::read_only::get_table_rows_params& p, fc::time_point deadline) {
      auto result = api.get_table_rows(p, deadline)();
      if (auto error = std::get_if<fc::exception_ptr>(&result))
         (*error)->dynamic_rethrow_exception();
      return std::move(std::get<chain_apis::read_only::get_table_rows_result>(result).rows);
   }

   std::optional<binding> find_binding(const chain_apis::read_only& api, fc::time_point deadline,
                                       chain::name owner, const std::string& address) const {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = owner;
      p.scope = owner.to_string();
      p.table = "proxyaccts"_n;
      p.index_position = "2";
      p.key_type = "sha256";
      p.encode_type = "hex";
      p.lower_bound = p.upper_bound = fc::sha256::hash(address).str();
      p.limit = 1;
      const auto rows = table_rows(api, p, deadline);
      if (rows.empty())
         return std::nullopt;
      const auto& row = rows.front().get_object();
      EOS_ASSERT(row["address"].as_string() == address, invalid_http_request, "Interop binding address mismatch");
      return binding{row["account"].as<chain::name>(), row["bind_txid"].as<digest_type>(), row["nonce"].as_uint64(),
                     row["sig_type"].as<chain::name>(), row["status"].as<chain::name>()};
   }

   bool account_exists(chain::name account) const {
      return node->chain().db().find<account_object, by_name>(account) != nullptr;
   }

   chain::name assign_account(const proxy_config& cfg, const std::string& address, uint32_t expires) const {
      constexpr char alphabet[] = "12345abcdefghijklmnopqrstuvwxyz";
      for (uint8_t attempt = 0; attempt < 8; ++attempt) {
         const auto seed = fc::raw::pack(std::make_tuple(cfg.chain_id, proxy, address, expires, attempt));
         const auto hash = fc::sha256::hash(seed.data(), seed.size());
         std::string candidate = "i";
         for (size_t i = 0; i < 11; ++i)
            candidate += alphabet[static_cast<unsigned char>(hash.data()[i]) % (sizeof(alphabet) - 1)];
         const chain::name account(candidate);
         if (!account_exists(account))
            return account;
      }
      EOS_THROW(invalid_http_request, "Could not allocate an interop account");
   }

   static std::vector<interop_action> actions_from(const fc::variant_object& request, fc::time_point deadline,
                                                    const chain_apis::read_only* abi_api = nullptr) {
      const auto& txns = parse_input([&]() -> const fc::variants& { return request["txns"].get_array(); });
      EOS_ASSERT(!txns.empty() && txns.size() <= 16, invalid_http_request, "Expected 1 to 16 interop actions");
      std::vector<interop_action> result;
      for (const auto& item : txns) {
         const auto obj = parse_input([&] { return fc::variant_object(item.get_object()); });
         const auto contract = parse_input([&] { return chain::name(obj["contract"].get_string()); });
         const auto action = parse_input([&] { return chain::name(obj["action"].get_string()); });
         EOS_ASSERT(contract != chain::name{} && action != chain::name{}, invalid_http_request, "Missing action contract or name");
         const auto& input = parse_input([&]() -> const fc::variant& { return obj["data"]; });
         bytes data;
         if (input.is_string()) {
            const auto& hex = input.get_string();
            EOS_ASSERT(hex.size() % 2 == 0 && hex.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos,
                       invalid_http_request, "Action data must be even-length hex");
            data.resize(hex.size() / 2);
            if (!data.empty())
               fc::from_hex(hex, data.data(), data.size());
         } else {
            EOS_ASSERT(abi_api && input.is_object(), invalid_http_request, "Action data must be hex or, when preparing, an ABI object");
            data = abi_api->abi_json_to_bin({contract, action, input}, deadline).binargs;
         }
         result.push_back({contract, action, std::move(data)});
      }
      return result;
   }

   fc::variant prepare(const fc::variant_object& request) const {
      const auto r = parse_input([&] { return select_route(routing.routes, request); });
      auto api = node->get_read_only_api(response_time);
      const auto deadline = api.start();
      const auto cfg = config(api, deadline);
      validate_action_abi(api, deadline, r);
      auto existing = find_binding(api, deadline, cfg.owner, r.address);
      validate_binding(r, existing);
      validate_binding_readiness(api, deadline, cfg, r);
      const uint32_t expires = fc::time_point_sec(fc::time_point::now()).sec_since_epoch() + signing_lifetime;
      const auto account = r.bind ? assign_account(cfg, r.address, expires) : existing->account;
      const auto id = r.bind ? digest_type{} : existing->id;
      const auto nonce = r.bind ? uint64_t{0} : existing->nonce;
      const auto txns = r.bind ? std::vector<interop_action>{} : actions_from(request, deadline, &api);
      const auto operation = r.bind ? "bind"_n : "execute"_n;
      const auto packed = fc::raw::pack(std::make_tuple(cfg.chain_id, proxy, cfg.owner, operation, r.selected.source,
                                                       account, r.address, expires, id, nonce, txns));
      fc::mutable_variant_object args;
      args("operation", operation)("source", r.selected.source)("address", r.address)("account", account)("expires_at", expires);
      if (!r.bind)
         args("binding_id", id)("nonce", std::to_string(nonce))("txns", txns);
      return fc::variant(fc::mutable_variant_object()
         ("signing_message", "FullOn Interop " + cfg.signing_protocol.to_string() + ":" + fc::to_hex(packed))
         ("expires_at", expires)("chain_id", cfg.chain_id)("proxy_contract", proxy)
         ("owner_contract", cfg.owner)("signing_protocol", protocol_name(cfg.signing_protocol))
         ("wallet_signing", r.selected.wallet_signing)("submit_args", args));
   }

   void submit(const fc::variant_object& request, const url_response_callback& cb,
               http_response_deadline transport_deadline) const {
      if (relay_deadline(transport_deadline, http_response_deadline::max()) <= pending_submissions::clock::now())
         throw request_error{504, "request_deadline", "No HTTP response budget remains; transaction was not submitted"};
      if (!signer || !node->api_accept_transactions())
         throw request_error{503, "submission_disabled", "Interop submission is disabled"};
      const auto r = parse_input([&] { return select_route(routing.routes, request); });
      auto api = node->get_read_only_api(response_time);
      const auto deadline = api.start();
      const auto cfg = config(api, deadline);
      validate_action_abi(api, deadline, r);
      EOS_ASSERT(permission.actor != cfg.owner, plugin_config_exception, "Relayer must not be the owner contract");
      const auto existing = find_binding(api, deadline, cfg.owner, r.address);
      validate_binding(r, existing);
      validate_binding_readiness(api, deadline, cfg, r);
      const auto expires = parse_input([&] { return unsigned_field(request["expires_at"]); });
      const auto now = fc::time_point_sec(fc::time_point::now()).sec_since_epoch();
      if (expires <= now)
         throw request_error{410, "signature_expired", "Signing deadline has passed; prepare again"};
      EOS_ASSERT(expires - now <= signing_lifetime, invalid_http_request, "Invalid signing deadline");
      const auto account = parse_input([&] { return chain::name(request["account"].get_string()); });
      fc::mutable_variant_object args;
      const auto& operation = r.operation();
      args("miner", permission.actor)(operation.address_field, r.address)
          ("expires_at", expires)(operation.signature_field, parse_input([&] { return std::string(request["signature"].get_string()); }));
      if (r.bind) {
         if (account != assign_account(cfg, r.address, static_cast<uint32_t>(expires)))
            throw request_error{409, "assigned_account_changed", "Assigned account changed or is unavailable; prepare again"};
         args("account", account);
      } else {
         const auto txns = actions_from(request, deadline);
         const auto nonce = parse_input([&] { return unsigned_field(request["nonce"]); });
         const auto binding_id = parse_input([&] { return digest_type(request["binding_id"].get_string()); });
         if (existing->account != account || existing->nonce != nonce || existing->id != binding_id)
            throw request_error{409, "binding_changed", "Interop binding or nonce changed; prepare again"};
         args("txns", txns)("nonce", existing->nonce);
      }
      const auto& fields = operation.wallet_fields;
      if (fields.size() != 0) {
         const auto wallet_fields = parse_input([&] { return fc::variant_object(request["wallet_fields"].get_object()); });
         for (const auto& field : fields)
            args(field, parse_input([&] { return wallet_fields[field]; }));
      }

      const auto action_name = operation.action;
      auto data = api.abi_json_to_bin({proxy, action_name, fc::variant(args)}, deadline).binargs;
      signed_transaction trx;
      trx.expiration = fc::time_point_sec(std::min<uint64_t>(expires, uint64_t{now} + 30));
      trx.set_reference_block(node->chain().head().id());
      trx.actions.emplace_back(std::vector<permission_level>{permission}, proxy, action_name, data);
      const auto digest = trx.sig_digest(node->get_chain_id(), trx.context_free_data);
      const pending_key key{cfg.owner, r.bind, r.address, r.bind ? 0 : existing->nonce};
      const auto fingerprint = digest_type::hash(trx.actions.front());
      const auto remaining = trx.expiration.to_time_point() - fc::time_point::now();
      const auto steady_now = pending_submissions::clock::now();
      const auto deadline_for_result = relay_deadline(transport_deadline,
         steady_now + std::chrono::microseconds(remaining.count()));
      if (deadline_for_result <= steady_now)
         throw request_error{504, "request_deadline", "No response budget remains; transaction was not submitted"};
      const auto generation = pending->reserve(key, fingerprint, trx.id(), deadline_for_result, cb);
      if (!generation)
         return;
      const url_response_callback complete = [pending = pending, key, generation = *generation](int status, std::optional<fc::variant> result) {
         pending->finish(key, generation, status, result);
      };
      try {
         boost::asio::post(*signing_pool, [trx = std::move(trx), sign = signer, digest, complete, pending = pending, key, generation = *generation]() mutable {
            try {
               const auto check_deadline = [&] {
                  if (trx.expiration.to_time_point() <= fc::time_point::now())
                     throw request_error{410, "submission_expired", "Submission expired while awaiting signature; prepare again"};
               };
               if (!pending->active(key, generation))
                  return;
               check_deadline();
               trx.signatures.push_back(sign(digest));
               if (!pending->active(key, generation))
                  return;
               check_deadline();
               auto packed = std::make_shared<packed_transaction>(std::move(trx));
               app().get_method<chain::plugin_interface::incoming::methods::transaction_async>()(
                  packed, true, transaction_metadata::trx_type::input, false,
                  [id = packed->id(), complete](const auto& result) {
                     try {
                        if (auto error = std::get_if<fc::exception_ptr>(&result))
                           (*error)->dynamic_rethrow_exception();
                        const auto& trace = std::get<transaction_trace_ptr>(result);
                        if (trace->except)
                           trace->except->dynamic_rethrow_exception();
                        complete(202, fc::variant(fc::mutable_variant_object()("transaction_id", id)));
                     } catch (...) {
                        http_plugin::handle_exception("interop", "submit", "", complete);
                     }
                  });
            } catch (const request_error& error) {
               send_error(complete, error);
            } catch (...) {
               http_plugin::handle_exception("interop", "submit", "", complete);
            }
         });
      } catch (...) {
         http_plugin::handle_exception("interop", "submit", "", complete);
      }
   }
};

static auto registered_interop_relayer_api = appbase::application::register_plugin<interop_relayer_api_plugin>();

} // namespace eosio
