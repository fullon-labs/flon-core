#pragma once

#include <eosio/chain/block_timestamp.hpp>
#include <eosio/chain/types.hpp>

namespace eosio::chain {

using block_num_type  = uint32_t;
using block_time_type = chain::block_timestamp_type;

struct block_ref
{
   block_ref(const block_id_type& block_id, block_time_type timestamp, const digest_type& finality_digest,
             uint32_t active_policy_generation, uint32_t pending_policy_generation)
      : block_id(block_id)
      , timestamp(timestamp)
      , finality_digest(finality_digest)
      , active_policy_generation(active_policy_generation)
      , pending_policy_generation(pending_policy_generation) {}

   block_ref() = default; // creates `empty` representation where `block_id.empty() == true`

   block_id_type    block_id;
   block_time_type  timestamp;
   digest_type      finality_digest;  // finality digest associated with the block
   uint32_t         active_policy_generation{0};
   uint32_t         pending_policy_generation{0};

   bool           empty() const { return block_id.empty(); }
   block_num_type block_num() const; // Extract from block_id.

   auto operator<=>(const block_ref&) const = default;
   bool operator==(const block_ref& o) const = default;
};

struct block_ref_digest_data
{
   block_num_type   block_num{0};
   block_time_type  timestamp;
   digest_type      finality_digest;
   block_time_type  parent_timestamp;
};

struct qc_link
{
   block_num_type  source_block_num {0};
   block_num_type  target_block_num {0}; // Must be less than or equal to source_block_num (only equal for genesis block).
   bool            is_link_strong {false};
};

struct qc_claim_t
{
   block_num_type  block_num {0};
   bool            is_strong_qc {false};

   auto operator<=>(const qc_claim_t&) const = default;
};

struct core_metadata
{
   block_num_type  last_final_block_num {0};
   block_num_type  latest_qc_claim_block_num {0};
};

// ------------------------------------------------------------------------------------------------------------------
struct finality_core
{
   std::vector<qc_link>    links; // Captures all relevant links sorted in order of ascending source_block_num.
   std::vector<block_ref>  refs;  // Covers ancestor blocks with block numbers greater than or equal to last_final_block_num.
                                  // Sorted in order of ascending block_num.
   block_time_type         genesis_timestamp;  // set and used only for the genesis finality core.

   // Invariants:
   // 1. links.empty() == false
   // 2. last_final_block_num() <= links.front().source_block_num <= latest_qc_claim().block_num
   // 3. If refs.empty() == true, then (links.size() == 1) and 
   //                                  (links.back().target_block_num == links.back().source_block_num == last_final_block_num())
   // 4. If refs.empty() == false, then refs.front().block_num() == links.front().target_block_num == last_final_block_num()
   // 5. If refs.empty() == false, then refs.back().block_num() + 1 == links.back().source_block_num == current_block_num()
   // 6. If refs.size() > 1, then:
   //       For i = 0 to refs.size() - 2: 
   //          (refs[i].block_num() + 1 == refs[i+1].block_num()) and (refs[i].timestamp < refs[i+1].timestamp)
   // 7. If links.size() > 1, then:
   //       For i = 0 to links.size() - 2:
   //          (links[i].source_block_num + 1 == links[i+1].source_block_num) and (links[i].target_block_num <= links[i+1].target_block_num)
   // 8. current_block_num() - last_final_block_num() == refs.size() (always implied by invariants 3 to 6)
   // 9. current_block_num() - links.front().source_block_num == links.size() - 1 (always implied by invariants 1 and 7)

   /**
    *  @pre none
    *
    *  @post returned core has current_block_num() == block_num
    *  @post returned core has latest_qc_claim() == {.block_num=block_num, .is_strong_qc=false}
    *  @post returned core has last_final_block_num() == block_num
    */
   static finality_core create_core_for_genesis_block(const block_id_type& block_id, block_time_type timestamp);

   bool is_genesis_core() const { return refs.empty(); }

   /**
    *  @pre this->links.empty() == false
    *  @post none
    *  @returns block number of the core
    */
   block_num_type current_block_num() const;

   /**
    *  @pre this->links.empty() == false
    *  @post none
    *  @returns last final block_num in respect to the core
    */
   block_num_type last_final_block_num() const;

   /**
    *  @pre this->links.empty() == false
    *  @post none
    *  @returns last final block timestamp in respect to the core
    */
   block_time_type last_final_block_timestamp() const;

   /**
    *  @pre this->links.empty() == false
    *  @post none
    *  @returns latest qc_claim made by the core
    */
   qc_claim_t latest_qc_claim() const;

   /**
    *  @pre  all finality_core invariants
    *  @post same
    *  @returns timestamp of latest qc_claim made by the core
    */
   block_time_type latest_qc_block_timestamp() const;

   /**
    *  @pre  all finality_core invariants
    *  @post same
    *  @returns boolean indicating whether `id` is an ancestor of this block
    */
   bool extends(const block_id_type& id) const;

    /**
    *  @pre  last_final_block_num() <= candidate_block_num <= current_block_block_num()
    *  @post same
    *  @returns boolean indicating whether `candidate_block_num` is the genesis block number or not
    */
   bool is_genesis_block_num(block_num_type candidate_block_num) const;

   /**
    *  @pre last_final_block_num() <= block_num < current_block_num()
    *
    *  @post returned block_ref has block_num() == block_num
    */
   const block_ref& get_block_reference(block_num_type block_num) const;

   /**
    *  @pre  all finality_core invariants
    *  @post same
    *  @returns Merkle root digest of a sequence of block_refs
    */
   digest_type get_reversible_blocks_mroot() const;

   /**
    *  @pre links.front().source_block_num <= block_num <= current_block_num()
    *
    *  @post returned qc_link has source_block_num == block_num
    */
   const qc_link& get_qc_link_from(block_num_type block_num) const;

   /**
    *  @pre this->latest_qc_claim().block_num <= most_recent_ancestor_with_qc.block_num <= this->current_block_num()
    *  @pre this->latest_qc_claim() <= most_recent_ancestor_with_qc
    *
    *  @post returned core_metadata has last_final_block_num <= latest_qc_claim_block_num
    *  @post returned core_metadata has latest_qc_claim_block_num == most_recent_ancestor_with_qc.block_num
    *  @post returned core_metadata has last_final_block_num >= this->last_final_block_num()
    */
   core_metadata next_metadata(const qc_claim_t& most_recent_ancestor_with_qc) const;

   /**
    *  @pre current_block.block_num() == this->current_block_num()
    *  @pre If this->refs.empty() == false, then current_block is the block after the one referenced by this->refs.back()
    *  @pre this->latest_qc_claim().block_num <= most_recent_ancestor_with_qc.block_num <= this->current_block_num()
    *  @pre this->latest_qc_claim() <= most_recent_ancestor_with_qc (i.e.
    *        this->latest_qc_claim().block_num == most_recent_ancestor_with_qc.block_num &&
    *        most_recent_ancestor_with_qc.is_strong_qc ).
    *     When block_num is the same, most_recent_ancestor_with_qc must be stronger than latest_qc_claim()
    *
    *  @post returned core has current_block_num() == this->current_block_num() + 1
    *  @post returned core has latest_qc_claim() == most_recent_ancestor_with_qc
    *  @post returned core has last_final_block_num() >= this->last_final_block_num()
    */
   finality_core next(const block_ref& current_block, const qc_claim_t& most_recent_ancestor_with_qc) const;

   // should match the serialization provided by FC_REFLECT below, except that for compatibility with
   // fullon 1.0.0 consensus we do not pack the two new members of `block_ref` which were added in
   // fullon 1.0.1 (the finalizer policy generations)
   // ------------------------------------------------------------------------------------------------
   template<typename Stream>
   void pack_for_digest(Stream& s) const {
      fc::raw::pack(s, links);

      // manually pack the vector of refs since we don't want to pack the generation numbers
      fc::raw::pack(s, unsigned_int((uint32_t)refs.size()));
      for(const auto& ref : refs) {
         fc::raw::pack(s, ref.block_id);
         fc::raw::pack(s, ref.timestamp);
         fc::raw::pack(s, ref.finality_digest);
      }
      fc::raw::pack(s, genesis_timestamp);
   }
};

} /// eosio::chain

// -----------------------------------------------------------------------------
namespace std {
   // define std ostream output so we can use BOOST_CHECK_EQUAL in tests
   inline std::ostream& operator<<(std::ostream& os, const eosio::chain::block_ref& br) {
      os << "block_ref(" << br.block_id << ", " << br.timestamp << ", " << br.finality_digest << ", "
         << br.active_policy_generation << ", " << br.pending_policy_generation << ")";
      return os;
   }

   inline std::ostream& operator<<(std::ostream& os, const eosio::chain::qc_link& l) {
      os << "qc_link(" << l.source_block_num << ", " << l.target_block_num << ", " << l.is_link_strong << ")";
      return os;
   }

   inline std::ostream& operator<<(std::ostream& os, const eosio::chain::qc_claim_t& c) {
      os << "qc_claim_t(" << c.block_num << ", " << c.is_strong_qc << ")";
      return os;
   }

   inline std::ostream& operator<<(std::ostream& os, const eosio::chain::core_metadata& cm) {
      os << "core_metadata(" << cm.last_final_block_num << ", " <<
         ", " << cm.latest_qc_claim_block_num << ")";
      return os;
   }
}

FC_REFLECT( eosio::chain::block_ref, (block_id)(timestamp)(finality_digest)(active_policy_generation)(pending_policy_generation) )
FC_REFLECT( eosio::chain::block_ref_digest_data, (block_num)(timestamp)(finality_digest)(parent_timestamp) )
FC_REFLECT( eosio::chain::qc_link, (source_block_num)(target_block_num)(is_link_strong) )
FC_REFLECT( eosio::chain::qc_claim_t, (block_num)(is_strong_qc) )
FC_REFLECT( eosio::chain::core_metadata, (last_final_block_num)(latest_qc_claim_block_num))
FC_REFLECT( eosio::chain::finality_core, (links)(refs)(genesis_timestamp))
