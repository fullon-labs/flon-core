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

add_subdirectory(flon.bios)
add_subdirectory(flon.boot)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/old_versions/v1.6.0-rc3/eosio.bios/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/old_versions/v1.6.0-rc3/eosio.bios/)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/old_versions/v1.7.0-develop-preactivate_feature/eosio.bios/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/old_versions/v1.7.0-develop-preactivate_feature/eosio.bios/)
