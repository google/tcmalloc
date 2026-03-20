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

function(tcmalloc_cc_library)
  cmake_parse_arguments(TCMALLOC "" "NAME;ALIAS" "SRCS;HDRS;COPTS;LINKOPTS;DEPS" ${ARGN})
  if(TCMALLOC_SRCS)
    if(TCMALLOC_NAME MATCHES ".*_main$")
      add_library(${TCMALLOC_NAME} OBJECT "")
    else()
      add_library(${TCMALLOC_NAME} STATIC "")
    endif()
    set_target_properties(${TCMALLOC_NAME} PROPERTIES LINKER_LANGUAGE CXX)
    target_sources(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_SRCS})
    if(TCMALLOC_HDRS)
      target_sources(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_HDRS})
    endif()
    if(TCMALLOC_COPTS)
      target_compile_options(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_COPTS})
    endif()
    if(TCMALLOC_LINKOPTS)
      target_link_options(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_LINKOPTS})
    endif()
    if(TCMALLOC_DEPS)
      target_link_libraries(${TCMALLOC_NAME} PUBLIC ${TCMALLOC_DEPS})
    endif()
    target_include_directories(${TCMALLOC_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
  else()
    add_library(${TCMALLOC_NAME} INTERFACE)
    if(TCMALLOC_HDRS)
      target_sources(${TCMALLOC_NAME} INTERFACE ${TCMALLOC_HDRS})
    endif()
    if(TCMALLOC_COPTS)
      target_compile_options(${TCMALLOC_NAME} INTERFACE ${TCMALLOC_COPTS})
    endif()
    if(TCMALLOC_LINKOPTS)
      target_link_options(${TCMALLOC_NAME} INTERFACE ${TCMALLOC_LINKOPTS})
    endif()
    if(TCMALLOC_DEPS)
      target_link_libraries(${TCMALLOC_NAME} INTERFACE ${TCMALLOC_DEPS})
    endif()
    target_include_directories(${TCMALLOC_NAME} INTERFACE ${CMAKE_SOURCE_DIR})
  endif()
  if(TCMALLOC_ALIAS)
    add_library(${TCMALLOC_ALIAS} ALIAS ${TCMALLOC_NAME})
  endif()
endfunction()

function(tcmalloc_cc_test)
  if(NOT (BUILD_TESTING AND TCMALLOC_BUILD_TESTING))
    return()
  endif()

  cmake_parse_arguments(TCMALLOC "" "NAME;ALIAS" "SRCS;HDRS;COPTS;LINKOPTS;DEPS" ${ARGN})
  add_executable(${TCMALLOC_NAME} "")
  if(TCMALLOC_SRCS)
    target_sources(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_SRCS})
  endif()
  if(TCMALLOC_HDRS)
    target_sources(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_HDRS})
  endif()
  if(TCMALLOC_COPTS)
    target_compile_options(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_COPTS})
  endif()
  if(TCMALLOC_LINKOPTS)
    target_link_options(${TCMALLOC_NAME} PRIVATE ${TCMALLOC_LINKOPTS})
  endif()
  if(TCMALLOC_DEPS)
    target_link_libraries(${TCMALLOC_NAME} PUBLIC ${TCMALLOC_DEPS})
  endif()
  target_include_directories(${TCMALLOC_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
  add_test(NAME ${TCMALLOC_NAME} COMMAND ${TCMALLOC_NAME})
  set_tests_properties(${TCMALLOC_NAME} PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR};TEST_SRCDIR=${CMAKE_SOURCE_DIR}")
endfunction()

function(tcmalloc_cc_binary)
  if(NOT (BUILD_TESTING AND TCMALLOC_BUILD_TESTING))
    return()
  endif()

  tcmalloc_cc_test(${ARGN})
endfunction()
