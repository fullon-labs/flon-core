file(GLOB HEADERS "include/eosio/sign_transaction_api_plugin/*.hpp")
add_library( sign_transaction_api_plugin
             sign_transaction_api_plugin.cpp
             ${HEADERS} )

target_link_libraries( sign_transaction_api_plugin sign_transaction_plugin http_plugin appbase )
target_include_directories( sign_transaction_api_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )
