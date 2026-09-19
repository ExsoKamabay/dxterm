# Host toolchain using GCC. Used for the sanitizer presets because the NDK's
# host Clang does not ship host (x86_64-linux-gnu) sanitizer runtimes, and as
# the GCC compatibility build. LLVM tools (clang-format/clang-tidy/llvm-nm) are
# still discovered from PATH or the pinned NDK when available.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../VhdpFindNdk.cmake")

find_program(_vhdp_gcc NAMES gcc NO_CACHE)
find_program(_vhdp_gxx NAMES g++ NO_CACHE)
if(NOT _vhdp_gcc OR NOT _vhdp_gxx)
  message(FATAL_ERROR "host-gcc toolchain: gcc/g++ not found on PATH.")
endif()
set(CMAKE_C_COMPILER "${_vhdp_gcc}")
set(CMAKE_CXX_COMPILER "${_vhdp_gxx}")
set(CMAKE_ASM_COMPILER "${_vhdp_gcc}")

if(NOT VHDP_LLVM_BIN_DIR)
  vhdp_find_ndk(_vhdp_ndk)
  if(_vhdp_ndk)
    set(VHDP_LLVM_BIN_DIR "${_vhdp_ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin"
      CACHE PATH "Directory with clang-format, clang-tidy, llvm-nm")
  endif()
endif()
