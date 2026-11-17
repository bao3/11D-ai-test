cmake_minimum_required(VERSION 3.16)

# On Linux images /usr/bin/c++ can be Clang without a libstdc++ link line, so
# default to g++. On macOS, default to AppleClang (clang++) with libc++.
if(NOT DEFINED CMAKE_CXX_COMPILER)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_CXX_COMPILER clang++ CACHE STRING "C++ compiler" FORCE)
    else()
        set(CMAKE_CXX_COMPILER g++ CACHE STRING "C++ compiler" FORCE)
    endif()
endif()

project(11dim-ai-test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(CheckCXXCompilerFlag)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Release CI passes -DPS5AI_STATIC_CXX=ON on Linux. Shared libcurl stays the default;
# a fully static curl pulls OpenSSL and friends and is not required for v1.1.
option(PS5AI_STATIC_CXX "Link libstdc++ and libgcc statically (Linux GCC)" OFF)
option(PS5AI_PREFER_STATIC_CURL "Prefer static libcurl if available (best-effort; v1.1 still uses shared curl)" OFF)

add_executable(11dim-ai-test
    src/main.cpp
    src/scoring.cpp
    src/long_context.cpp
    src/long_context_data.cpp
    src/prompt_data.cpp
    src/paths.cpp
    src/sandbox.cpp
    src/http.cpp
    src/perf.cpp
    src/report.cpp
    src/selftest.cpp
)

target_include_directories(11dim-ai-test PRIVATE src)
target_include_directories(11dim-ai-test SYSTEM PRIVATE third_party)
target_compile_definitions(11dim-ai-test PRIVATE PS5AI_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
target_compile_options(11dim-ai-test PRIVATE -Wall -Wextra -Wpedantic)

check_cxx_compiler_flag("-finput-charset=UTF-8" PS5AI_HAS_FINPUT_UTF8)
if(PS5AI_HAS_FINPUT_UTF8)
    target_compile_options(11dim-ai-test PRIVATE -finput-charset=UTF-8)
endif()
check_cxx_compiler_flag("-fexec-charset=UTF-8" PS5AI_HAS_FEXEC_UTF8)
if(PS5AI_HAS_FEXEC_UTF8)
    target_compile_options(11dim-ai-test PRIVATE -fexec-charset=UTF-8)
endif()

target_link_libraries(11dim-ai-test PRIVATE CURL::libcurl Threads::Threads)

if(PS5AI_STATIC_CXX)
  if(APPLE)
    message(STATUS "PS5AI_STATIC_CXX=ON ignored on macOS (links system libc++).")
  else()
    target_link_options(11dim-ai-test PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

if(PS5AI_PREFER_STATIC_CURL)
  message(STATUS "PS5AI_PREFER_STATIC_CURL=ON: shared CURL::libcurl remains linked. Fully static curl is not required for the v1.1 bundle.")
endif()

enable_testing()
add_test(NAME selftest COMMAND 11dim-ai-test --self-test)
