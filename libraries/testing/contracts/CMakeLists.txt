cmake_minimum_required( VERSION 3.8 )
project(testing_contracts)

if( EOSIO_COMPILE_TEST_CONTRACTS )
    set(EOSIO_WASM_OLD_BEHAVIOR "Off")
    find_package( flon.cdt REQUIRED )
    add_compile_options(-no-missing-ricardian-clause)
endif()

if ("${CMAKE_GENERATOR}" STREQUAL "Ninja")
    add_compile_options(-fcolor-diagnostics)
endif()

add_subdirectory(sys.bios)
add_subdirectory(flon.boot)
