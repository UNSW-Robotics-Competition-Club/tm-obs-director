cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# TM Director needs both: the frontend API to switch scenes and add a dock,
# and Qt for the dock UI itself.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_TESTS "Build the standalone unit tests" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# libcurl for HTTP, nlohmann/json for parsing. Both ship in the obs-deps bundle,
# so neither adds a dependency for anyone building the plugin. nlohmann is
# header-only and has no config package in the bundle, hence find_path.
find_package(CURL REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

# The TM client is deliberately free of libobs and Qt so it can be tested on its
# own. Keep it that way: anything needing OBS belongs in src/ui or src/engine.
set(
  TMD_TM_SOURCES
  src/tm/sha256.cpp
  src/tm/tm-signer.cpp
  src/tm/tm-types.cpp
  src/tm/tm-events.cpp
  src/tm/http-client.cpp
  src/tm/cerberus-auth.cpp
  src/tm/tm-api-client.cpp
  src/tm/field-set-socket.cpp
)

# The Cerberus client key is issued per build, never per laptop and never
# committed. It is resolved from the first of these that is set:
#
#   1. -DTMD_CERBERUS_KEY=ctm_...   for CI, where the secret is masked
#   2. the TMD_CERBERUS_KEY env var for a developer shell
#   3. cerberus-key.txt at the repo root
#
# The file is there because the other two put the key on a command line or in an
# environment, both of which get echoed into terminal scrollback, shell history
# and any transcript of the session. A file written once in an editor does not.
# .gitignore already excludes everything not explicitly whitelisted, so the file
# cannot be committed by accident.
#
# The key is never message()d back out, only where it came from, so configuring
# in a shared or recorded terminal does not leak it either.
#
# A build without a key still compiles and runs - it just cannot mint a bearer,
# and says so in the dock rather than failing as an indistinguishable 401. That
# matters for anyone building the plugin from source without SRA's key.
set(TMD_CERBERUS_KEY "" CACHE STRING "Cerberus client key (ctm_...) to bake into this build")

set(TMD_CERBERUS_KEY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cerberus-key.txt")

set(_tmd_cerberus_key "${TMD_CERBERUS_KEY}")
set(_tmd_cerberus_key_source "-DTMD_CERBERUS_KEY")

if(_tmd_cerberus_key STREQUAL "" AND NOT "$ENV{TMD_CERBERUS_KEY}" STREQUAL "")
  set(_tmd_cerberus_key "$ENV{TMD_CERBERUS_KEY}")
  set(_tmd_cerberus_key_source "the TMD_CERBERUS_KEY environment variable")
endif()

if(_tmd_cerberus_key STREQUAL "" AND EXISTS "${TMD_CERBERUS_KEY_FILE}")
  file(READ "${TMD_CERBERUS_KEY_FILE}" _tmd_cerberus_key)
  # A file written in an editor ends with a newline, and the key is compared
  # byte for byte at the other end.
  string(STRIP "${_tmd_cerberus_key}" _tmd_cerberus_key)
  set(_tmd_cerberus_key_source "cerberus-key.txt")
endif()

if(_tmd_cerberus_key STREQUAL "")
  message(
    WARNING
    "No Cerberus key. This build cannot sign in to Tournament Manager. "
    "Write the key to cerberus-key.txt in the project root, or configure with "
    "-DTMD_CERBERUS_KEY=ctm_... to produce a usable build."
  )
else()
  # Shape only. Enough to catch a truncated paste or a stray quote at configure
  # time rather than as a 401 at a venue, and it reveals nothing.
  #
  # Length and pattern are checked separately because CMake's regex engine has no
  # bounded repetition: {16} is matched literally, so a correct key fails a regex
  # that looks right. The exact check lives in isValidCerberusKey either way.
  string(LENGTH "${_tmd_cerberus_key}" _tmd_cerberus_key_length)
  if(NOT _tmd_cerberus_key MATCHES "^ctm_[0-9a-f]+_[0-9a-f]+$" OR NOT _tmd_cerberus_key_length EQUAL 69)
    message(
      WARNING
      "The Cerberus key from ${_tmd_cerberus_key_source} is not shaped like "
      "ctm_<16 hex>_<48 hex> (length ${_tmd_cerberus_key_length}). Sign-in will "
      "be refused locally before it is ever sent."
    )
  else()
    message(STATUS "Cerberus key: ${_tmd_cerberus_key_length} characters from ${_tmd_cerberus_key_source}")
  endif()
endif()

set_property(
  SOURCE src/tm/cerberus-auth.cpp
  APPEND
  PROPERTY COMPILE_DEFINITIONS "TMD_CERBERUS_KEY=\"${_tmd_cerberus_key}\"" "TMD_BUILD_VERSION=\"${PROJECT_VERSION}\""
)

# The engine holds the decisions: what state the field set is in, which scene an
# event calls for, and when the switch actually happens. Like the TM client it
# stays free of libobs and Qt, so all of that is testable against a fake clock
# rather than against a stopwatch and a running OBS.
set(
  TMD_ENGINE_SOURCES
  src/engine/fieldset-state.cpp
  src/engine/rules.cpp
  src/engine/scheduler.cpp
  src/engine/match-log.cpp
)

# FieldSetSocket runs its own thread. Explicit on Linux, a no-op on Windows.
find_package(Threads REQUIRED)

# The only sources that touch libobs or Qt. Everything they call is testable
# without either.
set(TMD_UI_SOURCES src/ui/settings.cpp src/ui/director.cpp src/ui/dock.cpp)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp ${TMD_TM_SOURCES} ${TMD_ENGINE_SOURCES} ${TMD_UI_SOURCES}
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl Threads::Threads)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src")

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Windows installer. The template only ever produced a zip, which leaves the
# operator to put the files in the one folder OBS reads and no other - the
# mistake this plugin's own history is a record of.
#
# Inno Setup is not a build dependency: without it the target simply is not
# offered, and CI builds the installer on a runner that has it.
if(WIN32)
  find_program(
    TMD_ISCC
    NAMES ISCC
    PATHS "C:/Program Files (x86)/Inno Setup 6" "C:/Program Files/Inno Setup 6"
    DOC "Inno Setup command-line compiler, for the installer target"
  )

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _tmd_buildspec)
  string(JSON TMD_INSTALLER_DISPLAY_NAME GET "${_tmd_buildspec}" displayName)
  set(TMD_INSTALLER_VERSION "${_version}")
  set(TMD_INSTALLER_PUBLISHER "${_author}")
  set(TMD_INSTALLER_URL "${_website}")
  set(TMD_INSTALLER_MODULE "${_name}")
  # The staged layout produced by the build: the same bin/64bit and data the
  # installer has to reproduce under ProgramData.
  set(TMD_INSTALLER_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/installer-stage/${_name}")
  set(TMD_INSTALLER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")
  # Absolute: anything relative in the .iss resolves against the staging
  # directory, which is inside the build tree and has no LICENSE in it.
  set(TMD_INSTALLER_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/build-aux/installer-Windows.iss.in"
    "${CMAKE_CURRENT_BINARY_DIR}/installer-Windows.iss"
    @ONLY
  )

  if(TMD_ISCC)
    add_custom_target(
      installer
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${TMD_INSTALLER_SOURCE_DIR}"
      # Staged from the source tree, not from the build's rundir: that layout
      # differs between a local build and CI, and the installed data is just
      # data/ either way.
      COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "${TMD_INSTALLER_SOURCE_DIR}/data"
      COMMAND
        "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
        "${TMD_INSTALLER_SOURCE_DIR}/bin/64bit/${_name}.dll"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${TMD_INSTALLER_OUTPUT_DIR}"
      COMMAND "${TMD_ISCC}" "${CMAKE_CURRENT_BINARY_DIR}/installer-Windows.iss"
      DEPENDS ${CMAKE_PROJECT_NAME}
      COMMENT "Building the Windows installer"
      VERBATIM
    )
  else()
    message(STATUS "Inno Setup not found: the 'installer' target is unavailable (the plugin still builds)")
  endif()
endif()

# The signer test links only against the signer and the hash it uses: no OBS, no
# Qt, no third-party crypto. That keeps the highest-risk code verifiable without
# a full plugin build.
if(ENABLE_TESTS)
  enable_testing()

  add_executable(test-signer test/test-signer.cpp src/tm/tm-signer.cpp src/tm/sha256.cpp)
  target_include_directories(test-signer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src")
  set_target_properties(test-signer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  add_test(NAME signer COMMAND test-signer)

  # The engine needs nothing but a compiler: no mock server, no network, no
  # clock. It is the cheapest test in the suite and the one that covers the
  # inference TM's wire protocol forces on us.
  add_executable(test-engine test/test-engine.cpp ${TMD_ENGINE_SOURCES} src/tm/tm-types.cpp)
  target_include_directories(
    test-engine
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src" ${NLOHMANN_JSON_INCLUDE_DIR}
  )
  set_target_properties(test-engine PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  add_test(NAME engine COMMAND test-engine)

  # Exercises the client against test/mock-tm-server.mjs. The Node runner starts
  # the mock on an ephemeral port, runs this binary against it, and shuts it
  # down, so the test is hermetic and needs no TM, no LAN, and no credentials.
  add_executable(test-tm-client test/test-tm-client.cpp ${TMD_TM_SOURCES})
  target_link_libraries(test-tm-client PRIVATE CURL::libcurl Threads::Threads)
  target_include_directories(
    test-tm-client
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src" ${NLOHMANN_JSON_INCLUDE_DIR}
  )
  set_target_properties(test-tm-client PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  # Same idea one layer up: the mock speaks RFC 6455 by hand, so this exercises
  # the real handshake, the real framing, and a real yanked connection.
  add_executable(test-field-set-socket test/test-field-set-socket.cpp ${TMD_TM_SOURCES})
  target_link_libraries(test-field-set-socket PRIVATE CURL::libcurl Threads::Threads)
  target_include_directories(
    test-field-set-socket
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/src" ${NLOHMANN_JSON_INCLUDE_DIR}
  )
  set_target_properties(test-field-set-socket PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  find_program(NODE_EXECUTABLE NAMES node)
  if(NODE_EXECUTABLE)
    add_test(
      NAME tm-client
      COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test/run-client-test.mjs" "$<TARGET_FILE:test-tm-client>"
    )

    # The mock is the oracle every integration test is judged against, so it is
    # tested first and on its own. Pure Node: no curl DLL, no PATH fixup.
    add_test(NAME mock-server COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test/test-mock-server.mjs")

    add_test(
      NAME field-set-socket
      COMMAND
        ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test/run-socket-test.mjs" "$<TARGET_FILE:test-field-set-socket>"
    )

    # obs-deps ships libcurl as a DLL. Without its directory on PATH the test
    # binary fails to start at all, which shows up as a silent non-zero exit with
    # no output rather than anything resembling a helpful error.
    get_filename_component(TMD_DEPS_PREFIX "${CURL_DIR}/../../.." ABSOLUTE)
    set_tests_properties(
      tm-client
      field-set-socket
      PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${TMD_DEPS_PREFIX}/bin"
    )
  else()
    message(WARNING "node not found: skipping the tm-client and field-set-socket integration tests")
  endif()
endif()
