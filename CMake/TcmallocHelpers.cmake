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

include(CMakeParseArguments)

# include current path
list(APPEND TCMALLOC_COMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})

# The IDE folder for TCMalloc that will be used if TCMalloc is included in a
# CMake project that sets
#    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
if(NOT DEFINED TCMALLOC_IDE_FOLDER)
  set(TCMALLOC_IDE_FOLDER TCMalloc)
endif()

# tcmalloc_cc_library()
#
# CMake function to imitate Bazel's cc_library rule, analogous to
# abseil-cpp's absl_cc_library().
#
# Parameters:
# NAME: name of target
# ALIAS: alias target name (e.g. tcmalloc::foo)
# HDRS: List of public header files for the library
# SRCS: List of source files for the library
# DEPS: List of other libraries to be linked in
# COPTS: List of private compile options
# DEFINES: List of public defines
# LINKOPTS: List of link options
# PUBLIC: Mark this as a public API target (affects IDE folder)
# TESTONLY: Only build when BUILD_TESTING AND TCMALLOC_BUILD_TESTING
# DISABLE_INSTALL: Skip installation rules for this target
#
function(tcmalloc_cc_library)
  cmake_parse_arguments(TCMALLOC_CC_LIB
    "DISABLE_INSTALL;PUBLIC;TESTONLY"
    "NAME;ALIAS"
    "SRCS;HDRS;COPTS;DEFINES;LINKOPTS;DEPS"
    ${ARGN}
  )

  if(TCMALLOC_CC_LIB_TESTONLY AND
      NOT ((BUILD_TESTING AND TCMALLOC_BUILD_TESTING) OR
        (TCMALLOC_BUILD_TEST_HELPERS AND TCMALLOC_CC_LIB_PUBLIC)))
    return()
  endif()

  set(_NAME "${TCMALLOC_CC_LIB_NAME}")

  # Check if this is a header-only library by stripping .h/.inc files from SRCS
  set(TCMALLOC_CC_SRCS "${TCMALLOC_CC_LIB_SRCS}")
  foreach(src_file IN LISTS TCMALLOC_CC_SRCS)
    if(${src_file} MATCHES ".*\\.(h|inc)")
      list(REMOVE_ITEM TCMALLOC_CC_SRCS "${src_file}")
    endif()
  endforeach()

  if(TCMALLOC_CC_SRCS STREQUAL "")
    set(TCMALLOC_CC_LIB_IS_INTERFACE 1)
  else()
    set(TCMALLOC_CC_LIB_IS_INTERFACE 0)
  endif()

  if(NOT TCMALLOC_CC_LIB_IS_INTERFACE)
    if(_NAME MATCHES ".*_main$")
      add_library(${_NAME} OBJECT "")
    else()
      add_library(${_NAME} "")
    endif()

    set_property(TARGET ${_NAME} PROPERTY LINKER_LANGUAGE "CXX")

    target_sources(${_NAME} PRIVATE ${TCMALLOC_CC_LIB_SRCS} ${TCMALLOC_CC_LIB_HDRS})

    target_include_directories(${_NAME}
      PUBLIC
        "$<BUILD_INTERFACE:${TCMALLOC_COMMON_INCLUDE_DIRS}>"
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_compile_options(${_NAME}
      PRIVATE ${TCMALLOC_CC_LIB_COPTS})
    target_compile_definitions(${_NAME} PUBLIC ${TCMALLOC_CC_LIB_DEFINES})

    target_link_libraries(${_NAME}
      PUBLIC ${TCMALLOC_CC_LIB_DEPS}
      PRIVATE
        ${TCMALLOC_CC_LIB_LINKOPTS}
    )

    if(APPLE)
      set_target_properties(${_NAME} PROPERTIES
        INSTALL_RPATH "@loader_path")
    elseif(UNIX)
      set_target_properties(${_NAME} PROPERTIES
        INSTALL_RPATH "$ORIGIN")
    endif()

    # IDE folder organization
    if(TCMALLOC_CC_LIB_PUBLIC)
      set_property(TARGET ${_NAME} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER})
    elseif(TCMALLOC_CC_LIB_TESTONLY)
      set_property(TARGET ${_NAME} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER}/test)
    else()
      set_property(TARGET ${_NAME} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER}/internal)
    endif()

    # When being installed, set SOVERSION for shared library versioning.
    if(TCMALLOC_ENABLE_INSTALL)
      set_target_properties(${_NAME} PROPERTIES
        SOVERSION "${TCMALLOC_SOVERSION}"
      )
    endif()
  else()
    # Header-only (interface) library
    add_library(${_NAME} INTERFACE)
    target_include_directories(${_NAME}
      INTERFACE
        "$<BUILD_INTERFACE:${TCMALLOC_COMMON_INCLUDE_DIRS}>"
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(${_NAME}
      INTERFACE
        ${TCMALLOC_CC_LIB_DEPS}
        ${TCMALLOC_CC_LIB_LINKOPTS}
    )
    target_compile_definitions(${_NAME} INTERFACE ${TCMALLOC_CC_LIB_DEFINES})
  endif()

  if(TCMALLOC_ENABLE_INSTALL)
    install(TARGETS ${_NAME} EXPORT ${PROJECT_NAME}Targets
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
  endif()

  if(TCMALLOC_CC_LIB_ALIAS)
    add_library(${TCMALLOC_CC_LIB_ALIAS} ALIAS ${_NAME})
  endif()
endfunction()

# tcmalloc_cc_test()
#
# CMake function to imitate Bazel's cc_test rule, analogous to
# abseil-cpp's absl_cc_test().
#
# Parameters:
# NAME: name of target (creates executable with this name)
# SRCS: List of source files for the binary
# DEPS: List of other libraries to be linked in
# COPTS: List of private compile options
# DEFINES: List of public defines
# LINKOPTS: List of link options
#
function(tcmalloc_cc_test)
  if(NOT (BUILD_TESTING AND TCMALLOC_BUILD_TESTING))
    return()
  endif()

  cmake_parse_arguments(TCMALLOC_CC_TEST
    ""
    "NAME;ALIAS"
    "SRCS;HDRS;COPTS;DEFINES;LINKOPTS;DEPS"
    ${ARGN}
  )

  set(_NAME "${TCMALLOC_CC_TEST_NAME}")

  add_executable(${_NAME} "")
  if(TCMALLOC_CC_TEST_SRCS)
    target_sources(${_NAME} PRIVATE ${TCMALLOC_CC_TEST_SRCS})
  endif()
  if(TCMALLOC_CC_TEST_HDRS)
    target_sources(${_NAME} PRIVATE ${TCMALLOC_CC_TEST_HDRS})
  endif()

  target_compile_options(${_NAME}
    PRIVATE ${TCMALLOC_CC_TEST_COPTS})
  target_compile_definitions(${_NAME}
    PUBLIC ${TCMALLOC_CC_TEST_DEFINES})

  target_link_libraries(${_NAME}
    PUBLIC ${TCMALLOC_CC_TEST_DEPS}
    PRIVATE ${TCMALLOC_CC_TEST_LINKOPTS}
  )

  target_include_directories(${_NAME}
    PUBLIC "$<BUILD_INTERFACE:${TCMALLOC_COMMON_INCLUDE_DIRS}>")

  # IDE folder organization
  set_property(TARGET ${_NAME} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER}/test)

  add_test(NAME ${_NAME} COMMAND ${_NAME})
  set_tests_properties(${_NAME} PROPERTIES
    ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR};TEST_SRCDIR=${CMAKE_SOURCE_DIR}")
endfunction()

function(tcmalloc_cc_binary)
  if(NOT (BUILD_TESTING AND TCMALLOC_BUILD_TESTING))
    return()
  endif()

  tcmalloc_cc_test(${ARGN})
endfunction()
