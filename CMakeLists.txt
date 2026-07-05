cmake_minimum_required(VERSION 3.28)
project(ohsrv)

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

add_executable(ohsrv
  src/app.c
)
target_link_libraries(ohsrv PRIVATE nng)

target_include_directories(ohsrv PRIVATE
  deps/nanomq/nanomq/include
  deps/nanomq/nng/include
)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(ohsrv PRIVATE -Wall)
endif()
