# Host toolchain using Clang as the reference compiler.
# Clang on PATH is preferred; otherwise the prebuilt host Clang shipped with the
# pinned Android NDK is used (it targets x86_64-unknown-linux-gnu and uses the
# system libstdc++/glibc). Configuration fails with an actionable message when
# neither is available, instead of silently falling back to another compiler.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../VhdpFindNdk.cmake")

if(NOT VHDP_LLVM_BIN_DIR)
  find_program(_vhdp_path_clang NAMES clang NO_CACHE)
  if(_vhdp_path_clang)
    get_filename_component(_vhdp_bin "${_vhdp_path_clang}" DIRECTORY)
  else()
    vhdp_find_ndk(_vhdp_ndk)
    if(_vhdp_ndk)
      set(_vhdp_bin "${_vhdp_ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin")
    endif()
  endif()
  if(NOT _vhdp_bin OR NOT EXISTS "${_vhdp_bin}/clang")
    message(FATAL_ERROR
      "host-clang toolchain: no Clang found on PATH and ${VHDP_NDK_SEARCH_REPORT}. "
      "Install clang or set ANDROID_NDK_HOME to NDK ${VHDP_NDK_VERSION}.")
  endif()
  set(VHDP_LLVM_BIN_DIR "${_vhdp_bin}" CACHE PATH "Directory with clang, clang-format, clang-tidy, llvm-nm")
endif()

set(CMAKE_C_COMPILER "${VHDP_LLVM_BIN_DIR}/clang")
set(CMAKE_CXX_COMPILER "${VHDP_LLVM_BIN_DIR}/clang++")
set(CMAKE_ASM_COMPILER "${VHDP_LLVM_BIN_DIR}/clang")
