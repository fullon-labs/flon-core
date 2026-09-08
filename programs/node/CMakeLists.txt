add_executable( ${NODE_EXECUTABLE_NAME} main.cpp )

add_executable(funod-state-inspect state_inspect.cpp)
target_link_libraries(funod-state-inspect PRIVATE eosio_chain fc)
copy_bin(funod-state-inspect)
install(TARGETS funod-state-inspect RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT base)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/state_checkpoint.py
        DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} RENAME funod-state-checkpoint COMPONENT base)
if(UNIX)
  find_program(FLON_CHECKPOINT_PYTHON NAMES python3)
  if(FLON_CHECKPOINT_PYTHON)
    add_test(NAME state_checkpoint_test COMMAND ${FLON_CHECKPOINT_PYTHON}
             ${CMAKE_SOURCE_DIR}/tests/state_checkpoint_tests.py)
  endif()
endif()

if( UNIX AND NOT APPLE )
  set(rt_library rt )
endif()

configure_file(config.hpp.in config.hpp ESCAPE_QUOTES)

target_include_directories(${NODE_EXECUTABLE_NAME} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

if(UNIX)
  if(APPLE)
    set(whole_archive_flag "-force_load")
    set(no_whole_archive_flag "")
    set(build_id_flag "")
  else()
    set(whole_archive_flag "--whole-archive")
    set(no_whole_archive_flag "--no-whole-archive")
    set(build_id_flag "--build-id")
  endif()
else()
  set(whole_archive_flag "--whole-archive")
  set(no_whole_archive_flag "--no-whole-archive")
  set(build_id_flag "")
endif()

target_link_libraries( ${NODE_EXECUTABLE_NAME}
        PRIVATE appbase version
        PRIVATE -Wl,${whole_archive_flag} chain_api_plugin                -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} net_plugin                      -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} net_api_plugin                  -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} db_size_api_plugin              -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} producer_api_plugin             -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} resource_monitor_plugin         -Wl,${no_whole_archive_flag}
        PRIVATE -Wl,${whole_archive_flag} prometheus_plugin               -Wl,${no_whole_archive_flag}

        PRIVATE chain_plugin http_plugin producer_plugin http_client_plugin
        PRIVATE eosio_chain_wrap fc ${CMAKE_DL_LIBS} ${PLATFORM_SPECIFIC_LIBS}
        Boost::dll )

if(build_id_flag)
  target_link_options(${NODE_EXECUTABLE_NAME} PRIVATE -Wl,${build_id_flag})
endif()
foreach(optional_plugin IN ITEMS state_history_plugin trace_api_plugin
        test_control_plugin test_control_api_plugin sign_transaction_plugin
        sign_transaction_api_plugin transaction_history_plugin transaction_history_api_plugin)
  if(TARGET ${optional_plugin})
    target_link_libraries(${NODE_EXECUTABLE_NAME} PRIVATE
        -Wl,${whole_archive_flag} ${optional_plugin} -Wl,${no_whole_archive_flag})
  endif()
endforeach()

include(additionalPlugins)

copy_bin( ${NODE_EXECUTABLE_NAME} )
install( TARGETS
   ${NODE_EXECUTABLE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR} COMPONENT base
)
