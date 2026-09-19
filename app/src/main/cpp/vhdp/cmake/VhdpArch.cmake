# Normalizes the target architecture name used to pick arch/ sources.
include_guard(GLOBAL)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _vhdp_proc)
if(_vhdp_proc MATCHES "^(x86_64|amd64)$")
  set(VHDP_ARCH "x86_64")
elseif(_vhdp_proc MATCHES "^(aarch64|arm64)$")
  set(VHDP_ARCH "aarch64")
else()
  set(VHDP_ARCH "unsupported")
endif()

if(ANDROID)
  set(VHDP_PLATFORM "android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(VHDP_PLATFORM "linux")
else()
  message(FATAL_ERROR "VHDP targets Linux and Android only (CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}).")
endif()

if(VHDP_FORCE_PORTABLE OR NOT VHDP_ENABLE_ASM OR VHDP_ARCH STREQUAL "unsupported")
  set(VHDP_USE_ASM OFF)
else()
  set(VHDP_USE_ASM ON)
endif()

message(STATUS "VHDP: arch=${VHDP_ARCH} platform=${VHDP_PLATFORM} asm=${VHDP_USE_ASM}")
