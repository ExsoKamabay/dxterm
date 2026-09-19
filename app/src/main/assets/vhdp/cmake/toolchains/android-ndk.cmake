# Wrapper that locates the pinned Android NDK and delegates to the official
# NDK CMake toolchain file (build/cmake/android.toolchain.cmake). No flags are
# overridden here; ABI/platform/STL come from CMakePresets.json.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../VhdpFindNdk.cmake")

vhdp_find_ndk(_vhdp_ndk)
if(NOT _vhdp_ndk)
  message(FATAL_ERROR "android-ndk toolchain: ${VHDP_NDK_SEARCH_REPORT}. "
    "Install NDK ${VHDP_NDK_VERSION} (sdkmanager \"ndk;${VHDP_NDK_VERSION}\") or set ANDROID_NDK_HOME.")
endif()
set(VHDP_ANDROID_NDK "${_vhdp_ndk}" CACHE PATH "Android NDK in use")
set(VHDP_LLVM_BIN_DIR "${_vhdp_ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin" CACHE PATH "NDK LLVM tools")
set(ANDROID_NDK "${_vhdp_ndk}")
include("${_vhdp_ndk}/build/cmake/android.toolchain.cmake")
