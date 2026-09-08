#pragma once

#include <fc/io/cfile.hpp>
#include <fc/scoped_exit.hpp>
#include <filesystem>
#include <system_error>

namespace eosio::chain::snapshot_file {

inline void sync(const std::filesystem::path& path) {
   fc::cfile file;
   file.set_file_path(path);
   file.open("rb+");
   file.sync(); // fsync, plus F_FULLFSYNC on macOS
}

inline void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
   const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
   if (descriptor < 0)
      throw std::system_error(errno, std::generic_category(), "open snapshot directory " + path.string());
   const auto close_descriptor = fc::make_scoped_exit([&] { ::close(descriptor); });
   if (::fsync(descriptor) != 0)
      throw std::system_error(errno, std::generic_category(), "sync snapshot directory " + path.string());
#else
   // Directory fsync is a POSIX durability guarantee. File sync is still
   // required above; Windows directory persistence needs separate validation.
   (void)path;
#endif
}

inline void rename(const std::filesystem::path& source, const std::filesystem::path& destination) {
   std::filesystem::rename(source, destination);
   sync_directory(destination.parent_path());
   if (source.parent_path() != destination.parent_path()) sync_directory(source.parent_path());
}

} // namespace eosio::chain::snapshot_file
