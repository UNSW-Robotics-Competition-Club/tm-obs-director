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

# mbedTLS supplies HMAC-SHA256 for TM API request signing. It arrives with the
# prebuilt obs-deps bundle on every platform and exports its own CMake config
# package, so this adds no new dependency for anyone building the plugin.
# Only mbedcrypto is needed — mbedtls_md_hmac lives there, not in libmbedtls.
find_package(MbedTLS REQUIRED)

# The upstream config package already brings in ws2_32 and bcrypt, but the
# prebuilt mbedcrypto.lib still references the legacy __imp_fopen / __imp_fread /
# __imp_ferror ... exports that the modern UCRT no longer surfaces. Without this
# the link dies with ~10 LNK2019s that look like an mbedTLS problem and are not.
set(TMD_CRYPTO_LIBS MbedTLS::mbedcrypto)
if(MSVC)
  list(APPEND TMD_CRYPTO_LIBS legacy_stdio_definitions)
endif()

# libcurl for HTTP, nlohmann/json for parsing. Both ship in the obs-deps bundle,
# so neither adds a dependency for anyone building the plugin. nlohmann is
# header-only and has no config package in the bundle, hence find_path.
find_package(CURL REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

# The TM client is deliberately free of libobs and Qt so it can be tested on its
# own. Keep it that way: anything needing OBS belongs in src/ui or src/engine.
set(TMD_TM_SOURCES
    src/tm/tm-signer.cpp
    src/tm/tm-types.cpp
    src/tm/http-client.cpp
    src/tm/tm-api-client.cpp
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp ${TMD_TM_SOURCES})

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${TMD_CRYPTO_LIBS} CURL::libcurl)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# The signer test links only against tm-signer.cpp and mbedTLS: no OBS, no Qt.
# That keeps the highest-risk code verifiable without a full plugin build.
if(ENABLE_TESTS)
  enable_testing()

  add_executable(test-signer test/test-signer.cpp src/tm/tm-signer.cpp)
  target_link_libraries(test-signer PRIVATE ${TMD_CRYPTO_LIBS})
  target_include_directories(test-signer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(test-signer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  add_test(NAME signer COMMAND test-signer)

  # Exercises the client against test/mock-tm-server.mjs. The Node runner starts
  # the mock on an ephemeral port, runs this binary against it, and shuts it
  # down, so the test is hermetic and needs no TM, no LAN, and no credentials.
  add_executable(test-tm-client test/test-tm-client.cpp ${TMD_TM_SOURCES})
  target_link_libraries(test-tm-client PRIVATE ${TMD_CRYPTO_LIBS} CURL::libcurl)
  target_include_directories(test-tm-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
  set_target_properties(test-tm-client PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  find_program(NODE_EXECUTABLE NAMES node)
  if(NODE_EXECUTABLE)
    add_test(
      NAME tm-client
      COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test/run-client-test.mjs"
              "$<TARGET_FILE:test-tm-client>"
    )

    # obs-deps ships libcurl as a DLL. Without its directory on PATH the test
    # binary fails to start at all, which shows up as a silent non-zero exit with
    # no output rather than anything resembling a helpful error.
    get_filename_component(TMD_DEPS_PREFIX "${CURL_DIR}/../../.." ABSOLUTE)
    set_tests_properties(
      tm-client
      PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${TMD_DEPS_PREFIX}/bin"
    )
  else()
    message(WARNING "node not found: skipping the tm-client integration test")
  endif()
endif()
