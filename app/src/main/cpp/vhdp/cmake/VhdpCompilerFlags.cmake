# Compiler/linker policy for first-party targets.
#  vhdp_flags    : hardening, RTTI policy, sanitizers (link into every first-party target)
#  vhdp_warnings : strict warnings (+ -Werror when VHDP_WERROR) for first-party sources only
# Guest fixture programs deliberately do not link these (they are static, unsanitized).
include_guard(GLOBAL)
include(CheckPIESupported)
check_pie_supported(LANGUAGES C CXX)

# Conservative optimisation baseline for distribution artifacts (see docs/adr/0002).
foreach(lang C CXX ASM)
  set(CMAKE_${lang}_FLAGS_RELEASE "-O2 -DNDEBUG")
  set(CMAKE_${lang}_FLAGS_RELWITHDEBINFO "-O1 -g -DNDEBUG")
  set(CMAKE_${lang}_FLAGS_DEBUG "-O0 -g")
endforeach()

add_library(vhdp_flags INTERFACE)
target_compile_options(vhdp_flags INTERFACE
  $<$<COMPILE_LANGUAGE:C,CXX>:-fstack-protector-strong>
  $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<CONFIG:Debug>>>:-U_FORTIFY_SOURCE>
  $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<CONFIG:Debug>>>:-D_FORTIFY_SOURCE=2>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  -fno-omit-frame-pointer)
if(VHDP_ARCH STREQUAL "x86_64")
  target_compile_options(vhdp_flags INTERFACE -fcf-protection=full)
elseif(VHDP_ARCH STREQUAL "aarch64")
  target_compile_options(vhdp_flags INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:-mbranch-protection=standard>)
endif()
target_link_options(vhdp_flags INTERFACE
  -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
if(ANDROID)
  target_link_options(vhdp_flags INTERFACE -Wl,-z,max-page-size=16384)
endif()
target_compile_definitions(vhdp_flags INTERFACE
  VHDP_ARCH_NAME="${VHDP_ARCH}"
  $<$<BOOL:${VHDP_USE_ASM}>:VHDP_USE_ASM=1>)

if(VHDP_SANITIZE)
  target_compile_options(vhdp_flags INTERFACE
    -fsanitize=${VHDP_SANITIZE} -fno-sanitize-recover=all)
  target_link_options(vhdp_flags INTERFACE -fsanitize=${VHDP_SANITIZE})
  target_compile_definitions(vhdp_flags INTERFACE VHDP_SANITIZER_BUILD=1)
endif()

add_library(vhdp_warnings INTERFACE)
target_compile_options(vhdp_warnings INTERFACE
  $<$<COMPILE_LANGUAGE:C,CXX>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2 -Wundef -Wvla -Wimplicit-fallthrough -Wcast-align>
  $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Woverloaded-virtual>
  $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes -Wmissing-prototypes>)
if(VHDP_WERROR)
  target_compile_options(vhdp_warnings INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:-Werror>)
endif()

# Helper: registers a CTest test, prefixing VHDP_TEST_LAUNCHER (e.g. setarch -R for TSan).
function(vhdp_add_test name)
  cmake_parse_arguments(T "" "WORKING_DIRECTORY" "COMMAND;LABELS;FIXTURES_REQUIRED;FIXTURES_SETUP;ENVIRONMENT" ${ARGN})
  set(cmd ${T_COMMAND})
  if(VHDP_TEST_LAUNCHER)
    set(cmd ${VHDP_TEST_LAUNCHER} ${T_COMMAND})
  endif()
  if(T_WORKING_DIRECTORY)
    add_test(NAME ${name} COMMAND ${cmd} WORKING_DIRECTORY ${T_WORKING_DIRECTORY})
  else()
    add_test(NAME ${name} COMMAND ${cmd})
  endif()
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  if(T_LABELS)
    set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
  endif()
  if(T_FIXTURES_REQUIRED)
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${T_FIXTURES_REQUIRED}")
  endif()
  if(T_FIXTURES_SETUP)
    set_tests_properties(${name} PROPERTIES FIXTURES_SETUP "${T_FIXTURES_SETUP}")
  endif()
  if(T_ENVIRONMENT)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${T_ENVIRONMENT}")
  endif()
endfunction()
