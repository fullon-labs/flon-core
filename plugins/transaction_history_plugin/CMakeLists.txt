file(GLOB HEADERS "include/eosio/transaction_history_plugin/*.hpp")

add_library( transaction_history_plugin
             transaction_history_plugin.cpp
             rocksdb_manager.cpp
             async_worker.cpp
             rollback_manager.cpp
             ${HEADERS} )

# Find RocksDB - Ubuntu/Debian specific handling
set(ROCKSDB_FOUND FALSE)

# Method 1: Try pkg-config (may not be available for all RocksDB installations)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
   pkg_check_modules(ROCKSDB rocksdb)
   if(ROCKSDB_FOUND)
      message(STATUS "Found RocksDB via pkg-config: ${ROCKSDB_VERSION}")
   endif()
endif()

# Method 2: Manual detection for Ubuntu/Debian librocksdb-dev package
if(NOT ROCKSDB_FOUND)
   # Look for headers
   find_path(ROCKSDB_INCLUDE_DIR
             NAMES rocksdb/db.h
             PATHS /usr/include
             NO_DEFAULT_PATH)

   # Look for library - Ubuntu typically names it librocksdb.so.X.X
   find_library(ROCKSDB_LIBRARY
                NAMES rocksdb
                PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu
                NO_DEFAULT_PATH)

   if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
      set(ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARY})
      set(ROCKSDB_INCLUDE_DIRS ${ROCKSDB_INCLUDE_DIR})
      set(ROCKSDB_FOUND TRUE)
      message(STATUS "Found RocksDB manually:")
      message(STATUS "  Headers: ${ROCKSDB_INCLUDE_DIR}")
      message(STATUS "  Library: ${ROCKSDB_LIBRARY}")
   endif()
endif()

# Method 3: Try our custom FindRocksDB.cmake
if(NOT ROCKSDB_FOUND)
   find_package(RocksDB QUIET)
   if(RocksDB_FOUND)
      set(ROCKSDB_LIBRARIES RocksDB::rocksdb)
      set(ROCKSDB_INCLUDE_DIRS ${RocksDB_INCLUDE_DIR})
      set(ROCKSDB_FOUND TRUE)
      message(STATUS "Found RocksDB via FindRocksDB.cmake")
   endif()
endif()

# Ensure we found RocksDB
if(NOT ROCKSDB_FOUND)
   message(FATAL_ERROR "RocksDB not found. Please install librocksdb-dev: sudo apt-get install librocksdb-dev")
endif()

message(STATUS "RocksDB include dirs: ${ROCKSDB_INCLUDE_DIRS}")
message(STATUS "RocksDB libraries: ${ROCKSDB_LIBRARIES}")

target_link_libraries( transaction_history_plugin
                      chain_plugin
                      http_plugin
                      appbase
                      fc
                      ${ROCKSDB_LIBRARIES}
                      ${CMAKE_DL_LIBS}
                      pthread )

target_include_directories( transaction_history_plugin
                           PUBLIC "include" ${ROCKSDB_INCLUDE_DIRS} )

if(ROCKSDB_CFLAGS_OTHER)
   target_compile_options( transaction_history_plugin PRIVATE ${ROCKSDB_CFLAGS_OTHER} )
endif()

INSTALL( TARGETS
   transaction_history_plugin

   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)

INSTALL( FILES ${HEADERS} DESTINATION "include/eosio/transaction_history_plugin" )
