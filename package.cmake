set(CPACK_GENERATOR "TZST")
find_program(DPKG_FOUND "dpkg")
find_program(RPMBUILD_FOUND "rpmbuild")
if(DPKG_FOUND)
   list(APPEND CPACK_GENERATOR "DEB")
endif()
if(RPMBUILD_FOUND)
   list(APPEND CPACK_GENERATOR "RPM")
endif()

set(CPACK_PACKAGE_VERSION "${VERSION_FULL}")
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${VERSION_FULL}")
if(EXISTS /etc/os-release)
   #if we're doing the build on Ubuntu or RHELish, add the platform version in to the package name
   file(READ /etc/os-release OS_RELEASE LIMIT 4096)
   if(OS_RELEASE MATCHES "\n?ID=\"?ubuntu" AND OS_RELEASE MATCHES "\n?VERSION_ID=\"?([0-9.]+)")
      string(APPEND CPACK_PACKAGE_FILE_NAME "-ubuntu${CMAKE_MATCH_1}")
   elseif(OS_RELEASE MATCHES "\n?ID=\"?rhel" AND OS_RELEASE MATCHES "\n?VERSION_ID=\"?([0-9]+)")
	   string(APPEND CPACK_PACKAGE_FILE_NAME "-el${CMAKE_MATCH_1}")
   elseif(OS_RELEASE MATCHES "\n?ID_LIKE=\"?([a-zA-Z0-9 ]*)" AND CMAKE_MATCH_1 MATCHES "rhel" AND OS_RELEASE MATCHES "\n?VERSION_ID=\"?([0-9]+)")
      string(APPEND CPACK_PACKAGE_FILE_NAME "-el${CMAKE_MATCH_1}")
   endif()
endif()

# Fix debian package filename as it should have the format:
# <PackageName>_<VersionNumber>_<DebianArchitecture>.deb

# Find architecture using dpkg
if (DPKG_FOUND)
    execute_process(COMMAND bash -c "${DPKG_FOUND} --print-architecture"
        OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(REGEX REPLACE "^${CMAKE_PROJECT_NAME}-(.*)$" "${CMAKE_PROJECT_NAME}_\\1_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}.deb" CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")

string(APPEND CPACK_PACKAGE_FILE_NAME "-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_PACKAGE_CONTACT "FullOn Network Foundation")
set(CPACK_PACKAGE_VENDOR "FullOn Network Foundation")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++ implementation of the FullOn protocol with Savanna consensus")
set(CPACK_COMPONENT_BASE_DESCRIPTION "shared runtime files for FLON packages")
set(CPACK_COMPONENT_NODE_DESCRIPTION "${NODE_EXECUTABLE_NAME} node daemon and BP, RPC, seed, and history configuration presets")
set(CPACK_COMPONENT_TOOLS_DESCRIPTION "CLI, wallet, maintenance, and state inspection tools")
set(CPACK_COMPONENT_DEV_DESCRIPTION "headers and libraries for native contract unit testing")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/fullon-labs/flon-core")

set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_BASE_PACKAGE_SECTION "utils")

set(CPACK_DEBIAN_PACKAGE_CONFLICTS "eosio, mandel, leap")
set(CPACK_RPM_PACKAGE_CONFLICTS "eosio, mandel, leap")

set(CPACK_COMPONENTS_ALL "base" "node")
if(FLON_BUILD_CLIENT_TOOLS)
   find_program(FLON_CHMOD_EXECUTABLE chmod REQUIRED)
   list(APPEND CPACK_COMPONENTS_ALL "tools")
endif()
if(ENABLE_PROJECT_DEV_DEB)
   list(APPEND CPACK_COMPONENTS_ALL "dev")
endif()

#enable per component packages for .deb; ensure main package is just "${CMAKE_PROJECT_NAME}", not "${CMAKE_PROJECT_NAME}-base", and make the dev package have "${CMAKE_PROJECT_NAME}-dev" at the front not the back
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_BASE_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_DEBIAN_BASE_FILE_NAME "${CPACK_DEBIAN_FILE_NAME}")
string(REGEX REPLACE "^(${CMAKE_PROJECT_NAME})" "\\1-node" CPACK_DEBIAN_NODE_FILE_NAME "${CPACK_DEBIAN_FILE_NAME}")
string(REGEX REPLACE "^(${CMAKE_PROJECT_NAME})" "\\1-tools" CPACK_DEBIAN_TOOLS_FILE_NAME "${CPACK_DEBIAN_FILE_NAME}")
string(REGEX REPLACE "^(${CMAKE_PROJECT_NAME})" "\\1-dev" CPACK_DEBIAN_DEV_FILE_NAME "${CPACK_DEBIAN_FILE_NAME}")

#deb package tooling will be unable to detect deps for the dev package. llvm is tricky since we don't know what package could have been used; try to figure it out
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "libgmp-dev, python3-distutils, python3-numpy, zlib1g-dev")
find_program(DPKG_QUERY "dpkg-query")
if(DPKG_QUERY AND OS_RELEASE MATCHES "\n?ID=\"?ubuntu" AND LLVM_CMAKE_DIR)
   execute_process(COMMAND "${DPKG_QUERY}" -S "${LLVM_CMAKE_DIR}" COMMAND cut -d: -f1 RESULT_VARIABLE LLVM_PKG_FIND_RESULT OUTPUT_VARIABLE LLVM_PKG_FIND_OUTPUT)
   if(LLVM_PKG_FIND_OUTPUT)
      string(STRIP "${LLVM_PKG_FIND_OUTPUT}" LLVM_PKG_FIND_OUTPUT)
      string(APPEND CPACK_DEBIAN_DEV_PACKAGE_DEPENDS ", ${LLVM_PKG_FIND_OUTPUT}")
   endif()
endif()
set(CPACK_DEBIAN_DEV_PACKAGE_CONTROL_EXTRA "${CMAKE_BINARY_DIR}/scripts/postinst;${CMAKE_BINARY_DIR}/scripts/prerm")

#since rpm packages aren't component based, make sure description picks up more detailed description instead of just summary
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_COMPONENT_BASE_DESCRIPTION}")

#turn some knobs to try and make package paths cooperate with GNUInstallDirs a little better
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE OFF)

# Portable role-oriented archives. These contain no configuration secrets and
# no wallet data. The normal distro packages remain available through CPack.
string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" FLON_ARCHIVE_PLATFORM)
set(FLON_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/packages")
file(MAKE_DIRECTORY "${FLON_ARCHIVE_DIR}")

if(FLON_NODE_VARIANT STREQUAL "history")
   set(FLON_NODE_ARCHIVE_NAME "flon-node-history-${VERSION_FULL}-${FLON_ARCHIVE_PLATFORM}")
else()
   set(FLON_NODE_ARCHIVE_NAME "flon-node-${VERSION_FULL}-${FLON_ARCHIVE_PLATFORM}")
endif()
set(FLON_NODE_ARCHIVE_STAGE "${FLON_ARCHIVE_DIR}/${FLON_NODE_ARCHIVE_NAME}")

add_custom_target(flon-node-package
   COMMAND ${CMAKE_COMMAND} -E rm -rf "${FLON_NODE_ARCHIVE_STAGE}"
   COMMAND ${CMAKE_COMMAND} -E make_directory "${FLON_NODE_ARCHIVE_STAGE}/bin"
   COMMAND ${CMAKE_COMMAND} -E make_directory "${FLON_NODE_ARCHIVE_STAGE}/config"
   COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${NODE_EXECUTABLE_NAME}>" "${FLON_NODE_ARCHIVE_STAGE}/bin/"
   COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/config/node" "${FLON_NODE_ARCHIVE_STAGE}/config"
   COMMAND ${CMAKE_COMMAND} -E tar "czf" "${FLON_ARCHIVE_DIR}/${FLON_NODE_ARCHIVE_NAME}.tar.gz" --format=gnutar "${FLON_NODE_ARCHIVE_NAME}"
   COMMAND ${CMAKE_COMMAND} -E tar "cf" "${FLON_ARCHIVE_DIR}/${FLON_NODE_ARCHIVE_NAME}.zip" --format=zip "${FLON_NODE_ARCHIVE_NAME}"
   WORKING_DIRECTORY "${FLON_ARCHIVE_DIR}"
   DEPENDS ${NODE_EXECUTABLE_NAME}
   USES_TERMINAL
)

if(FLON_BUILD_CLIENT_TOOLS)
   set(FLON_TOOLS_ARCHIVE_NAME "flon-tools-${VERSION_FULL}-${FLON_ARCHIVE_PLATFORM}")
   set(FLON_TOOLS_ARCHIVE_STAGE "${FLON_ARCHIVE_DIR}/${FLON_TOOLS_ARCHIVE_NAME}")
   add_custom_target(flon-tools-package
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${FLON_TOOLS_ARCHIVE_STAGE}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${FLON_TOOLS_ARCHIVE_STAGE}/bin"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${CLIENT_EXECUTABLE_NAME}>" "${FLON_TOOLS_ARCHIVE_STAGE}/bin/"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${KEY_STORE_EXECUTABLE_NAME}>" "${FLON_TOOLS_ARCHIVE_STAGE}/bin/"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${UTIL_EXECUTABLE_NAME}>" "${FLON_TOOLS_ARCHIVE_STAGE}/bin/"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:funod-state-inspect>" "${FLON_TOOLS_ARCHIVE_STAGE}/bin/"
      COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/scripts/state_checkpoint.py" "${FLON_TOOLS_ARCHIVE_STAGE}/bin/funod-state-checkpoint"
      COMMAND ${FLON_CHMOD_EXECUTABLE} +x "${FLON_TOOLS_ARCHIVE_STAGE}/bin/funod-state-checkpoint"
      COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/packaging/flon-tools/README.md" "${FLON_TOOLS_ARCHIVE_STAGE}/README.md"
      COMMAND ${CMAKE_COMMAND} -E tar "czf" "${FLON_ARCHIVE_DIR}/${FLON_TOOLS_ARCHIVE_NAME}.tar.gz" --format=gnutar "${FLON_TOOLS_ARCHIVE_NAME}"
      COMMAND ${CMAKE_COMMAND} -E tar "cf" "${FLON_ARCHIVE_DIR}/${FLON_TOOLS_ARCHIVE_NAME}.zip" --format=zip "${FLON_TOOLS_ARCHIVE_NAME}"
      WORKING_DIRECTORY "${FLON_ARCHIVE_DIR}"
      DEPENDS ${CLIENT_EXECUTABLE_NAME} ${KEY_STORE_EXECUTABLE_NAME} ${UTIL_EXECUTABLE_NAME} funod-state-inspect
      USES_TERMINAL
   )
endif()
