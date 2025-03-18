if( EOSIO_COMPILE_TEST_CONTRACTS )
message("111111111111111 CMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}")
message("111111111111111 EXTRA_DEFINITIONS=${EXTRA_DEFINITIONS}")


   # add_compile_definitions( SYSTEM_ACCOUNT_NAME="name" )
   add_contract( reject_all reject_all reject_all.cpp )
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/reject_all.wasm ${CMAKE_CURRENT_BINARY_DIR}/reject_all.wasm COPYONLY )
endif()
