cmake_minimum_required(VERSION 3.22)

project(VHDP
  VERSION 1.0.0
  DESCRIPTION "Virtual Hardware Driver Platform"
  LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Tests and examples require running built binaries on the build host, so they are
# off by default when cross-compiling for Android (device testing is a separate
# step; see docs/ANDROID_BUILD.md).
if(ANDROID)
  set(VHDP_HOST_RUNNABLE_DEFAULT OFF)
else()
  set(VHDP_HOST_RUNNABLE_DEFAULT ON)
endif()

option(VHDP_ENABLE_ASM "Use hand-written assembly boundary routines where available" ON)
option(VHDP_FORCE_PORTABLE "Force the portable C reference implementations of assembly routines" OFF)
option(VHDP_ENABLE_ROOTED "Build the rooted namespace/chroot engine (not implemented in this milestone)" OFF)
option(VHDP_BUILD_TESTS "Build unit and integration tests" ${VHDP_HOST_RUNNABLE_DEFAULT})
option(VHDP_BUILD_EXAMPLES "Build C and C++ embedding examples" ${VHDP_HOST_RUNNABLE_DEFAULT})
# Embedding consumers (e.g. an Android app linking libvhdp) turn this off so the phdp
# CLI, its parser library and its install rule stay out of their build graph.
option(VHDP_BUILD_CLI "Build the phdp command-line tool and its parser library" ON)
option(VHDP_WERROR "Treat warnings in first-party sources as errors" ON)
set(VHDP_SANITIZE "" CACHE STRING "Sanitizer list passed to -fsanitize= (empty disables)")
set(VHDP_TEST_LAUNCHER "" CACHE STRING "Command prefix used to launch test executables (e.g. setarch -R for TSan)")

if(VHDP_ENABLE_ROOTED)
  message(FATAL_ERROR
    "VHDP_ENABLE_ROOTED=ON was requested, but the rooted engine is not implemented in this "
    "milestone. See docs/CAPABILITIES.md (profile 'rooted' is backend-required).")
endif()

if(VHDP_BUILD_TESTS AND NOT VHDP_BUILD_CLI)
  message(FATAL_ERROR
    "VHDP_BUILD_TESTS=ON requires VHDP_BUILD_CLI=ON: the unit tests link phdp_cli and the "
    "integration tests drive phdp. Set VHDP_BUILD_TESTS=OFF for an embedded build.")
endif()

if(VHDP_BUILD_TESTS AND NOT VHDP_BUILD_EXAMPLES)
  message(FATAL_ERROR
    "VHDP_BUILD_TESTS=ON requires VHDP_BUILD_EXAMPLES=ON: the integration tests link and run the "
    "embed examples (T-EX-C-EMBED / T-EX-CPP-EMBED depend on embed_run_c / embed_run_cpp). "
    "Set VHDP_BUILD_TESTS=OFF for an embedded build.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VhdpArch)
include(VhdpCompilerFlags)
include(VhdpTools)

include(GNUInstallDirs)

add_subdirectory(src)

if(VHDP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(VHDP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(fuzz)
  add_subdirectory(benchmarks)
endif()

include(VhdpInstall)
