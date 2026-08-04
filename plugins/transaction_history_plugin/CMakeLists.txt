file(GLOB HEADERS "include/eosio/transaction_history_plugin/*.hpp")

add_library( transaction_history_plugin
             transaction_history_plugin.cpp
             rocksdb_manager.cpp
             async_worker.cpp
             rollback_manager.cpp
             ${HEADERS} )

# Prefer an upstream package configuration because it carries RocksDB's
# compression and threading dependencies. Fall back to the repository's find
# module for distributions which only install headers and a library.
find_package(RocksDB CONFIG QUIET)
if(NOT TARGET RocksDB::rocksdb)
   find_package(RocksDB MODULE REQUIRED)
endif()

target_link_libraries( transaction_history_plugin
                      chain_plugin
                      http_plugin
                      appbase
                      fc
                      RocksDB::rocksdb
                      ${CMAKE_DL_LIBS}
                      pthread )

target_include_directories( transaction_history_plugin
                           PUBLIC "include"
                           PRIVATE ${RocksDB_INCLUDE_DIRS} ${RocksDB_INCLUDE_DIR}
                                   ${ROCKSDB_INCLUDE_DIRS} ${ROCKSDB_INCLUDE_DIR} )

INSTALL( TARGETS
   transaction_history_plugin

   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)

INSTALL( FILES ${HEADERS} DESTINATION "include/eosio/transaction_history_plugin" )
