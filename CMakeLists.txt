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

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_SKIP_INSTALL_RULES ON)
set(ABSL_PROPAGATE_CXX_STD ON)
set(protobuf_INSTALL OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF)
set(ANTLR_BUILD_CPP_TESTS OFF)

set(ABSL_BUILD_TEST_HELPERS ON)
set(ABSL_USE_EXTERNAL_GOOGLETEST ON)
set(ABSL_FIND_GOOGLETEST OFF)

enable_testing()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  abseil-cpp
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(abseil-cpp)

FetchContent_Declare(
  protobuf
  GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(protobuf)

FetchContent_Declare(
  re2
  GIT_REPOSITORY https://github.com/google/re2.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(re2)

target_compile_definitions(gtest PUBLIC GTEST_HAS_ABSL=1)
target_link_libraries(gtest PUBLIC
  absl::failure_signal_handler
  absl::stacktrace
  absl::symbolize
  absl::flags_parse
  absl::flags_reflection
  absl::flags_usage
  absl::strings
  re2::re2
)

FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(benchmark)

FetchContent_Declare(
  fuzztest
  GIT_REPOSITORY https://github.com/google/fuzztest.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(fuzztest)

include(tcmalloc/tcmalloc_helpers.cmake)
include(tcmalloc/tcmalloc_variants.cmake)

add_subdirectory(tcmalloc/internal)
add_subdirectory(tcmalloc/testing)
add_subdirectory(tcmalloc)
