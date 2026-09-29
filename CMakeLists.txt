cmake_minimum_required(VERSION 3.28)
project(ohmg)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")

find_package(PkgConfig QUIET)
find_package(Threads REQUIRED)
if(PkgConfig_FOUND)
  pkg_check_modules(HIREDIS QUIET hiredis)
  pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
endif()

# BLE HAN module backend: only ONE hes_ble_*.c implementation may be
# compiled, since both define hes_ble_mgr_* (mirrors the poc2 Makefile's
# BLE_BACKEND=sim|bluez switch):
#   bluez (default) - real BlueZ D-Bus via GIO (needs libglib2.0-dev)
#   sim             - hardware-free simulator stand-in
set(BLE_BACKEND "bluez" CACHE STRING "BLE connection-manager backend (sim|bluez)")
if(BLE_BACKEND STREQUAL "sim")
  set(HES_BLE_BACKEND_SRC src/han/ble/hes_ble.c)
elseif(BLE_BACKEND STREQUAL "bluez")
  set(HES_BLE_BACKEND_SRC src/han/ble/hes_ble_bluez.c)
else()
  message(FATAL_ERROR "BLE_BACKEND must be 'sim' or 'bluez' (got '${BLE_BACKEND}')")
endif()

if(NOT HIREDIS_FOUND)
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
  find_library(HIREDIS_LIBRARY hiredis)
  if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    set(HIREDIS_FOUND TRUE)
    set(HIREDIS_INCLUDE_DIRS ${HIREDIS_INCLUDE_DIR})
    set(HIREDIS_LIBRARIES ${HIREDIS_LIBRARY})
  endif()
endif()

# Disable vendored test targets in subprojects.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
set(NNG_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/nng)

# Keep third-party code quiet without changing vendored sources.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  foreach(_third_party_target nng nng_testing)
    if(TARGET ${_third_party_target})
      target_compile_options(${_third_party_target} PRIVATE -w)
    endif()
  endforeach()
endif()

set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)  # Disable building xmllint, xmlcatalog, etc.
set(LIBXML2_WITH_TESTS OFF CACHE BOOL "" FORCE)     # Disable tests
add_subdirectory(deps/libxml2)

set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(deps/jansson)

# Suppress warnings from the vendored jansson. At -O3 its lexer trips
# -Wstringop-overflow on a false positive: stream_get() (src/load.c) bounds its
# write into stream_t::buffer[5] with utf8_check_first(), which is defined in
# utf.c but only prototyped in utf.h, so GCC sees a size_t of unknown range and
# cannot prove the loop stays inside the buffer. jansson's implementation only
# ever returns 0..4, so buffer[4] is the last byte written.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND TARGET jansson)
  target_compile_options(jansson PRIVATE -w)
endif()

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/mbedtls)

# libcurl: the HTTPS GET behind common/hes_https.c (the time service's time zone
# lookup). Built static and as small as the client needs: HTTP(S) only, no
# executable/examples/docs, and the vendored mbed TLS as the TLS backend (so no
# OpenSSL dependency and no second TLS stack in the binary).
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)

# The trust store libcurl verifies against. Left to itself, curl probes a fixed
# list of paths at configure time (deps/curl/CMakeLists.txt) and bakes in the one
# it finds -- or none, if it finds none, which is what happened to the arm64
# cross build's tree: with no CA bundle compiled in, libcurl loads no CA store and
# every HTTPS GET is rejected as "not correctly signed by the trusted CA". Pinning
# the path makes both trees behave the same. Keep in sync with
# HES_HTTPS_CA_BUNDLE in src/app_config.h, which the HTTPS client passes to curl
# as CURLOPT_CAINFO.
set(CURL_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt" CACHE STRING
    "Absolute path to the CA bundle.")

# curl's FindMbedTLS only knows how to look for an *installed* mbedTLS
# (pkg-config, a CMake package, find_library); it cannot see the build-tree
# targets that add_subdirectory(deps/mbedtls) just created, and its mbedTLS 4
# branch would ask for MbedTLS::tfpsacrypto, a name mbedTLS only exports as
# TF-PSA-Crypto::tfpsacrypto. Filling in the find module's results skips that
# search: the "libraries" are the imported targets to link, and the version is
# read from the real build_info.h under the include directory.
set(MBEDTLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls/include" CACHE FILEPATH "" FORCE)
set(MBEDTLS_LIBRARY "MbedTLS::mbedtls" CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY "MbedTLS::mbedx509" CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY "TF-PSA-Crypto::tfpsacrypto" CACHE STRING "" FORCE)
add_subdirectory(deps/curl)

# Suppress warnings from libcurl. Its sources compile straight into the static
# library target; 'libcurl' and 'CURL::libcurl' are ALIAS targets, which
# target_compile_options rejects. The TARGET guard keeps this a no-op (just
# noisier output) if a future curl renames them.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  foreach(_curl_target libcurl_static libcurl_object)
    if(TARGET ${_curl_target})
      target_compile_options(${_curl_target} PRIVATE -w)
    endif()
  endforeach()
endif()

# Suppress warnings from libxml2.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  if(TARGET LibXml2)
    target_compile_options(LibXml2 PRIVATE -w)
  endif()
endif()

add_subdirectory(deps/libuv)

# Set the version info embedded in Win32 binaries.
set(APP_NAME "ohmg")
set(COMPANY_NAME "Tom G. Huang")
set(FILE_DESC "ohmg - OpenHES Modular Gateway")
set(INTERNAL_NAME "${PROJECT_NAME}")
set(LEGAL_COPYRIGHT "Copyright 2026 ${COMPANY_NAME} All rights reserved.")
set(ORIGINAL_FILE_NAME "${TARGET_BASE_NAME}")
set(PRODUCT_NAME "OpenHES Modular Gateway")
include(config_version_info)

set(LUA_SRCS
  deps/lua/src/lapi.c
  deps/lua/src/lauxlib.c
  deps/lua/src/lbaselib.c
  deps/lua/src/lcode.c
  deps/lua/src/lcorolib.c
  deps/lua/src/lctype.c
  deps/lua/src/ldblib.c
  deps/lua/src/ldebug.c
  deps/lua/src/ldo.c
  deps/lua/src/ldump.c
  deps/lua/src/lfunc.c
  deps/lua/src/lgc.c
  deps/lua/src/linit.c
  deps/lua/src/liolib.c
  deps/lua/src/llex.c
  deps/lua/src/lmathlib.c
  deps/lua/src/lmem.c
  deps/lua/src/loadlib.c
  deps/lua/src/lobject.c
  deps/lua/src/lopcodes.c
  deps/lua/src/loslib.c
  deps/lua/src/lparser.c
  deps/lua/src/lstate.c
  deps/lua/src/lstring.c
  deps/lua/src/lstrlib.c
  deps/lua/src/ltable.c
  deps/lua/src/ltablib.c
  deps/lua/src/ltm.c
  deps/lua/src/lundump.c
  deps/lua/src/lutf8lib.c
  deps/lua/src/lvm.c
  deps/lua/src/lzio.c
)

# Lua normally enables LUA_USE_POSIX via -DLUA_USE_LINUX in its own build
# (see luaconf.h). Without it, os.tmpname() falls back to the ISO-C
# tmpnam(), which glibc flags at link time ("use of 'tmpnam' is
# dangerous"). Scope LUA_USE_POSIX to just the Lua sources so they use
# mkstemp()/gmtime_r()/etc. instead -- no extra link libraries needed.
set_source_files_properties(${LUA_SRCS} PROPERTIES COMPILE_DEFINITIONS "LUA_USE_POSIX")

# deps/miniz/miniz.c picks its file-I/O calls from a platform chain. On arm64
# Linux neither __USE_LARGEFILE64 nor __x86_64__ holds, so it lands in that
# chain's last branch, which carries a "#pragma message" note about large
# files. miniz.h gives the remedy itself ("On platforms using glibc, Be sure to
# #define _LARGEFILE64_SOURCE 1 before including miniz.c"): with it the
# fopen64()/ftello64()/stat64() branch is selected, so the note is not compiled
# at all. No flag can silence it instead: GCC reports "#pragma message" as a
# bare note, which -w and -Wno-pragmas both leave in place.
set_source_files_properties(deps/miniz/miniz.c
  PROPERTIES COMPILE_DEFINITIONS "_LARGEFILE64_SOURCE=1")

# The device layer (src/devices/): per-device driver code. It is HES-free by
# construction -- no bus, no lexicon, no manifest, no presence (see
# src/devices/README.md). That is enforced here rather than merely documented:
# the target gets NO include directories, so a device source that tries to
# #include "common/hes_bus.h" simply fails to compile, because src/ is not on
# its include path. Interface modules (src/han/*) depend on it; it depends on
# nothing of this project.
add_library(ohmg_devices_ti_cc2650 OBJECT
  src/devices/ti_cc2650/sensortag.c
  src/devices/ti_cc2650/sensortag_bar.c
  src/devices/ti_cc2650/sensortag_battery.c
  src/devices/ti_cc2650/sensortag_button.c
  src/devices/ti_cc2650/sensortag_humid.c
  src/devices/ti_cc2650/sensortag_light.c
  src/devices/ti_cc2650/sensortag_mic.c
  src/devices/ti_cc2650/sensortag_motion.c
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(ohmg_devices_ti_cc2650 PRIVATE -Wall)
endif()

add_executable(ohmg
  src/app.c
  src/utils.c
  src/cli/cmd_help.c
  src/cli/cmd_bm.c
  src/cli/cmd_bm_show.c
  src/cli/cmd_sm.c
  src/cli/cmd_sm_core.c
  src/cli/cmd_han.c
  src/cli/cmd_han_ble.c
  src/cli/cmd_han_wifi.c
  src/cli/cmd_dm.c
  src/cli/cmd_info.c
  src/bm/bm.c
  src/common/hes_bus.c
  src/common/hes_clock.c
  src/common/hes_devreg.c
  src/common/hes_https.c
  src/common/hes_mreg.c
  src/common/hes_query.c
  src/common/hes_gateway.c
  ${HES_BLE_BACKEND_SRC}
  src/han/ble/ble.c
  src/han/wifi/wifi.c
  src/han/wifi/wiz_client.c
  src/common/hes_dispatch.c
  src/modules/core/core.c
  src/modules/core/app_lua.c
  src/services/auth/auth.c
  src/services/crypto/crypto.c
  src/services/id/id.c
  src/services/time/geo_tz.c
  src/services/time/sntp.c
  src/services/time/time.c
  src/services/time/time_sync.c
  src/dm/dm.c
  deps/log/log.c
  deps/argtable3/argtable3.c
  deps/bstrlib/bstrlib.c
  deps/cwalk/src/cwalk.c
  deps/miniz/miniz.c
  ${LUA_SRCS}
)
# The BLE HAN module (hes_ble_bluez.c) talks to BlueZ via GLib/GIO; link GLIB2
# so its include dirs + libs reach that source. The SensorTag device accessors
# it calls (src/devices/ti_cc2650/) use only libc and come in via
# ohmg_devices_ti_cc2650, the HES-free device-layer object library above.
#
# CURL::libcurl is the HTTPS GET the time service's time zone lookup uses
# (services/time/geo_tz.c -> common/hes_https.c); it brings the vendored mbed
# TLS in as its TLS backend. TF-PSA-Crypto is the crypto core the identity
# service calls directly. Threads::Threads is for the time service's sync
# thread.
target_link_libraries(ohmg PRIVATE nng LibXml2 uv_a jansson::jansson
  CURL::libcurl TF-PSA-Crypto::tfpsacrypto
  Threads::Threads PkgConfig::GLIB2 ohmg_devices_ti_cc2650)

# Tell ble.c when the sim backend is active, so it can compile the simulated
# button auto-toggle (a hardware-free demo stand-in) only for sim builds.
# In a real bluez build the button must come exclusively from GATT notify,
# never from a simulated toggle.
if(BLE_BACKEND STREQUAL "sim")
  target_compile_definitions(ohmg PRIVATE HES_BLE_BACKEND_SIM=1)
endif()

target_include_directories(ohmg PRIVATE
  deps/nng/include
  deps/libxml2/include
  deps/log
  deps/argtable3
  deps/bstrlib
  deps/cwalk/include
  deps/miniz
  deps/lua/src
  deps/jansson/src
  ${CMAKE_CURRENT_BINARY_DIR}
  src
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(ohmg PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# Unit tests (opt-in). Builds tests/ against the REAL vendored NNG + libxml2
# (no stub) and registers them with ctest:
#   cmake -B build-amd64 -DOPENHES_BUILD_TESTS=ON
#   cmake --build build-amd64
#   ctest --test-dir build-amd64 --output-on-failure
# ---------------------------------------------------------------------------
option(OPENHES_BUILD_TESTS "Build the ohmg unit tests under tests/" OFF)
if(OPENHES_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# The standalone SensorTag hardware programs in src/devices/ti_cc2650/tests are
# a SEPARATE opt-in from the unit tests above: each is a BlueZ D-Bus client with
# its own main() that connects to a real SensorTag and prints a reading, so it
# needs a Bluetooth adapter, a paired/trusted tag and a running BlueZ. They are
# therefore not ctest tests and not part of `make all` -- just 7 binaries you
# build when you have the hardware (see src/devices/ti_cc2650/README.md).
option(OPENHES_BUILD_DEVICE_TESTS
  "Build the standalone SensorTag hardware programs in src/devices/*/tests" OFF)
if(OPENHES_BUILD_DEVICE_TESTS)
  add_subdirectory(src/devices/ti_cc2650/tests)
endif()
