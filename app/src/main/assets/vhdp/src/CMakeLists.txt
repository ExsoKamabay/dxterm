find_package(Threads REQUIRED)

configure_file(core/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/vhdp_version.hpp" @ONLY)

set(VHDP_LIBRARY_SOURCES
  common/status.cpp
  common/json.cpp
  common/strings.cpp
  linux_abi/errno_table.c
  linux_abi/elf.c
  linux_abi/syscall_table.cpp
  vfs/guest_path.cpp
  vfs/mount_table.cpp
  vfs/resolver.cpp
  core/config.cpp
  core/lifecycle.cpp
  core/events.cpp
  core/engine_select.cpp
  core/context.cpp
  core/session.cpp
  core/capabilities.cpp
  core/doctor.cpp
  core/inspect.cpp
  core/configure.cpp
  core/dpkg_recovery.cpp
  core/projection.cpp
  engines/unavailable/unavailable_engine.cpp
  engines/rootless/rootless_engine.cpp
  engines/rootless/supervisor.cpp
  engines/rootless/syscall_handlers.cpp
  engines/rootless/exec_resolver.cpp
  engines/rootless/link_store.cpp
  engines/rootless/proc_synth.cpp
  engines/rootless/seccomp_filter.cpp
  engines/rootless/termios2.cpp
  engines/rootless/tracee_mem.cpp
  engines/rootless/bootstrap.c
  platform/linux/proc.cpp
  platform/linux/host_probe.cpp
  platform/linux/probe_child.c
  arch/portable/raw_syscall_portable.c
  "${PROJECT_SOURCE_DIR}/hdr/hardware_registry.cpp"
  "${PROJECT_SOURCE_DIR}/vdr/drivers.cpp"
  capi/capi.cpp)

if(VHDP_ARCH STREQUAL "x86_64")
  list(APPEND VHDP_LIBRARY_SOURCES arch/x86_64/regs_x86_64.cpp)
  if(VHDP_USE_ASM)
    list(APPEND VHDP_LIBRARY_SOURCES arch/x86_64/raw_syscall.S)
  endif()
elseif(VHDP_ARCH STREQUAL "aarch64")
  list(APPEND VHDP_LIBRARY_SOURCES arch/aarch64/regs_aarch64.cpp)
  if(VHDP_USE_ASM)
    list(APPEND VHDP_LIBRARY_SOURCES arch/aarch64/raw_syscall.S)
  endif()
else()
  message(FATAL_ERROR "libvhdp's rootless engine supports x86_64 and aarch64 targets only (got ${CMAKE_SYSTEM_PROCESSOR}).")
endif()

add_library(vhdp_objects OBJECT ${VHDP_LIBRARY_SOURCES})
target_include_directories(vhdp_objects
  PUBLIC "${PROJECT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(vhdp_objects PRIVATE VHDP_BUILDING_LIBRARY=1)
target_link_libraries(vhdp_objects PRIVATE vhdp_flags vhdp_warnings Threads::Threads)

# Shared library: only the C ABI allowlist is exported.
add_library(vhdp SHARED $<TARGET_OBJECTS:vhdp_objects>)
target_include_directories(vhdp PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vhdp PRIVATE vhdp_flags Threads::Threads)
set(VHDP_EXPORT_MAP "${CMAKE_CURRENT_SOURCE_DIR}/capi/libvhdp.map")
target_link_options(vhdp PRIVATE "-Wl,--version-script=${VHDP_EXPORT_MAP}" -Wl,-z,defs)
if(NOT ANDROID AND NOT VHDP_SANITIZE)
  # Keep libstdc++ private to the library (its symbols are hidden by the map).
  target_link_options(vhdp PRIVATE -static-libstdc++ -static-libgcc)
endif()
set_target_properties(vhdp PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  LINK_DEPENDS "${VHDP_EXPORT_MAP}")

# Static library for self-contained executables (phdp) and white-box tests.
add_library(vhdp_static STATIC $<TARGET_OBJECTS:vhdp_objects>)
set_target_properties(vhdp_static PROPERTIES OUTPUT_NAME vhdp)
target_include_directories(vhdp_static PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(vhdp_static INTERFACE Threads::Threads)

# White-box access to internal headers (tests, fuzz, benchmarks only).
add_library(vhdp_internal INTERFACE)
target_link_libraries(vhdp_internal INTERFACE vhdp_static)
target_include_directories(vhdp_internal INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")

# CLI: parser is a separate library so it can be unit tested; it only uses the
# public headers. Skipped entirely when VHDP_BUILD_CLI=OFF (embedded consumers).
if(VHDP_BUILD_CLI)
  add_library(phdp_cli STATIC cli/cli_args.cpp cli/json_dom.cpp)
  target_include_directories(phdp_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cli" "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(phdp_cli PRIVATE vhdp_flags vhdp_warnings)

  add_executable(phdp cli/main.cpp cli/cli_run.cpp cli/cli_report.cpp)
  target_link_libraries(phdp PRIVATE phdp_cli vhdp_static vhdp_flags vhdp_warnings)
  if(NOT ANDROID AND NOT VHDP_SANITIZE)
    target_link_options(phdp PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

# Userland ELF loader (engines/rootless/loader). The rootless engine executes it in place of a
# guest program wherever the host forbids execve() of the guest's files -- an Android app
# targeting API 29+ may not exec anything from its writable storage, but may exec from its
# nativeLibraryDir and may still map app files PROT_EXEC. It is a freestanding static PIE with
# no dynamic relocations, so it must not pick up vhdp_flags (stack protector, sanitizers,
# fortify) and needs the assembly syscall gateway whatever VHDP_USE_ASM says.
# Built together with the CLI: AGP cannot package executables from the app's embedded CMake,
# so prebuilts/vhdp-cli/build.sh ships it as jniLibs/<abi>/libvhdp-loader.so.
if(VHDP_BUILD_CLI)
  add_executable(vhdp_loader
    engines/rootless/loader/loader.c
    engines/rootless/loader/loader_entry_${VHDP_ARCH}.S
    arch/${VHDP_ARCH}/raw_syscall.S)
  set_target_properties(vhdp_loader PROPERTIES OUTPUT_NAME vhdp-loader)
  target_include_directories(vhdp_loader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(vhdp_loader PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-O2 -fPIE -ffreestanding -fno-builtin -fno-stack-protector
                            -fno-asynchronous-unwind-tables -fno-unwind-tables -U_FORTIFY_SOURCE>)
  if(VHDP_ARCH STREQUAL "x86_64")
    target_compile_options(vhdp_loader PRIVATE $<$<COMPILE_LANGUAGE:C>:-fcf-protection=none>)
  else()
    target_compile_options(vhdp_loader PRIVATE $<$<COMPILE_LANGUAGE:C>:-mbranch-protection=none>)
  endif()
  target_link_options(vhdp_loader PRIVATE
    -nostdlib -static-pie -Wl,--no-dynamic-linker -Wl,-e,_start -Wl,-z,noexecstack
    -Wl,-z,max-page-size=16384 -Wl,--build-id=none)
endif()
