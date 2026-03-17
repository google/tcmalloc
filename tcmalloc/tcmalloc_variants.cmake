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

# CMake variant helper for TCMalloc
function(tcmalloc_cc_library_variants)
  cmake_parse_arguments(TCMALLOC "" "NAME;ALIAS" "SRCS;HDRS;COPTS;LINKOPTS;DEPS" ${ARGN})
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_8k_pages
    ALIAS ${TCMALLOC_ALIAS}_8k_pages
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_8K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_deprecated_perthread
    ALIAS ${TCMALLOC_ALIAS}_deprecated_perthread
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_8K_PAGES -DTCMALLOC_DEPRECATED_PERTHREAD
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_large_pages
    ALIAS ${TCMALLOC_ALIAS}_large_pages
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_32K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_256k_pages
    ALIAS ${TCMALLOC_ALIAS}_256k_pages
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_small_but_slow
    ALIAS ${TCMALLOC_ALIAS}_small_but_slow
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_SMALL_BUT_SLOW
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_small_but_slow_with_assertions
    ALIAS ${TCMALLOC_ALIAS}_small_but_slow_with_assertions
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_SMALL_BUT_SLOW -DTCMALLOC_INTERNAL_WITH_ASSERTIONS
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_numa_aware
    ALIAS ${TCMALLOC_ALIAS}_numa_aware
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_8K_PAGES -DTCMALLOC_INTERNAL_NUMA_AWARE
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_256k_pages_numa_aware
    ALIAS ${TCMALLOC_ALIAS}_256k_pages_numa_aware
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES -DTCMALLOC_INTERNAL_NUMA_AWARE
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
  tcmalloc_cc_library(NAME ${TCMALLOC_NAME}_legacy_locking
    ALIAS ${TCMALLOC_ALIAS}_legacy_locking
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_8K_PAGES -DTCMALLOC_INTERNAL_LEGACY_LOCKING
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS}
  )
endfunction()

function(tcmalloc_cc_test_variants)
  cmake_parse_arguments(TCMALLOC "" "NAME;ALIAS" "SRCS;HDRS;COPTS;LINKOPTS;DEPS" ${ARGN})
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_8k_pages
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_8k_pages PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_32k_pages
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_32K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_large_pages tcmalloc::common_large_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_32k_pages PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_256k_pages
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_256k_pages tcmalloc::common_256k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_256k_pages PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_small_but_slow
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_SMALL_BUT_SLOW
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_small_but_slow tcmalloc::common_small_but_slow
  )
  set_tests_properties(${TCMALLOC_NAME}_small_but_slow PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_small_but_slow_with_assertions
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_SMALL_BUT_SLOW
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_small_but_slow_with_assertions tcmalloc::common_small_but_slow_with_assertions
  )
  set_tests_properties(${TCMALLOC_NAME}_small_but_slow_with_assertions PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_256k_pages_pow2
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_256k_pages tcmalloc::common_256k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_256k_pages_pow2 PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TEST_ONLY_TCMALLOC_POW2_SIZECLASS;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_256k_pages_sharded_transfer_cache
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_256k_pages tcmalloc::common_256k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_256k_pages_sharded_transfer_cache PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_numa_aware
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_NUMA_AWARE
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_numa_aware tcmalloc::common_numa_aware tcmalloc::want_numa_aware
  )
  set_tests_properties(${TCMALLOC_NAME}_numa_aware PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_numa_aware_enabled_runtime
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_NUMA_AWARE
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_numa_aware tcmalloc::common_numa_aware
  )
  set_tests_properties(${TCMALLOC_NAME}_numa_aware_enabled_runtime PROPERTIES ENVIRONMENT "TCMALLOC_NUMA_AWARE=1;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_partitioned_enabled_runtime
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_partitioned_enabled_runtime PROPERTIES ENVIRONMENT "TCMALLOC_HEAP_PARTITIONING=true;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_numa_aware_disabled
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_NUMA_AWARE
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_numa_aware tcmalloc::common_numa_aware tcmalloc::want_numa_aware
  )
  set_tests_properties(${TCMALLOC_NAME}_numa_aware_disabled PROPERTIES ENVIRONMENT "TCMALLOC_NUMA_AWARE=0;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_256k_pages_numa_aware
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES -DTCMALLOC_INTERNAL_NUMA_AWARE
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_256k_pages_numa_aware tcmalloc::common_256k_pages_numa_aware tcmalloc::want_numa_aware
  )
  set_tests_properties(${TCMALLOC_NAME}_256k_pages_numa_aware PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_256k_pages_pow2_sharded_transfer_cache
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_256K_PAGES
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_256k_pages tcmalloc::common_256k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_256k_pages_pow2_sharded_transfer_cache PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TEST_ONLY_TCMALLOC_POW2_SIZECLASS,TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_legacy_size_classes
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages tcmalloc::want_legacy_size_classes
  )
  set_tests_properties(${TCMALLOC_NAME}_legacy_size_classes PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_huge_cache_release_30s
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_huge_cache_release_30s PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TEST_ONLY_TCMALLOC_HUGE_CACHE_RELEASE_30S;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_hpaa
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages tcmalloc::want_hpaa
  )
  set_tests_properties(${TCMALLOC_NAME}_hpaa PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_deprecated_perthread
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_DEPRECATED_PERTHREAD
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_deprecated_perthread tcmalloc::common_deprecated_perthread
  )
  set_tests_properties(${TCMALLOC_NAME}_deprecated_perthread PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_pgho_experiment
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_pgho_experiment PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TCMALLOC_PGHO_EXPERIMENT;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_flat_cpu_caches
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_flat_cpu_caches PROPERTIES ENVIRONMENT "PERCPU_VCPU_MODE=flat;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_real_cpu_caches
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_real_cpu_caches PROPERTIES ENVIRONMENT "PERCPU_VCPU_MODE=none;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_no_glibc_rseq
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_no_glibc_rseq PROPERTIES ENVIRONMENT "GLIBC_TUNABLES=glibc.pthread.rseq=0;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_mm_vcpu_cpu_caches
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_mm_vcpu_cpu_caches PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TEST_ONLY_MM_VCPU;GLIBC_TUNABLES=glibc.pthread.rseq=0;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_legacy_locking
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS} -DTCMALLOC_INTERNAL_8K_PAGES -DTCMALLOC_INTERNAL_LEGACY_LOCKING
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc_legacy_locking tcmalloc::common_legacy_locking
  )
  set_tests_properties(${TCMALLOC_NAME}_legacy_locking PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_tcmalloc_span_lifetime_tracking
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_tcmalloc_span_lifetime_tracking PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TEST_ONLY_TCMALLOC_SPAN_LIFETIME_TRACKING;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
  tcmalloc_cc_test(NAME ${TCMALLOC_NAME}_tcmalloc_eager_backing
    SRCS ${TCMALLOC_SRCS}
    HDRS ${TCMALLOC_HDRS}
    COPTS ${TCMALLOC_COPTS}
    LINKOPTS ${TCMALLOC_LINKOPTS}
    DEPS ${TCMALLOC_DEPS} tcmalloc::tcmalloc tcmalloc::common_8k_pages
  )
  set_tests_properties(${TCMALLOC_NAME}_tcmalloc_eager_backing PROPERTIES ENVIRONMENT "BORG_EXPERIMENTS=TCMALLOC_EAGER_BACKING;TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

function(tcmalloc_cc_binary_variants)
  tcmalloc_cc_test_variants(${ARGN})
endfunction()
