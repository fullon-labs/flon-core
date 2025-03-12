
configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)
add_executable( ${UTIL_EXECUTABLE_NAME} main.cpp actions/subcommand.cpp actions/generic.cpp actions/blocklog.cpp actions/bls.cpp actions/snapshot.cpp actions/chain.cpp "${CMAKE_CURRENT_BINARY_DIR}/config.hpp")

if( UNIX AND NOT APPLE )
  set(rt_library rt )
endif()

target_include_directories(${UTIL_EXECUTABLE_NAME} PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries( ${UTIL_EXECUTABLE_NAME}
        PRIVATE appbase version
        PRIVATE eosio_chain chain_plugin fc fullon-cli11 producer_plugin ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS} )

copy_bin( ${UTIL_EXECUTABLE_NAME} )
install( TARGETS
  ${UTIL_EXECUTABLE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT base
)
