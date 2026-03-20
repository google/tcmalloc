# Copyright 2026 The TCMalloc Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.24)
project(tcmalloc CXX C ASM)

set(TCMALLOC_SOVERSION 0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

set(CMAKE_SKIP_INSTALL_RULES ON)
set(ABSL_PROPAGATE_CXX_STD ON)
set(protobuf_ABSL_PROVIDER "package" CACHE STRING "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

include(CTest)

option(TCMALLOC_BUILD_TESTING
  "If ON, TCMalloc will build all of its own tests." OFF)

option(TCMALLOC_BUILD_TEST_HELPERS
  "If ON, TCMalloc will build test helper libraries." OFF)

set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TEST_HELPERS ON CACHE BOOL "" FORCE)
set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "" FORCE)
set(ABSL_FIND_GOOGLETEST OFF CACHE BOOL "" FORCE)

include(FetchContent)

##
## Dependency resolution options
##
## Each dependency supports a layered resolution strategy inspired by
## abseil-cpp's ABSL_USE_EXTERNAL_GOOGLETEST / ABSL_FIND_GOOGLETEST
## pattern.  For offline or hermetic builds, set the appropriate
## TCMALLOC_USE_EXTERNAL_* or TCMALLOC_LOCAL_*_DIR options so that
## no network fetch is required.
##

# -- abseil-cpp --
option(TCMALLOC_USE_EXTERNAL_ABSEIL
  "If ON, assume abseil-cpp targets (absl::*) are already provided by the parent project." OFF)
set(TCMALLOC_LOCAL_ABSEIL_DIR "" CACHE PATH
  "Path to a local abseil-cpp source directory. Used when TCMALLOC_USE_EXTERNAL_ABSEIL is OFF.")

if(TCMALLOC_USE_EXTERNAL_ABSEIL)
  if(NOT TARGET absl::base)
    find_package(absl REQUIRED)
  endif()
else()
  if(TCMALLOC_LOCAL_ABSEIL_DIR)
    add_subdirectory("${TCMALLOC_LOCAL_ABSEIL_DIR}" abseil-cpp EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(
      abseil-cpp
      GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
      GIT_TAG        master
      FIND_PACKAGE_ARGS NAMES absl
    )
    FetchContent_MakeAvailable(abseil-cpp)
  endif()
endif()

# -- Test-only dependencies --
if((BUILD_TESTING AND TCMALLOC_BUILD_TESTING) OR TCMALLOC_BUILD_TEST_HELPERS)

  # GoogleTest
  option(TCMALLOC_USE_EXTERNAL_GOOGLETEST
    "If ON, assume GoogleTest targets (GTest::*) are already provided." OFF)
  set(TCMALLOC_LOCAL_GOOGLETEST_DIR "" CACHE PATH
    "Path to a local GoogleTest source directory.")

  if(TCMALLOC_USE_EXTERNAL_GOOGLETEST)
    if(NOT TARGET GTest::gtest)
      find_package(GTest REQUIRED)
    endif()
  else()
    if(TCMALLOC_LOCAL_GOOGLETEST_DIR)
      add_subdirectory("${TCMALLOC_LOCAL_GOOGLETEST_DIR}" googletest EXCLUDE_FROM_ALL)
      if(TARGET gtest AND NOT TARGET GTest::gtest)
        add_library(GTest::gtest ALIAS gtest)
        add_library(GTest::gtest_main ALIAS gtest_main)
        add_library(GTest::gmock ALIAS gmock)
        add_library(GTest::gmock_main ALIAS gmock_main)
      endif()
    else()
      FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        main
        FIND_PACKAGE_ARGS NAMES GTest
      )
      FetchContent_MakeAvailable(googletest)
    endif()
  endif()

  # Protobuf
  option(TCMALLOC_USE_EXTERNAL_PROTOBUF
    "If ON, assume Protobuf targets are already provided." OFF)
  set(TCMALLOC_LOCAL_PROTOBUF_DIR "" CACHE PATH
    "Path to a local protobuf source directory.")

  if(TCMALLOC_USE_EXTERNAL_PROTOBUF)
    if(NOT TARGET protobuf::libprotobuf)
      find_package(Protobuf REQUIRED)
    endif()
  else()
    if(TCMALLOC_LOCAL_PROTOBUF_DIR)
      add_subdirectory("${TCMALLOC_LOCAL_PROTOBUF_DIR}" protobuf EXCLUDE_FROM_ALL)
    else()
      FetchContent_Declare(
        protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG        main
        FIND_PACKAGE_ARGS NAMES Protobuf
      )
      FetchContent_MakeAvailable(protobuf)
    endif()
  endif()

  # Google Benchmark
  option(TCMALLOC_USE_EXTERNAL_BENCHMARK
    "If ON, assume Google Benchmark targets are already provided." OFF)
  set(TCMALLOC_LOCAL_BENCHMARK_DIR "" CACHE PATH
    "Path to a local Google Benchmark source directory.")

  if(TCMALLOC_USE_EXTERNAL_BENCHMARK)
    if(NOT TARGET benchmark::benchmark)
      find_package(benchmark REQUIRED)
    endif()
  else()
    if(TCMALLOC_LOCAL_BENCHMARK_DIR)
      add_subdirectory("${TCMALLOC_LOCAL_BENCHMARK_DIR}" benchmark EXCLUDE_FROM_ALL)
    else()
      FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        main
        FIND_PACKAGE_ARGS NAMES benchmark
      )
      FetchContent_MakeAvailable(benchmark)
    endif()
  endif()

  # FuzzTest
  option(TCMALLOC_USE_EXTERNAL_FUZZTEST
    "If ON, assume FuzzTest targets are already provided." OFF)
  set(TCMALLOC_LOCAL_FUZZTEST_DIR "" CACHE PATH
    "Path to a local FuzzTest source directory.")

  if(TCMALLOC_USE_EXTERNAL_FUZZTEST)
    if(NOT TARGET fuzztest::fuzztest)
      find_package(fuzztest REQUIRED)
    endif()
  else()
    if(TCMALLOC_LOCAL_FUZZTEST_DIR)
      add_subdirectory("${TCMALLOC_LOCAL_FUZZTEST_DIR}" fuzztest EXCLUDE_FROM_ALL)
    else()
      FetchContent_Declare(
        fuzztest
        GIT_REPOSITORY https://github.com/google/fuzztest.git
        GIT_TAG        main
        FIND_PACKAGE_ARGS NAMES fuzztest
      )
      FetchContent_MakeAvailable(fuzztest)
    endif()
  endif()

endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/CMake)
include(TcmallocHelpers)
include(TcmallocVariants)

add_subdirectory(tcmalloc/internal)
add_subdirectory(tcmalloc/testing)
add_subdirectory(tcmalloc)
