# Copyright (c) 2026 Sachu James. SPDX-License-Identifier: MIT.
#
# TickForge: deterministic market microstructure simulator.
# Day 01: build foundation only. No simulation subsystems are implemented yet.

cmake_minimum_required(VERSION 3.22)

project(tickforge
  VERSION 0.1.0
  DESCRIPTION "Deterministic market microstructure simulator"
  HOMEPAGE_URL "https://github.com/SachuJames/tickforge"
  LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings are applied per-target to TickForge's own targets only, so that
# third-party dependency builds (e.g. GoogleTest via FetchContent) are not
# affected by our warning set.
add_library(tickforge_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tickforge_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
  )
endif()

# ---------------------------------------------------------------------------
# Sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer)
# ---------------------------------------------------------------------------
option(TICKFORGE_ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" OFF)
if(TICKFORGE_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "Sanitizers require GCC or Clang; ignoring TICKFORGE_ENABLE_SANITIZERS")
  else()
    add_library(tickforge_sanitizers INTERFACE)
    target_compile_options(tickforge_sanitizers INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tickforge_sanitizers INTERFACE
      -fsanitize=address,undefined)
  endif()
endif()

# ---------------------------------------------------------------------------
# Version header. Single source of truth: the project() VERSION above.
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/tickforge/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/tickforge/version.hpp"
  @ONLY
)

add_library(tickforge_core INTERFACE)
target_include_directories(tickforge_core INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
)
target_link_libraries(tickforge_core INTERFACE tickforge_warnings)
if(TARGET tickforge_sanitizers)
  target_link_libraries(tickforge_core INTERFACE tickforge_sanitizers)
endif()

# ---------------------------------------------------------------------------
# Event model library (Day 02): canonical normalized event types.
# Public headers live in include/tickforge/event/; non-trivial function
# definitions live in src/event/.
# ---------------------------------------------------------------------------
add_library(tickforge_event STATIC src/event/event.cpp)
target_link_libraries(tickforge_event PUBLIC tickforge_core)

# ---------------------------------------------------------------------------
# Main executable (Day 01: proves the build works, nothing more)
# ---------------------------------------------------------------------------
add_executable(tickforge src/main.cpp)
target_link_libraries(tickforge PRIVATE tickforge_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(TICKFORGE_BUILD_TESTS "Build TickForge unit tests" ON)
if(TICKFORGE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  # Avoid overriding the parent project's compiler/linker settings on Windows.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(tickforge_tests
    tests/test_smoke.cpp
    tests/test_primitives.cpp
    tests/test_side_and_type.cpp
    tests/test_event.cpp
    tests/test_event_ordering.cpp
    tests/test_event_validation.cpp
  )
  target_link_libraries(tickforge_tests PRIVATE tickforge_core tickforge_event GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(tickforge_tests)
endif()
