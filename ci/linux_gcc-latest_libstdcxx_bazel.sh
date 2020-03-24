#!/bin/bash
#
# Copyright 2019 The TCMalloc Authors.
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

# This script that can be invoked to test tcmalloc in a hermetic environment
# using a Docker image on Linux. You must have Docker installed to use this
# script.

set -euox pipefail

if [ -z ${TCMALLOC_ROOT:-} ]; then
  TCMALLOC_ROOT="$(realpath $(dirname ${0})/..)"
fi

if [ -z ${STD:-} ]; then
  STD="c++17"
fi

if [ -z ${COMPILATION_MODE:-} ]; then
  COMPILATION_MODE="fastbuild opt"
fi

if [ -z ${EXCEPTIONS_MODE:-} ]; then
  EXCEPTIONS_MODE="-fno-exceptions -fexceptions"
fi

readonly DOCKER_CONTAINER="gcr.io/google.com/absl-177019/linux_gcc-latest:20200319"

for std in ${STD}; do
  for compilation_mode in ${COMPILATION_MODE}; do
    for exceptions_mode in ${EXCEPTIONS_MODE}; do
      echo "--------------------------------------------------------------------"
      time docker run \
        --volume="${TCMALLOC_ROOT}:/tcmalloc:ro" \
        --workdir=/tcmalloc \
        --cap-add=SYS_PTRACE \
        --rm \
        -e CC="/usr/local/bin/gcc" \
        -e BAZEL_CXXOPTS="-std=${std}" \
        ${DOCKER_EXTRA_ARGS:-} \
        ${DOCKER_CONTAINER} \
        /usr/local/bin/bazel test ... \
          --compilation_mode="${compilation_mode}" \
          --copt="${exceptions_mode}" \
          --copt=-Werror \
          --define="absl=1" \
          --keep_going \
          --show_timestamps \
          --test_env="GTEST_INSTALL_FAILURE_SIGNAL_HANDLER=1" \
          --test_output=errors \
          --test_tag_filters=-benchmark \
          ${BAZEL_EXTRA_ARGS:-}
    done
  done
done
