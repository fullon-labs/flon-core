# FindRocksDB.cmake
# Finds the RocksDB library
#
# This will define the following variables:
#   RocksDB_FOUND       - True if the system has the RocksDB library
#   RocksDB_INCLUDE_DIR - The include directory for RocksDB
#   RocksDB_LIBRARIES   - Libraries to link against
#
# and the following imported targets:
#   RocksDB::rocksdb

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_ROCKSDB QUIET IMPORTED_TARGET rocksdb)
endif()

if(TARGET PkgConfig::PC_ROCKSDB)
    add_library(RocksDB::rocksdb ALIAS PkgConfig::PC_ROCKSDB)
    set(RocksDB_FOUND TRUE)
    set(RocksDB_VERSION ${PC_ROCKSDB_VERSION})
    set(RocksDB_INCLUDE_DIRS ${PC_ROCKSDB_INCLUDE_DIRS})
    set(RocksDB_LIBRARIES RocksDB::rocksdb)
    return()
endif()

find_path(RocksDB_INCLUDE_DIR
    NAMES rocksdb/db.h
    PATHS ${PC_ROCKSDB_INCLUDE_DIRS}
    PATH_SUFFIXES rocksdb
)

find_library(RocksDB_LIBRARY
    NAMES rocksdb
    PATHS ${PC_ROCKSDB_LIBRARY_DIRS}
)

set(RocksDB_VERSION ${PC_ROCKSDB_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RocksDB
    FOUND_VAR RocksDB_FOUND
    REQUIRED_VARS
        RocksDB_LIBRARY
        RocksDB_INCLUDE_DIR
    VERSION_VAR RocksDB_VERSION
)

if(RocksDB_FOUND)
    set(RocksDB_LIBRARIES ${RocksDB_LIBRARY})
    set(RocksDB_INCLUDE_DIRS ${RocksDB_INCLUDE_DIR})
    set(RocksDB_DEFINITIONS ${PC_ROCKSDB_CFLAGS_OTHER})
endif()

if(RocksDB_FOUND AND NOT TARGET RocksDB::rocksdb)
    add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
    set_target_properties(RocksDB::rocksdb PROPERTIES
        IMPORTED_LOCATION "${RocksDB_LIBRARY}"
        INTERFACE_COMPILE_OPTIONS "${PC_ROCKSDB_CFLAGS_OTHER}"
        INTERFACE_INCLUDE_DIRECTORIES "${RocksDB_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(
    RocksDB_INCLUDE_DIR
    RocksDB_LIBRARY
)
