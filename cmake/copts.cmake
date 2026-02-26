#
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

# Determine basic compile options:
  set(TCMALLOC_LLVM_FLAGS -Wno-deprecated-declarations
    -Wno-implicit-int-float-conversion
    -Wno-sign-compare
    -Wno-uninitialized
    -Wno-unused-function
    -Wno-unused-variable)
    
  set(TCMALLOC_GCC_FLAGS -Wno-attribute-alias
      -Wno-sign-compare
      -Wno-uninitialized
      -Wno-unused-function
      # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66425
      -Wno-unused-result
      -Wno-unused-variable)
    
  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(TCMALLOC_DEFAULT_COPTS ${TCMALLOC_LLVM_FLAGS})
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(TCMALLOC_DEFAULT_COPTS ${TCMALLOC_GCC_FLAGS})
  endif()