#pragma once

#include <eosio/chain/exceptions.hpp>

namespace eosio::chain {

struct chain_snapshot_header {
   /**
    * Version history
    *   8: initial version
    */

   static constexpr uint32_t minimum_compatible_version = 2;
   static constexpr uint32_t current_version = 8;

   // TODO: should remove unsupported old versions and the related implements?
   static constexpr uint32_t first_version_with_split_table_sections = 7;

   uint32_t version = current_version;

   void validate() const {
      auto min = minimum_compatible_version;
      auto max = current_version;
      EOS_ASSERT(version >= min && version <= max,
              snapshot_validation_exception,
              "Unsupported version of chain snapshot: ${version}. Supported version must be between ${min} and ${max} inclusive.",
              ("version",version)("min",min)("max",max));
   }
};

}

FC_REFLECT(eosio::chain::chain_snapshot_header,(version))
