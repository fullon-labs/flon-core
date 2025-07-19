#pragma once

#include <eosio/eosio.hpp>
#include <eosio/crypto.hpp>
#include <eosio/symbol.hpp>
#include <eosio/asset.hpp>

using namespace eosio;
using bytes = std::vector<char>;
using namespace std;

class [[eosio::contract]] action_data_to_json_test : public eosio::contract {
public:
   using eosio::contract::contract;

   [[eosio::action]]
   void actionjson(const eosio::name& contract_name, const eosio::name& action_name, const std::vector<char> action_data,
                   const std::string expected_json);


   struct all_base_types {
      bool                    bool_value;
      int8_t                  int8_value;
      uint8_t                 uint8_value;
      int16_t                 int16_value;
      uint16_t                uint16_value;
      int32_t                 int32_value;
      uint32_t                uint32_value;
      int64_t                 int64_value;
      uint64_t                uint64_value;
      int128_t                int128_value;
      uint128_t               uint128_value;
      signed_int              varint32_value;
      unsigned_int            varuint32_value;
      float                   float32_value;
      double                  float64_value;
      long double             float128_value;
      time_point              time_point_value;
      time_point_sec          time_point_sec_value;
      block_timestamp_type    block_timestamp_type_value;
      name                    name_value;
      bytes                   bytes_value;
      string                  string_value;
      checksum160             checksum160_value;
      checksum256             checksum256_value;
      checksum512             checksum512_value;
      public_key              public_key_value;
      signature               signature_value;
      symbol                  symbol_value;
      symbol_code             symbol_code_value;
      asset                   asset_value;
      extended_asset          extended_asset_value;

      EOSLIB_SERIALIZE(all_base_types,
         (bool_value)
         (int8_value)
         (uint8_value)
         (int16_value)
         (uint16_value)
         (int32_value)
         (uint32_value)
         (int64_value)
         (uint64_value)
         (int128_value)
         (uint128_value)
         (varint32_value)
         (varuint32_value)
         (float32_value)
         (float64_value)
         (float128_value)
         (time_point_value)
         (time_point_sec_value)
         (block_timestamp_type_value)
         (name_value)
         (bytes_value)
         (string_value)
         (checksum160_value)
         (checksum256_value)
         (checksum512_value)
         (public_key_value)
         (signature_value)
         (symbol_value)
         (symbol_code_value)
         (asset_value)
         (extended_asset_value)
      )
   };

   [[eosio::action]]
   void testallbase(all_base_types all_base_types_value);
};
