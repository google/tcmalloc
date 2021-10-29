#
# Copyright 2017 The Abseil Authors.
# Copyright 2021 Raffael Casagrande
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include(CMakeParseArguments)
include(cmake/copts.cmake)

# The IDE folder for Tcmalloc that will be used if Tcmalloc is included in a CMake
# project that sets
#    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
# For example, Visual Studio supports folders.
if(NOT DEFINED TCMALLOC_IDE_FOLDER)
  set(TCMALLOC_IDE_FOLDER tcmalloc)
endif()

# tcmalloc_cc_library()
#
# CMake function to imitate Bazel's cc_library rule.
#
# Parameters:
# NAME: name of target (see Note)
# HDRS: List of public header files for the library
# SRCS: List of source files for the library
# DEPS: List of other libraries to be linked in to the binary targets
# COPTS: List of private compile options
# DEFINES: List of public defines
# LINKOPTS: List of link options
# PUBLIC: Add this so that this library will be exported under tcmalloc::
# Also in IDE, target will appear in tcmalloc folder while non PUBLIC will be in tcmalloc/internal.
# TESTONLY: When added, this target will only be built if BUILD_TESTING=ON.
# ALWAYSLINK: Add this so that any binary that depends on this library will link all the object files listed in srcs.
# LINKSTATIC: Link this library always statically (even if BUILD_SHARED_LIBS=ON)
#
# Note:
# By default, tcmalloc_cc_library will always create a library named tcmalloc_${NAME},
# and alias target tcmalloc::${NAME}.  The tcmalloc:: form should always be used.
# This is to reduce namespace pollution.
#
# tcmalloc_cc_library(
#   NAME
#     awesome
#   HDRS
#     "a.h"
#   SRCS
#     "a.cc"
# )
# tcmalloc_cc_library(
#   NAME
#     fantastic_lib
#   SRCS
#     "b.cc"
#   DEPS
#     tcmalloc::awesome # not "awesome" !
#   PUBLIC
# )
#
# tcmalloc_cc_library(
#   NAME
#     main_lib
#   ...
#   DEPS
#     tcmalloc::fantastic_lib
# )
#
function(tcmalloc_cc_library)
  cmake_parse_arguments(TCMALLOC_CC_LIB
    "DISABLE_INSTALL;PUBLIC;TESTONLY;ALWAYSLINK;LINKSTATIC"
    "NAME"
    "HDRS;SRCS;COPTS;DEFINES;LINKOPTS;DEPS"
    ${ARGN}
  )
  
  set(_NAME "${TCMALLOC_CC_LIB_NAME}")

  # Check if this is a header-only library
  set(TCMALLOC_CC_SRCS "${TCMALLOC_CC_LIB_SRCS}")
  list(FILTER "${TCMALLOC_CC_SRCS}" EXCLUDE REGEX ".*\\.(h|inc)")

  if(TCMALLOC_CC_SRCS STREQUAL "")
    set(TCMALLOC_CC_LIB_IS_INTERFACE 1)
  else()
    set(TCMALLOC_CC_LIB_IS_INTERFACE 0)
  endif()

  # Determine this build target's relationship to the DLL. It's one of two things:
  # 1. "shared"  -- This is a shared library, perhaps on a non-windows platform
  #                 where DLL doesn't make sense.
  # 2. "static"  -- This target does not depend on the DLL and should be built
  #                 statically.
  if(BUILD_SHARED_LIBS AND NOT TCMALLOC_CC_LIB_LINKSTATIC)
    set(_build_type "SHARED")
  else()
    set(_build_type "STATIC")
  endif()

  if(NOT TCMALLOC_CC_LIB_IS_INTERFACE)
    if(_build_type STREQUAL "STATIC" OR _build_type STREQUAL "SHARED")
      if(TCMALLOC_CC_LIB_ALWAYSLINK)
        # If ALWAYSLINK is set, we must create a OBJECT library (see cmake manual)
        # but since an object library doesn't support transitive dependencies completly,
        # we create a wrapper INTERFACE target which then links in the OBJECT target
        # See also https://cmake.org/cmake/help/v3.22/command/target_link_libraries.html#linking-object-libraries-via-target-objects
        add_library("${_NAME}_obj" OBJECT ${TCMALLOC_CC_LIB_SRCS} ${TCMALLOC_CC_LIB_HDRS})
        target_link_libraries("${_NAME}_obj" PUBLIC ${TCMALLOC_CC_LIB_DEPS})
        add_library(${_NAME} INTERFACE)
        target_link_libraries(${_NAME} 
          INTERFACE 
            "${_NAME}_obj"
            $<TARGET_OBJECTS:$<TARGET_NAME:${_NAME}_obj>>
        )
        set(_realname ${_NAME}_obj)
        
        install(TARGETS ${_NAME}_obj EXPORT ${PROJECT_NAME}Targets
          OBJECTS DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
      else()
        add_library(${_NAME} ${_build_type} "")
        target_sources(${_NAME} PRIVATE ${TCMALLOC_CC_LIB_SRCS} ${TCMALLOC_CC_LIB_HDRS})
        target_link_libraries(${_NAME}
          PUBLIC  ${TCMALLOC_CC_LIB_DEPS}
          PRIVATE ${TCMALLOC_CC_LIB_LINKOPTS}
        )
        set(_realname ${_NAME})
      endif()
    else()
      message(FATAL_ERROR "Invalid build type: ${_build_type}")
    endif()

    # Linker language can be inferred from sources, but in the case of DLLs we
    # don't have any .cc files so it would be ambiguous. We could set it
    # explicitly only in the case of DLLs but, because "CXX" is always the
    # correct linker language for static or for shared libraries, we set it
    # unconditionally.
    set_property(TARGET ${_realname} PROPERTY LINKER_LANGUAGE "CXX")

    target_include_directories(${_realname}
      PUBLIC
        "$<BUILD_INTERFACE:${TCMALLOC_COMMON_INCLUDE_DIRS}>"
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    
    target_compile_options(${_realname}
      PRIVATE ${TCMALLOC_DEFAULT_COPTS} ${TCMALLOC_CC_LIB_COPTS})
    target_compile_definitions(${_realname} PUBLIC ${TCMALLOC_CC_LIB_DEFINES})

    # Add all Tcmalloc targets to a a folder in the IDE for organization.
    if(TCMALLOC_CC_LIB_PUBLIC)
      set_property(TARGET ${_realname} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER})
    elseif(TCMALLOC_CC_LIB_TESTONLY)
      set_property(TARGET ${_realname} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER}/test)
    else()
      set_property(TARGET ${_realname} PROPERTY FOLDER ${TCMALLOC_IDE_FOLDER}/internal)
    endif()

    # INTERFACE libraries can't have the CXX_STANDARD property set
    set_property(TARGET ${_realname} PROPERTY CXX_STANDARD ${TCMALLOC_CXX_STANDARD})
    set_property(TARGET ${_realname} PROPERTY CXX_STANDARD_REQUIRED ON)

    
    set_target_properties(${_NAME} PROPERTIES
      OUTPUT_NAME "tcmalloc_${_NAME}"
    )
  else()
    # Generating header-only library
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
  
  install(TARGETS ${_NAME} EXPORT ${PROJECT_NAME}Targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
  
  install(FILES ${TCMALLOC_CC_LIB_HDRS} DESTINATION "include/tcmalloc")

  add_library(tcmalloc::${TCMALLOC_CC_LIB_NAME} ALIAS ${_NAME})
endfunction()