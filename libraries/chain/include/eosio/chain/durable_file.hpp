#pragma once

#include <eosio/chain/snapshot_file.hpp>
#include <fstream>
#include <vector>

namespace eosio::chain::durable_file {

// The first safety record must not disappear because its newly-created parent
// directory was never committed to the parent's directory entries.
inline void create_directories(const std::filesystem::path& path) {
   std::vector<std::filesystem::path> missing;
   auto current = std::filesystem::absolute(path);
   while (!std::filesystem::exists(current)) {
      missing.push_back(current);
      current = current.parent_path();
   }
   std::filesystem::create_directories(path);
   for (const auto& directory : missing) {
      snapshot_file::sync_directory(directory);
      snapshot_file::sync_directory(directory.parent_path());
   }
}

struct sync_operations {
   static void sync(const std::filesystem::path& path) { snapshot_file::sync(path); }
   static void promote(const std::filesystem::path& from, const std::filesystem::path& to) {
      snapshot_file::rename(from, to);
   }
};

// Stage on the destination filesystem in a private directory. Never truncate
// the last published generation. Success includes checked close, file sync,
// atomic rename and directory sync. A failure after rename is still a failure;
// callers must not authorize clean state or a vote on an uncertain result.
template<class Operations = sync_operations, class Writer>
void replace(const std::filesystem::path& destination, Writer&& writer) {
   const auto target = std::filesystem::absolute(destination);
   fc::temp_directory staging(target.parent_path());
   const auto temporary = staging.path() / target.filename();
   std::ofstream out;
   out.exceptions(std::ios::badbit | std::ios::failbit);
   out.open(temporary, std::ios::binary | std::ios::trunc);
   std::filesystem::permissions(temporary, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write);
   std::forward<Writer>(writer)(out);
   out.flush();
   out.close();
   Operations::sync(temporary);
   Operations::promote(temporary, target);
}

// fc::datastream_crc expects boolean write/put results, unlike std::ostream.
struct output_adapter {
   std::ofstream& stream;
   bool write(const char* data, size_t size) { stream.write(data, size); return true; }
   bool put(char value) { stream.put(value); return true; }
};

} // namespace eosio::chain::durable_file
