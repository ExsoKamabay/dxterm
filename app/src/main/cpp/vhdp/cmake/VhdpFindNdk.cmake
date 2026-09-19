# Locates the Android NDK used both for Android cross builds and, when no host
# Clang is on PATH, as the source of the reference host Clang/LLVM tools.
#
# Search order (first hit wins):
#   1. -DVHDP_ANDROID_NDK=<dir> or environment VHDP_ANDROID_NDK
#   2. ANDROID_NDK_HOME, ANDROID_NDK_ROOT, ANDROID_NDK environment variables
#   3. $ANDROID_HOME/ndk/<VHDP_NDK_VERSION>, $ANDROID_SDK_ROOT/ndk/<...>, $HOME/Android/Sdk/ndk/<...>
# The pinned version is VHDP_NDK_VERSION. Other installed versions are never picked
# silently; the error message lists them so the user can pin explicitly.

include_guard(GLOBAL)

set(VHDP_NDK_VERSION "29.0.14206865" CACHE STRING "Pinned Android NDK revision")

function(vhdp_find_ndk out_var)
  set(candidates "")
  foreach(v IN ITEMS VHDP_ANDROID_NDK)
    if(DEFINED ${v} AND NOT "${${v}}" STREQUAL "")
      list(APPEND candidates "${${v}}")
    endif()
  endforeach()
  foreach(e IN ITEMS VHDP_ANDROID_NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK)
    if(DEFINED ENV{${e}} AND NOT "$ENV{${e}}" STREQUAL "")
      list(APPEND candidates "$ENV{${e}}")
    endif()
  endforeach()
  set(sdk_roots "")
  foreach(e IN ITEMS ANDROID_HOME ANDROID_SDK_ROOT)
    if(DEFINED ENV{${e}} AND NOT "$ENV{${e}}" STREQUAL "")
      list(APPEND sdk_roots "$ENV{${e}}")
    endif()
  endforeach()
  if(DEFINED ENV{HOME})
    list(APPEND sdk_roots "$ENV{HOME}/Android/Sdk")
  endif()
  foreach(root IN LISTS sdk_roots)
    list(APPEND candidates "${root}/ndk/${VHDP_NDK_VERSION}")
  endforeach()

  foreach(dir IN LISTS candidates)
    if(EXISTS "${dir}/build/cmake/android.toolchain.cmake")
      set(${out_var} "${dir}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  set(found_versions "")
  foreach(root IN LISTS sdk_roots)
    file(GLOB vers LIST_DIRECTORIES true "${root}/ndk/*")
    list(APPEND found_versions ${vers})
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
  set(VHDP_NDK_SEARCH_REPORT
    "pinned NDK ${VHDP_NDK_VERSION} not found; searched: ${candidates}; other installed NDKs: ${found_versions}"
    PARENT_SCOPE)
endfunction()
