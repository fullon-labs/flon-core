add_executable( test_sign_transaction_plugin
        plugin_config_test.cpp
        main.cpp
        )
target_link_libraries( test_sign_transaction_plugin sign_transaction_plugin eosio_testing eosio_chain_wrap )
add_test(NAME test_sign_transaction_plugin COMMAND plugins/sign_transaction_plugin/test/test_sign_transaction_plugin WORKING_DIRECTORY ${CMAKE_BINARY_DIR})