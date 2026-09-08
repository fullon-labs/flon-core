#include <eosio/chain/contract_types.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/transaction_history_plugin/rocksdb_manager.hpp>
#include <fc/io/json.hpp>
#include <iostream>

// Test-only executable, never installed. The key is the public development
// genesis key; callers must use an isolated, P2P-disabled test chain.
int main(int argc, char** argv) {
   try {
      if ((argc == 4 || argc == 5) && std::string(argv[1]) == "transaction") {
         using namespace eosio::chain;
         private_key_type key("5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3");
         signed_transaction trx;
         trx.expiration = fc::time_point_sec(fc::time_point::now() + fc::seconds(60));
         trx.set_reference_block(fc::variant(argv[3]).as<block_id_type>());
         trx.actions.emplace_back(std::vector<permission_level>{{"flon"_n, "active"_n}},
            newaccount{"flon"_n, argc == 5 ? name(argv[4]) : "smokeacct"_n,
                       authority(key.get_public_key()), authority(key.get_public_key())});
         trx.sign(key, fc::variant(argv[2]).as<chain_id_type>());
         const packed_transaction packed(trx, packed_transaction::compression_type::none);
         std::cout << fc::json::to_string(fc::mutable_variant_object("id", trx.id())("packed", packed),
                                        fc::time_point::maximum());
         return 0;
      }
      if (argc == 3) {
         eosio::rocksdb_manager db(8 * 1024 * 1024);
         if (!db.open(argv[2])) return 2;
         const std::string account_index = "acc:flon:00000000000000000000";
         if (std::string(argv[1]) == "corrupt-sequence") {
            std::string original;
            if (!db.get(account_index, original)) return 7;
            return db.batch_write({{"_internal_account_sequence:flon", "broken"},
                                   {"_internal_test_original_index", original}}, {}, true) ? 0 : 8;
         }
         if (std::string(argv[1]) == "check-sequence") {
            std::string original, current, sequence, gap;
            return db.get("_internal_test_original_index", original) &&
                   db.get(account_index, current) && original == current &&
                   db.get("_internal_account_sequence:flon", sequence) && sequence == "broken" &&
                   db.get("_internal_history_gap_block", gap) ? 0 : 9;
         }
         if (std::string(argv[1]) == "seed-gap") {
            return db.batch_write({{"_internal_history_gap_block", "2"},
               {"trx:gap-test-sentinel", R"({"block_num":1})"},
               {"_internal_last_accepted_block_id", "unverified-test-branch"}}, {}, true) ? 0 : 3;
         }
         if (std::string(argv[1]) == "check-gap") {
            std::string gap, sentinel;
            return db.get("_internal_history_gap_block", gap) && gap == "2" &&
                   db.get("trx:gap-test-sentinel", sentinel) ? 0 : 4;
         }
      }
      return 1;
   } catch (const fc::exception& e) {
      std::cerr << e.to_detail_string();
      return 5;
   } catch (const std::exception& e) {
      std::cerr << e.what();
      return 6;
   }
}
