#include <eosio/chain/webassembly/interface.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/block_summary_object.hpp>

namespace eosio { namespace chain { namespace webassembly {
   /* these are both unfortunate that we didn't make the return type an int64_t */
   uint64_t interface::current_time() const {
      return static_cast<uint64_t>( context.control.pending_block_time().time_since_epoch().count() );
   }

   uint64_t interface::publication_time() const {
      return static_cast<uint64_t>( context.trx_context.published.time_since_epoch().count() );
   }

   bool interface::is_feature_activated( legacy_ptr<const digest_type> feature_digest ) const {
      return context.control.is_protocol_feature_activated( *feature_digest );
   }

   name interface::get_sender() const {
      return context.get_sender();
   }

   uint32_t interface::get_block_num() const {
      return context.control.pending_block_num();
   }

   bool interface::get_recent_block_id(uint32_t block_num, legacy_ptr<block_id_type> block_id) const {
      if (block_num == 0 || block_num >= context.control.pending_block_num()) {
         return false;
      }

      const auto& summary = context.db.get<block_summary_object>(static_cast<uint16_t>(block_num));
      if (block_header::num_from_id(summary.block_id) != block_num) {
         return false;
      }

      *block_id = summary.block_id;
      return true;
   }

   uint32_t interface::get_last_irreversible_block_num() const {
      // The fork database root is node-local and can be different while producing,
      // validating, or replaying the same block. The parent block's consensus state
      // is identical in all three cases.
      return context.control.head().irreversible_blocknum();
   }

}}} // ns eosio::chain::webassembly
