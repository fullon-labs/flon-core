cmake_minimum_required( VERSION 3.16 )

include( chain_config.cmake )
project( ${PROJECT_NAME} )
include(CTest) # suppresses DartConfiguration.tcl error
enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

include( GNUInstallDirs )
include( MASSigning )

# Add RocksDB module path for transaction_history_plugin
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_EXTENSIONS ON )
set( CXX_STANDARD_REQUIRED ON)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 6)
set(VERSION_PATCH 5)
# set(VERSION_SUFFIX alpha)

if(VERSION_SUFFIX)
    set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-${VERSION_SUFFIX}")
else()
    set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
endif()


# Options
option(ENABLE_WERROR "Enable `-Werror` compilation flag." Off)
option(ENABLE_WEXTRA "Enable `-Wextra` compilation flag." Off)

# http://stackoverflow.com/a/18369825
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10.2)
        message(FATAL_ERROR "GCC version must be at least 10.2.0!")
    endif()
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
   add_compile_options(-fdiagnostics-color=always)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")
set(BUILD_DOXYGEN FALSE CACHE BOOL "Build doxygen documentation on every make")
set(ENABLE_MULTIVERSION_PROTOCOL_TEST FALSE CACHE BOOL "Enable node multiversion protocol test")

option(ENABLE_OC "Enable eosvm-oc on supported platforms" ON)

# WASM runtimes to enable. Each runtime in this list will have:
#  * definition PROJECT_<RUNTIME>_RUNTIME_ENABLED defined in public libchain interface
#  * ctest entries with --runtime
if(ENABLE_OC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
   if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux" AND "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
      list(APPEND CHAIN_WASM_RUNTIMES eos-vm-oc)
      # EOS VM OC requires LLVM, but move the check up here to a central location so that the ProjectTester.cmakes
      # can be created with the exact version found
      find_package(LLVM REQUIRED CONFIG)
      if(LLVM_VERSION_MAJOR VERSION_LESS 7 OR LLVM_VERSION_MAJOR VERSION_GREATER_EQUAL 16)
	      message(FATAL_ERROR "${PROJECT_NAME} requires an LLVM version 7 through 15")
      endif()
   endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
   if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
      list(APPEND CHAIN_WASM_RUNTIMES eos-vm eos-vm-jit)
   else()
      list(APPEND CHAIN_WASM_RUNTIMES eos-vm)
   endif()
endif()

if(UNIX)
  if(APPLE)
    set(whole_archive_flag "-force_load")
    set(no_whole_archive_flag "")
  else()
    set(whole_archive_flag "--whole-archive")
    set(no_whole_archive_flag "--no-whole-archive")
  endif()
else()
  set(whole_archive_flag "--whole-archive")
  set(no_whole_archive_flag "--no-whole-archive")
endif()

if( APPLE AND UNIX )
# Apple Specific Options Here
    message( STATUS "Configuring ${PROJECT_NAME} on macOS" )
    set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wall -Wno-deprecated-declarations" )
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-deprecated-declarations" )
else()
    # Linux Specific Options Here
    message( STATUS "Configuring ${PROJECT_NAME} on Linux" )
    set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wall" )
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall" )
    if ( FULL_STATIC_BUILD )
      set( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
    endif ( FULL_STATIC_BUILD )

    if( "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" )
        if( CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL 4.0.0 OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 4.0.0 )
            set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-invalid-partial-specialization" )
        endif()
    endif()
endif()

if(ENABLE_WERROR)
  set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Werror" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror" )
endif()
if(ENABLE_WEXTRA)
  set( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wextra" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wextra" )
endif()


option(PROJECT_ENABLE_DEVELOPER_OPTIONS "enable developer options for ${PROJECT_NAME}" OFF)

# based on http://www.delorie.com/gnu/docs/gdb/gdb_70.html
# uncomment this line to tell GDB about macros (slows compile times)
# set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -gdwarf-2 -g3" )

set(ENABLE_COVERAGE_TESTING FALSE CACHE BOOL "Build ${PROJECT_NAME} for code coverage analysis")

if(ENABLE_COVERAGE_TESTING)
  SET(CMAKE_C_FLAGS   "--coverage ${CMAKE_C_FLAGS}")
  SET(CMAKE_CXX_FLAGS "--coverage ${CMAKE_CXX_FLAGS}")
  find_program( LCOV_PATH lcov )
  find_program( LLVMCOV_PATH  llvm-cov )
  find_program( GENHTML_PATH NAMES genhtml)
endif()

include(utils)

string(TOUPPER ${CORE_SYMBOL_NAME} CORE_SYMBOL_NAME_UPPER)
if (NOT CORE_SYMBOL_NAME STREQUAL CORE_SYMBOL_NAME_UPPER )
  message(FATAL_ERROR "CORE_SYMBOL_NAME ${CORE_SYMBOL_NAME} must be upper case")
endif()

string(LENGTH ${CORE_SYMBOL_NAME} CORE_SYMBOL_NAME_LENGTH)
if (CORE_SYMBOL_NAME_LENGTH GREATER 7)
  message(FATAL_ERROR "CORE_SYMBOL_NAME length must be between 1 and 7 characters")
endif()

if (CORE_SYMBOL_PRECISION GREATER 18)
  message(FATAL_ERROR "CORE_SYMBOL_NAME length must less or equal 18")
endif()

message( STATUS "Using '${CORE_SYMBOL_NAME}' as CORE symbol name" )
message( STATUS "Using '${CORE_SYMBOL_PRECISION}' as CORE symbol precision" )

if ("${SYSTEM_ROOT_KEY}" STREQUAL "")
   set(SYSTEM_ROOT_KEY "${PUBLIC_KEY_LEGACY_PREFIX}6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV")
endif()

message( STATUS "Using '${SYSTEM_ROOT_KEY}' as public key for '${SYSTEM_ACCOUNT_NAME}' account" )

option(ENABLE_TCMALLOC "use tcmalloc (requires gperftools)" OFF)
if( ENABLE_TCMALLOC )
    find_package( Gperftools REQUIRED )
    message( STATUS "Compiling ${PROJECT_NAME} with TCMalloc")
    #if doing this by the book, simply link_libraries( ${GPERFTOOLS_TCMALLOC} ) here. That will
    #give the performance benefits of tcmalloc but since it won't be linked last
    #the heap profiler & checker may not be accurate. This here is rather undocumented behavior
    #to stuff a library toward the end of the link list
    set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} ${GPERFTOOLS_TCMALLOC}")
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${GPERFTOOLS_TCMALLOC}")
endif()

# project includes a bundled BoringSSL which conflicts with OpenSSL. Make sure any other bundled libraries (such as boost)
# do not attempt to use an external OpenSSL in any manner
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL On)

# many tests require handling of signals themselves and even when they don't we'd like for them to generate a core dump, this
# is effectively --catch_system_errors=no broadly across all tests
add_compile_definitions(BOOST_TEST_DEFAULTS_TO_CORE_DUMP)

# Unless this is set, some boost unordered headers include `arm_neon.h` when building on ARM processors,
# which includes a definition for `float16_t` conflicting with the one in `libraries/softfloat`.
add_compile_definitions(BOOST_UNORDERED_DISABLE_NEON)

add_subdirectory( libraries )
add_subdirectory( plugins )
add_subdirectory( programs )
add_subdirectory( scripts )
add_subdirectory( tools )

option(BUILD_TESTS "Build unit tests" OFF)
option(DISABLE_WASM_SPEC_TESTS "disable building of wasm spec unit tests" OFF)

if(BUILD_TESTS)
  message(STATUS "Building tests.")
  add_subdirectory( tests )
  message(STATUS "Building unittests.")
  add_subdirectory( unittests )
  message(STATUS "Building benchmark.")
  add_subdirectory( benchmark )
else()
  message(STATUS "Tests will not be built. To build tests, set BUILD_TESTS to ON.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/project.version.in ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.version.hpp)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.version.hpp DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL)

set(PROJECT_LIB_ROOT_DIR "${CMAKE_BINARY_DIR}/lib")
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/ChainCore-config.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/${PROJECT_NAME}/${PROJECT_NAME}-config.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/ChainCoreTesterBuild.cmake.in ${CMAKE_BINARY_DIR}/lib/cmake/${PROJECT_NAME}/ChainCoreTester.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/ChainCoreCheckVersion.cmake ${CMAKE_BINARY_DIR}/lib/cmake/${PROJECT_NAME}/ChainCoreCheckVersion.cmake COPYONLY)

set(PROJECT_LIB_ROOT_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/ChainCore-config.cmake.in ${CMAKE_BINARY_DIR}/modules/ChainCore-config.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/ChainCoreTesterBuild.cmake.in ${CMAKE_BINARY_DIR}/modules/ChainCoreTester.cmake @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/CMakeModules/ChainCoreCheckVersion.cmake ${CMAKE_BINARY_DIR}/modules/ChainCoreCheckVersion.cmake COPYONLY)

# Install the project config files
install(FILES ${CMAKE_BINARY_DIR}/modules/ChainCore-config.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/${PROJECT_NAME} COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_BINARY_DIR}/modules/ChainCoreTester.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/${PROJECT_NAME} COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES ${CMAKE_SOURCE_DIR}/modules/ChainCoreCheckVersion.cmake DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/cmake/${PROJECT_NAME} COMPONENT dev EXCLUDE_FROM_ALL)

configure_file(LICENSE                                                     licenses/${PROJECT_NAME}/LICENSE           COPYONLY)
configure_file(libraries/softfloat/COPYING.txt                             licenses/${PROJECT_NAME}/LICENSE.softfloat COPYONLY)
configure_file(libraries/wasm-jit/LICENSE                                  licenses/${PROJECT_NAME}/LICENSE.wavm      COPYONLY)
configure_file(libraries/libfc/secp256k1/secp256k1/COPYING                 licenses/${PROJECT_NAME}/LICENSE.secp256k1 COPYONLY)
configure_file(libraries/libfc/include/fc/crypto/webauthn_json/license.txt licenses/${PROJECT_NAME}/LICENSE.rapidjson COPYONLY)
configure_file(libraries/eos-vm/LICENSE                                    licenses/${PROJECT_NAME}/LICENSE.eos-vm    COPYONLY)
configure_file(libraries/prometheus/prometheus-cpp/LICENSE                 licenses/${PROJECT_NAME}/LICENSE.prom      COPYONLY)
configure_file(programs/client/LICENSE.CLI11                                licenses/${PROJECT_NAME}/LICENSE.CLI11     COPYONLY)
configure_file(libraries/libfc/libraries/bls12-381/LICENSE                 licenses/${PROJECT_NAME}/LICENSE.bls12-381 COPYONLY)
configure_file(libraries/libfc/libraries/boringssl/boringssl/src/LICENSE   licenses/${PROJECT_NAME}/LICENSE.boringssl COPYONLY)

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/licenses/${PROJECT_NAME}" DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/licenses/" COMPONENT base)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/libraries/testing/contracts DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/${PROJECT_NAME}_testing/libraries/testing COMPONENT dev EXCLUDE_FROM_ALL
        PATTERN "CMakeFiles" EXCLUDE
        PATTERN "*.cmake" EXCLUDE
        PATTERN "Makefile" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/unittests/contracts DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/${PROJECT_NAME}_testing/unittests COMPONENT dev EXCLUDE_FROM_ALL
        PATTERN "CMakeFiles" EXCLUDE
        PATTERN "*.cmake" EXCLUDE
        PATTERN "Makefile" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests/TestHarness DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/${PROJECT_NAME}_testing/tests COMPONENT dev EXCLUDE_FROM_ALL
        FILES_MATCHING
        PATTERN "*.py"
        PATTERN "*.json"
        PATTERN "__pycache__" EXCLUDE
        PATTERN "CMakeFiles" EXCLUDE)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
   # Cmake versions < 3.21 did not support installing symbolic links to a directory via install(FILES ...)
   # Cmake 3.21 fixed this (https://gitlab.kitware.com/cmake/cmake/-/commit/d71a7cc19d6e03f89581efdaa8d63db216184d40)
   # If/when the lowest cmake version supported becomes >= 3.21, the else block as well as the postinit and prerm scripts
   # would be a good option to remove in favor of using the facilities in cmake / cpack directly.

   add_custom_target(link_target ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory lib/python3/dist-packages
      COMMAND ${CMAKE_COMMAND} -E create_symlink ../../../share/${PROJECT_NAME}_testing/tests/TestHarness lib/python3/dist-packages/TestHarness
      COMMAND ${CMAKE_COMMAND} -E make_directory share/${PROJECT_NAME}_testing
      COMMAND ${CMAKE_COMMAND} -E create_symlink ../../bin share/${PROJECT_NAME}_testing/bin)

   install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lib/python3/dist-packages/TestHarness DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}/python3/dist-packages COMPONENT dev EXCLUDE_FROM_ALL)
   install(FILES ${CMAKE_CURRENT_BINARY_DIR}/share/${PROJECT_NAME}_testing/bin DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/${PROJECT_NAME}_testing COMPONENT dev EXCLUDE_FROM_ALL)
else()
   # The following install(SCRIPT ...) steps are necessary for `make dev-install` to work on cmake versions < 3.21.
   install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/scripts/install_testharness_symlinks.cmake COMPONENT dev EXCLUDE_FROM_ALL)

   # The `make package` installation of symlinks happens via the `postinst` script installed in cmake.package via the line below
endif()

configure_file(${CMAKE_SOURCE_DIR}/libraries/cli11/bash-completion/completions/util.in
               ${CMAKE_BINARY_DIR}/programs/util/bash-completion/completions/${UTIL_EXECUTABLE_NAME} @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/libraries/cli11/bash-completion/completions/client.in
               ${CMAKE_BINARY_DIR}/programs/client/bash-completion/completions/${CLIENT_EXECUTABLE_NAME} @ONLY)

install(FILES ${CMAKE_BINARY_DIR}/programs/util/bash-completion/completions/${UTIL_EXECUTABLE_NAME} DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/bash-completion/completions COMPONENT base)
install(FILES ${CMAKE_BINARY_DIR}/programs/client/bash-completion/completions/${CLIENT_EXECUTABLE_NAME} DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/bash-completion/completions COMPONENT base)

# Add the boost submodule we used to build to our install package, so headers can be found for libtester
install(DIRECTORY "${CMAKE_SOURCE_DIR}/libraries/boost/"
        DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/${PROJECT_NAME}_boost
        COMPONENT dev EXCLUDE_FROM_ALL
        PATTERN ".git/*" EXCLUDE
        PATTERN "example/*" EXCLUDE
        PATTERN "bench/*" EXCLUDE
        PATTERN "doc/*" EXCLUDE
        PATTERN "libs/*/test" EXCLUDE
        PATTERN "tools/*/test" EXCLUDE
        )

add_custom_target(dev-install
  COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component dev
  USES_TERMINAL
)

include(doxygen)

option(ENABLE_PROJECT_DEV_DEB "Enable building the ${PROJECT_NAME}-dev .deb package" OFF)

include(package.cmake)
include(CPack)
