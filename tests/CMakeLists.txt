# ohmg unit tests (opt-in; see the root CMakeLists' OPENHES_BUILD_TESTS).

# Sources needed by every test that puts a message on the bus. hes_bus.c now
# stamps each outgoing message with hes_clock_now_ns() (18012-3 11.2.3.1
# requires a time stamp on every HES-CLME message), so the clock comes with it.
set(TEST_BUS_SRCS
  ../src/common/hes_bus.c
  ../src/common/hes_clock.c
)

# The XML layer (src/common/hes_query.c) is the single place in the tree that
# touches libxml2, so it is tested directly, with no bus and no stub: path
# translation, element handles, collections, tree navigation, and the
# "no silent truncation" regressions. Fixture: hes_query_sample.xml, in this
# directory (the test runs here, see WORKING_DIRECTORY below).
add_executable(test_hes_query
  test_hes_query.c
  ../src/common/hes_query.c
)

target_include_directories(test_hes_query PRIVATE
  ../src
  ../deps/libxml2/include
)

target_link_libraries(test_hes_query PRIVATE LibXml2 m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_hes_query PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_hes_query COMMAND test_hes_query)
set_tests_properties(test_hes_query PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# test_bm exercises the binding-map engine (src/bm/bm.c) offline against the
# REAL vendored nng + libxml2 (no stub): it hands the map a pub socket listening
# on inproc:// and captures its sends with a subscribed sub socket.
# Fixtures are the scenario XMLs in this directory (tests/*.xml).

add_executable(test_bm
  test_bm.c
  ../src/bm/bm.c
  ../src/common/hes_query.c
  ${TEST_BUS_SRCS}
  ../deps/log/log.c
)

target_include_directories(test_bm PRIVATE
  ../src
  ../deps/nng/include
  ../deps/libxml2/include
  ../deps/log
)

target_link_libraries(test_bm PRIVATE nng LibXml2 m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_bm PRIVATE -Wall -Wextra)
endif()

# Run from the source dir so the fixture path ("bm_button_controlled_light.xml")
# resolves without needing argv.
add_test(NAME test_bm_button COMMAND test_bm)
set_tests_properties(test_bm_button PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# App-service routing: a row with op='ap' (appService) has its operation
# performed by the protected app instead of by the built-in op set. Uses the
# same real nng + libxml2, and installs a C stub for the binding map's app
# hook instead of a Lua state.
add_executable(test_bm_appservice
  test_bm_appservice.c
  ../src/bm/bm.c
  ../src/common/hes_query.c
  ${TEST_BUS_SRCS}
  ../deps/log/log.c
)

target_include_directories(test_bm_appservice PRIVATE
  ../src
  ../deps/nng/include
  ../deps/libxml2/include
  ../deps/log
)

target_link_libraries(test_bm_appservice PRIVATE nng LibXml2 m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_bm_appservice PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_bm_appservice COMMAND test_bm_appservice)
set_tests_properties(test_bm_appservice PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Authorization propagation: the A&A service writes each applicable permission
# into a binding map's authorType ('at') field, and the binding map enforces it
# (deny by default, 'bk' suppresses the PUT). Uses the real nng + libxml2 +
# jansson; no Lua and no bus round-trip.
add_executable(test_auth_propagate
  test_auth_propagate.c
  ../src/bm/bm.c
  ../src/common/hes_query.c
  ../src/services/auth/auth.c
  ${TEST_BUS_SRCS}
  ../deps/log/log.c
)

target_include_directories(test_auth_propagate PRIVATE
  ../src
  ../deps/nng/include
  ../deps/libxml2/include
  ../deps/log
)

target_link_libraries(test_auth_propagate PRIVATE nng LibXml2 jansson::jansson m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_auth_propagate PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_auth_propagate COMMAND test_auth_propagate)
set_tests_properties(test_auth_propagate PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Per-message (Mode B) authorization: two buttons drive the same light; the
# credential travelling with each message decides which one may output. Phase 2
# adds the proof gate (auth_policy_proof.json), so the crypto and identification
# services come along: verifying a peer's proof needs no key of the gateway's
# own, only the public half the proof carries.
add_executable(test_auth_authorize
  test_auth_authorize.c
  ../src/bm/bm.c
  ../src/common/hes_query.c
  ../src/services/auth/auth.c
  ../src/services/crypto/crypto.c
  ../src/services/id/id.c
  ${TEST_BUS_SRCS}
  ../deps/log/log.c
)

target_include_directories(test_auth_authorize PRIVATE
  ../src
  ../deps/nng/include
  ../deps/libxml2/include
  ../deps/jansson/src
  ../deps/log
  ${CMAKE_CURRENT_BINARY_DIR}/../deps/jansson/include
)

target_link_libraries(test_auth_authorize PRIVATE nng LibXml2 jansson::jansson
  TF-PSA-Crypto::tfpsacrypto m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_auth_authorize PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_auth_authorize COMMAND test_auth_authorize)
set_tests_properties(test_auth_authorize PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Identification service: the identity document is validated strictly at
# startup and a malformed or incomplete one is refused rather than defaulted.
# The secret fingerprint (memoryType 'sp', obscured) must never appear in a GET
# response, and 'pi' is now the public key DERIVED from it, so this needs PSA.
add_executable(test_id_service
  test_id_service.c
  ../src/services/id/id.c
  ../deps/log/log.c
)

target_include_directories(test_id_service PRIVATE
  ../src
  ../deps/nng/include
  ../deps/log
  ../deps/jansson/src
  ${CMAKE_CURRENT_BINARY_DIR}/../deps/jansson/include
)

target_link_libraries(test_id_service PRIVATE nng jansson::jansson
  TF-PSA-Crypto::tfpsacrypto m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_id_service PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_id_service COMMAND test_id_service)
set_tests_properties(test_id_service PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Local routing: an object's own address returns all of its data, a trailing
# transCode selects one datum, and '?da=a,b' selects several. Includes the
# longest-prefix rule for one object nested inside another's address.
add_executable(test_dispatch
  test_dispatch.c
  ../src/common/hes_dispatch.c
  ${TEST_BUS_SRCS}
  ../deps/log/log.c
)

target_include_directories(test_dispatch PRIVATE
  ../src
  ../deps/nng/include
  ../deps/log
)

target_link_libraries(test_dispatch PRIVATE nng m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_dispatch PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_dispatch COMMAND test_dispatch)
set_tests_properties(test_dispatch PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Time service (ISO/IEC 18012-3 11.2.3): the three functional objects of Tables
# 45-56, the datum addressing of their answers through the real router, and the
# binding map's ability to read the 'va' datum out of such an answer -- which is
# what a time-based rule depends on. The sync state comes from a stub, so no
# network and no socket to a server is involved.
add_executable(test_time_service
  test_time_service.c
  ../src/services/time/time.c
  ../src/common/hes_dispatch.c
  ../src/bm/bm.c
  ../src/common/hes_query.c
  ${TEST_BUS_SRCS}
  ../deps/log/log.c
)

target_include_directories(test_time_service PRIVATE
  ../src
  ../deps/nng/include
  ../deps/libxml2/include
  ../deps/log
)

target_link_libraries(test_time_service PRIVATE nng LibXml2 m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_time_service PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_time_service COMMAND test_time_service)
set_tests_properties(test_time_service PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# The time service's network-facing helpers, tested without opening a socket:
# the SNTP offset maths against hand-built reply packets, and the time zone
# name reader plus the system tz database lookups. hes_https.c is compiled in
# because geo_tz_detect() calls it, but no test reaches it.
add_executable(test_time_net
  test_time_net.c
  ../src/services/time/sntp.c
  ../src/services/time/geo_tz.c
  ../src/common/hes_https.c
  ../src/common/hes_clock.c
  ../deps/log/log.c
)

target_include_directories(test_time_net PRIVATE
  ../src
  ../deps/log
  ../deps/jansson/src
  ${CMAKE_CURRENT_BINARY_DIR}/../deps/jansson/include
)

target_link_libraries(test_time_net PRIVATE jansson::jansson
  CURL::libcurl
  Threads::Threads m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_time_net PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_time_net COMMAND test_time_net)
set_tests_properties(test_time_net PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# The identity key pair (18012-3 11.2.2.1): 'fp' derives a P-256 signing key and
# 'pi' becomes its verifiable public half, so the public ID stops being a bare
# name and starts being a claim that can be proved. Offline and deterministic by
# construction -- both the derivation and the signatures are reproducible, so
# nothing here needs randomness, a network, or hardware.
add_executable(test_crypto_identity
  test_crypto_identity.c
  ../src/services/id/id.c
  ../src/services/crypto/crypto.c
  ../deps/log/log.c
)

target_include_directories(test_crypto_identity PRIVATE
  ../src
  ../deps/nng/include
  ../deps/log
  ../deps/jansson/src
  ${CMAKE_CURRENT_BINARY_DIR}/../deps/jansson/include
)

target_link_libraries(test_crypto_identity PRIVATE nng jansson::jansson
  TF-PSA-Crypto::tfpsacrypto m)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test_crypto_identity PRIVATE -Wall -Wextra)
endif()

add_test(NAME test_crypto_identity COMMAND test_crypto_identity)
set_tests_properties(test_crypto_identity PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
