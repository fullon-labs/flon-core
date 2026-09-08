// Internal read-only companion for scripts/state_checkpoint.py. The caller must
// hold the node operation locks; this program never clears a dirty marker.
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/database_header_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/block_handle.hpp>
#include <eosio/chain/block_log.hpp>
#include <eosio/chain/fork_database.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/json.hpp>
#include <iostream>

namespace {
// Deliberately accept only the current v3 fork-file format. Use the native
// per-branch decoder (which does not consume/persist files) and fail closed
// on future formats instead of teaching a Python tool private C++ layouts.
void check_fork_file(const std::filesystem::path& path, const eosio::chain::block_id_type& head) {
   using namespace eosio::chain;
   fc::cfile file;
   file.set_file_path(path);
   file.open("rb");
   fc::cfile_datastream stream(file);
   uint32_t magic, version, active;
   fc::raw::unpack(stream, magic);
   fc::raw::unpack(stream, version);
   EOS_ASSERT(magic == 0x30510FDB && version == 3, fork_database_exception, "Unsupported checkpoint fork file");
   fc::raw::unpack(stream, active);
   EOS_ASSERT(active <= 2, fork_database_exception, "Invalid checkpoint fork mode");
   fork_database_legacy_t legacy;
   fork_database_if_t savanna;
   // Structural/link validation here; configured protocol-feature validation
   // is still performed by the controller when starting the restored node.
   validator_t validator = [](auto, const auto&, const auto&) {};
   bool have_legacy, have_savanna;
   fc::raw::unpack(stream, have_legacy);
   if (have_legacy) legacy.open("checkpoint legacy", path, stream, validator);
   fc::raw::unpack(stream, have_savanna);
   if (have_savanna) savanna.open("checkpoint savanna", path, stream, validator);
   EOS_ASSERT(file.tellp() == std::filesystem::file_size(path), fork_database_exception, "Trailing checkpoint fork data");
   EOS_ASSERT((active != 0 || have_legacy) && (active != 1 || have_savanna) &&
              (active != 2 || (have_legacy && have_savanna)), fork_database_exception, "Missing active checkpoint fork database");
   const bool found_legacy = have_legacy && legacy.is_valid() && legacy.get_block(head, include_root_t::yes);
   const bool found_savanna = have_savanna && savanna.is_valid() && savanna.get_block(head, include_root_t::yes);
   EOS_ASSERT((active == 0 && found_legacy) || (active == 1 && found_savanna) ||
              (active == 2 && (found_legacy || found_savanna)), fork_database_exception, "Checkpoint head missing from fork database");
}
}

int main(int argc, char** argv) {
   using namespace eosio::chain;
   try {
      if (argc == 5 && std::string(argv[1]) == "anchor") {
         auto head = block_log::check_recovery_anchor(argv[2], chain_id_type(argv[3]), block_id_type(argv[4]));
         std::cout << fc::json::to_string(fc::mutable_variant_object()("log_head", head), fc::time_point::maximum()) << '\n';
         return 0;
      }
      if (argc != 3 || std::string(argv[1]) != "state") {
         std::cerr << "Usage: funod-state-inspect state STATE_DIR | anchor BLOCKS_DIR CHAIN_ID BLOCK_ID\n";
         return 1;
      }
      chainbase::database db(argv[2], chainbase::database::read_only);
      db.add_index<database_header_multi_index>();
      db.add_index<global_property_multi_index>();
      const auto& headers = db.get_index<database_header_multi_index>().indices().get<by_id>();
      EOS_ASSERT(!headers.empty() && db.revision() >= 1, database_exception, "Missing initialized state database");
      headers.begin()->validate();
      const auto* global = db.find<global_property_object>();
      EOS_ASSERT(global, database_exception, "Missing state chain identity");
      block_handle head;
      EOS_ASSERT(head.read(std::filesystem::path(argv[2]) / "chain_head.dat", false) && head.is_valid(),
                 database_exception, "Missing clean chain head");
      EOS_ASSERT(db.revision() == head.block_num(), database_exception, "Checkpoint state/head revision mismatch");
      check_fork_file(std::filesystem::path(argv[2]).parent_path() / "blocks/reversible/fork_db.dat", head.id());
      const auto undo = db.get_index<database_header_multi_index>().undo_stack_revision_range();
      std::cout << fc::json::to_string(fc::mutable_variant_object()
         ("chain_id", global->chain_id)("head_block_id", head.id())("head_block_num", head.block_num())
         ("undo_first_revision", undo.first)("undo_last_revision", undo.second), fc::time_point::maximum()) << '\n';
      return 0;
   } catch (const std::system_error& e) {
      std::cerr << e.what() << '\n';
      return e.code() == chainbase::make_error_code(chainbase::db_error_code::dirty) ? 2 : 1;
   } catch (const fc::exception& e) {
      std::cerr << e.to_detail_string() << '\n';
      return 1;
   } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
   }
}
