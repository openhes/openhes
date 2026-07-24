cmake_minimum_required(VERSION 3.28)
project(ohsrv)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(HIREDIS QUIET hiredis)
  pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
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
set(NANOMQ_TESTS OFF CACHE BOOL "" FORCE)
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/nanomq)

# Keep third-party code quiet without changing vendored sources.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  foreach(_third_party_target nng nng_testing nanomq nanomq_cli)
    if(TARGET ${_third_party_target})
      target_compile_options(${_third_party_target} PRIVATE -w)
    endif()
  endforeach()
endif()

set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)  # Disable building xmllint, xmlcatalog, etc.
set(LIBXML2_WITH_TESTS OFF CACHE BOOL "" FORCE)     # Disable tests
add_subdirectory(deps/libxml2)

# Suppress warnings from libxml2 (optional, like you do with nanomq)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  if(TARGET LibXml2)
    target_compile_options(LibXml2 PRIVATE -w)
  endif()
endif()

add_executable(ohsrv
  src/app.c
  src/hes_bm.c
)
target_link_libraries(ohsrv PRIVATE nng LibXml2)

target_include_directories(ohsrv PRIVATE
  deps/nanomq/nanomq/include
  deps/nanomq/nng/include
  deps/libxml2/include
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(ohsrv PRIVATE -Wall)
endif()

# MQTT server

add_executable(hes_han
  src/hes_han.c
)
target_link_libraries(hes_han PRIVATE nng)

target_include_directories(hes_han PRIVATE
  deps/nanomq/nanomq/include
  deps/nanomq/nng/include
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(hes_han PRIVATE -Wall)
endif()

# # Vendored Mosquitto (client library only)
# set(WITH_BROKER OFF CACHE BOOL "" FORCE)
# set(WITH_CLIENTS OFF CACHE BOOL "" FORCE)
# set(WITH_APPS OFF CACHE BOOL "" FORCE)
# set(WITH_PLUGINS OFF CACHE BOOL "" FORCE)
# set(WITH_DOCS OFF CACHE BOOL "" FORCE)
# set(WITH_TESTS OFF CACHE BOOL "" FORCE)
# set(WITH_LIB_CPP OFF CACHE BOOL "" FORCE)
# set(WITH_STATIC_LIBRARIES ON CACHE BOOL "" FORCE)

# Optional: if you don't need TLS now
# set(WITH_TLS OFF CACHE BOOL "" FORCE)

# add_subdirectory(deps/mosquitto EXCLUDE_FROM_ALL)

# MQTT publisher

add_executable(mqtt_pub
  src/mqtt_pub.c
)

target_link_libraries(mqtt_pub PRIVATE nng)

target_include_directories(mqtt_pub PRIVATE
  deps/nanomq/nanomq/include
  deps/nanomq/nng/include
  deps/mosquitto/include
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(mqtt_pub PRIVATE -Wall)
endif()

# MQTT subscriber

add_executable(mqtt_sub
  src/mqtt_sub.c
)

target_link_libraries(mqtt_sub PRIVATE nng)

target_include_directories(mqtt_sub PRIVATE
  deps/nanomq/nanomq/include
  deps/nanomq/nng/include
  deps/mosquitto/include
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(mqtt_sub PRIVATE -Wall)
endif()

# test

add_executable(test
  src/test.c
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(test PRIVATE -Wall)
endif()

# sensortag button — reads BLE button state from TI CC2650 SensorTag
# via D-Bus / GattCharacteristic1 notifications using GLib + GIO.
# Linked dynamically (glib only ships .so on Ubuntu arm64).

add_executable(sensortag_button
  src/sensortag_button.c
)

target_include_directories(sensortag_button PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_button PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_button PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# sensortag_bar – Barometric pressure reader (CC2650 SensorTag)
# Like sensortag_button, but discovers characteristics by UUID at runtime
# and reads pressure data every second.
# ---------------------------------------------------------------------------
add_executable(sensortag_bar
  src/sensortag_bar.c
)

target_include_directories(sensortag_bar PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_bar PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_bar PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# sensortag_battery – Battery level reader (CC2650 SensorTag)
# Reads the standard Battery Level characteristic once and exits.
# ---------------------------------------------------------------------------
add_executable(sensortag_battery
  src/sensortag_battery.c
)

target_include_directories(sensortag_battery PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_battery PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_battery PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# sensortag_humid – Humidity / temperature reader (CC2650 SensorTag)
# Enables the HDC1000 sensor and reads temperature + % RH every second.
# ---------------------------------------------------------------------------
add_executable(sensortag_humid
  src/sensortag_humid.c
)

target_include_directories(sensortag_humid PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_humid PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_humid PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# sensortag_light – Ambient light (OPT3001) reader (CC2650 SensorTag)
# Enables the OPT3001 sensor and reads illuminance (Lux) every second.
# ---------------------------------------------------------------------------
add_executable(sensortag_light
  src/sensortag_light.c
)

target_include_directories(sensortag_light PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_light PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_light PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# sensortag_mic – Digital microphone (SPH0641LU4H) reader (CC2650 SensorTag)
# Enables the microphone and displays a real-time sound level meter.
# ---------------------------------------------------------------------------
add_executable(sensortag_mic
  src/sensortag_mic.c
)

target_include_directories(sensortag_mic PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_mic PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_mic PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# sensortag_motion – 9-axis motion (MPU-9250) reader (CC2650 SensorTag)
# Enables gyroscope, accelerometer and magnetometer and streams all axes.
# ---------------------------------------------------------------------------
add_executable(sensortag_motion
  src/sensortag_motion.c
)

target_include_directories(sensortag_motion PRIVATE
  /usr/aarch64-linux-gnu/include/glib-2.0
  /usr/aarch64-linux-gnu/lib/glib-2.0/include
)

target_link_libraries(sensortag_motion PRIVATE
  PkgConfig::GLIB2
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(sensortag_motion PRIVATE -Wall)
endif()
