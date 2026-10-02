cmake_minimum_required(VERSION 3.16)
project(claw_runner VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CLAW_BUILD_TESTS "Build unit/integration tests" ON)
option(CLAW_STATIC_OPENSSL "Link OpenSSL statically" OFF)

set(OPENSSL_USE_STATIC_LIBS ${CLAW_STATIC_OPENSSL})
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_library(claw_core STATIC
  src/util.cpp
  src/http.cpp
  src/catalog.cpp
  src/router.cpp
  src/scanner.cpp
  src/proxy.cpp
  src/repair.cpp
  src/launcher.cpp
  src/policy.cpp
  src/skills.cpp
  src/browse.cpp
  src/fstools.cpp
  src/route.cpp
  src/lineedit.cpp
  src/mdstream.cpp
  src/activity.cpp
  src/language.cpp
  src/agents.cpp)
target_include_directories(claw_core PUBLIC src third_party)
target_compile_definitions(claw_core PUBLIC
  CPPHTTPLIB_OPENSSL_SUPPORT
  CLAW_VERSION="${PROJECT_VERSION}")
target_link_libraries(claw_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

if(WIN32)
  target_compile_definitions(claw_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX)
  target_link_libraries(claw_core PUBLIC ws2_32 crypt32)
elseif(APPLE)
  target_link_libraries(claw_core PUBLIC "-framework CoreFoundation" "-framework CFNetwork" "-framework Security")
endif()

if(MSVC)
  target_compile_options(claw_core PUBLIC /W3 /utf-8 /EHsc /bigobj)
else()
  target_compile_options(claw_core PRIVATE -Wall -Wextra)
endif()

add_executable(claw src/main.cpp)
target_link_libraries(claw PRIVATE claw_core)

if(CLAW_BUILD_TESTS)
  enable_testing()
  add_executable(claw_tests tests/test_main.cpp)
  target_link_libraries(claw_tests PRIVATE claw_core)
  add_test(NAME claw_tests COMMAND claw_tests)
endif()
