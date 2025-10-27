file(GLOB HEADERS "include/eosio/transaction_history_api_plugin/*.hpp")

add_library( transaction_history_api_plugin
             transaction_history_api_plugin.cpp
             ${HEADERS} )

target_link_libraries( transaction_history_api_plugin
                      transaction_history_plugin
                      http_plugin
                      appbase
                      fc )

target_include_directories( transaction_history_api_plugin PUBLIC "include" )

INSTALL( TARGETS
   transaction_history_api_plugin

   RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
)

INSTALL( FILES ${HEADERS} DESTINATION "include/eosio/transaction_history_api_plugin" )
