file(GLOB HEADERS "include/eosio/sign_transaction_plugin/*.hpp")
add_library( sign_transaction_plugin
             sign_transaction_plugin.cpp
             ${HEADERS} )

target_link_libraries( sign_transaction_plugin eosio_chain chain_plugin producer_plugin signature_provider_plugin custom_appbase appbase Boost::bimap )
target_include_directories( sign_transaction_plugin PUBLIC
                            "${CMAKE_CURRENT_SOURCE_DIR}/include"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../chain_interface/include"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../chain_plugin/include"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../producer_plugin/include"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../signature_provider_plugin/include"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/appbase/include" )

add_subdirectory( test )
