# format / format-check / tidy targets for first-party sources.
# When a tool is missing the target fails with an explicit NOT RUN message
# instead of reporting success.
include_guard(GLOBAL)

set(_vhdp_hints "")
if(VHDP_LLVM_BIN_DIR)
  list(APPEND _vhdp_hints "${VHDP_LLVM_BIN_DIR}")
endif()
find_program(VHDP_CLANG_FORMAT NAMES clang-format HINTS ${_vhdp_hints})
find_program(VHDP_CLANG_TIDY NAMES clang-tidy HINTS ${_vhdp_hints})
find_program(VHDP_LLVM_NM NAMES llvm-nm nm HINTS ${_vhdp_hints})
find_program(VHDP_LLVM_READELF NAMES llvm-readelf readelf HINTS ${_vhdp_hints})

file(GLOB_RECURSE VHDP_FORMAT_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/include/*.h" "${PROJECT_SOURCE_DIR}/include/*.hpp"
  "${PROJECT_SOURCE_DIR}/src/*.c" "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/src/*.hpp"
  "${PROJECT_SOURCE_DIR}/hdr/*.cpp" "${PROJECT_SOURCE_DIR}/hdr/*.hpp"
  "${PROJECT_SOURCE_DIR}/vdr/*.cpp" "${PROJECT_SOURCE_DIR}/vdr/*.hpp"
  "${PROJECT_SOURCE_DIR}/tests/*.c" "${PROJECT_SOURCE_DIR}/tests/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.h" "${PROJECT_SOURCE_DIR}/tests/*.hpp"
  "${PROJECT_SOURCE_DIR}/examples/*.c" "${PROJECT_SOURCE_DIR}/examples/*.cpp"
  "${PROJECT_SOURCE_DIR}/fuzz/*.cpp" "${PROJECT_SOURCE_DIR}/benchmarks/*.cpp")

file(GLOB_RECURSE VHDP_TIDY_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/src/*.c" "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/hdr/*.cpp" "${PROJECT_SOURCE_DIR}/vdr/*.cpp")
# Architecture-specific translation units that are not part of this build have no
# compile_commands.json entry; clang-tidy only runs on files that were compiled.
if(NOT VHDP_ARCH STREQUAL "x86_64")
  list(FILTER VHDP_TIDY_SOURCES EXCLUDE REGEX "/arch/x86_64/")
endif()
if(NOT VHDP_ARCH STREQUAL "aarch64")
  list(FILTER VHDP_TIDY_SOURCES EXCLUDE REGEX "/arch/aarch64/")
endif()
if(NOT ANDROID)
  list(FILTER VHDP_TIDY_SOURCES EXCLUDE REGEX "/platform/android/")
endif()

if(VHDP_CLANG_FORMAT)
  add_custom_target(format-check
    COMMAND "${VHDP_CLANG_FORMAT}" --style=file --dry-run --Werror ${VHDP_FORMAT_SOURCES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format --dry-run --Werror (${VHDP_CLANG_FORMAT})"
    VERBATIM)
  add_custom_target(format
    COMMAND "${VHDP_CLANG_FORMAT}" --style=file -i ${VHDP_FORMAT_SOURCES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    VERBATIM)
else()
  add_custom_target(format-check
    COMMAND "${CMAKE_COMMAND}" -E echo "NOT RUN: clang-format not found (set VHDP_LLVM_BIN_DIR)"
    COMMAND "${CMAKE_COMMAND}" -E false)
endif()

if(VHDP_CLANG_TIDY)
  add_custom_target(tidy
    COMMAND "${CMAKE_COMMAND}"
      -DCLANG_TIDY=${VHDP_CLANG_TIDY}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      "-DSOURCES=${VHDP_TIDY_SOURCES}"
      -P "${PROJECT_SOURCE_DIR}/cmake/RunClangTidy.cmake"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-tidy (${VHDP_CLANG_TIDY})"
    VERBATIM)
else()
  add_custom_target(tidy
    COMMAND "${CMAKE_COMMAND}" -E echo "NOT RUN: clang-tidy not found (set VHDP_LLVM_BIN_DIR)"
    COMMAND "${CMAKE_COMMAND}" -E false)
endif()
