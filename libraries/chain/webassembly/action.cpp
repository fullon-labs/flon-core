#include <eosio/chain/webassembly/interface.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/global_property_object.hpp>

namespace eosio { namespace chain { namespace webassembly {
   int32_t interface::read_action_data(legacy_span<char> memory) const {
      auto s = context.get_action().data.size();
      auto copy_size = std::min( static_cast<size_t>(memory.size()), s );
      if( copy_size == 0 ) return s;
      std::memcpy( memory.data(), context.get_action().data.data(), copy_size );

      return copy_size;
   }

   int32_t interface::action_data_size() const {
      return context.get_action().data.size();
   }

   name interface::current_receiver() const {
      return context.get_receiver();
   }

   void interface::set_action_return_value( span<const char> packed_blob ) {
      auto max_action_return_value_size =
         context.control.get_global_properties().configuration.max_action_return_value_size;
      if( !context.trx_context.is_read_only() )
         EOS_ASSERT(packed_blob.size() <= max_action_return_value_size,
                    action_return_value_exception,
                    "action return value size must be less or equal to ${s} bytes", ("s", max_action_return_value_size));
      context.action_return_value.assign( packed_blob.data(), packed_blob.data() + packed_blob.size() );
   }


   uint64_t interface::init_action_data_to_json(uint64_t contract_name, uint64_t action_name, legacy_span<const char> action_data, legacy_ptr<uint64_t> json_size) {
      EOS_ASSERT( context.control.is_builtin_activated( builtin_protocol_feature_t::action_data_to_json ),
                  protocol_feature_exception,
                  "The action_data_to_json protocol feature not activated, the init_action_data_to_json() interface not supported");   

      fc::datastream<const char*> ds( action_data.data(), action_data.size() );
      return context.init_action_data_to_json(name(contract_name), name(action_name), ds, *json_size);
   }

   void interface::final_action_data_to_json(const uint64_t convertor_id, legacy_span<char> json_output) {
      EOS_ASSERT( context.control.is_builtin_activated( builtin_protocol_feature_t::action_data_to_json ),
                  protocol_feature_exception,
                  "The action_data_to_json protocol feature not activated, the init_action_data_to_json() interface not supported");   
                  
      context.final_action_data_to_json(convertor_id, json_output.data(), json_output.size());
   }

}}} // ns eosio::chain::webassembly
