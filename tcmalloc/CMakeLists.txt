add_subdirectory(internal)

tcmalloc_cc_library(
  NAME "experiment"
  SRCS "experiment.cc"
  HDRS 
        "experiment.h"
        "experiment_config.h"
  DEPS 
        "tcmalloc::malloc_extension"
        "tcmalloc::environment"
        "tcmalloc::logging"
        "absl::core_headers"
        "absl::strings"
        "absl::optional"
)

# Dependencies required by :tcmalloc and its variants.  Since :common is built
# several different ways it should not be included on this list.
set(tcmalloc_deps 
    "tcmalloc::experiment"
    "tcmalloc::malloc_extension"
    "tcmalloc::new_extension"
    "absl::base"
    "absl::config"
    "absl::core_headers"
    "absl::dynamic_annotations"
    "absl::leak_check"
    "absl::stacktrace"
    "absl::symbolize"
    "absl::memory"
    "absl::strings"
    "absl::bits"
    "tcmalloc::config"
    "tcmalloc::declarations"
    "tcmalloc::linked_list"
    "tcmalloc::logging"
    "tcmalloc::memory_stats"
    "tcmalloc::optimization"
    "tcmalloc::percpu"
)

# This library provides tcmalloc always
tcmalloc_cc_library(
 NAME "tcmalloc"
 SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
 LINKSTATIC
 PUBLIC
 DEPS ${tcmalloc_deps} 
        "tcmalloc::common"
    
  ALWAYSLINK
)

# Provides tcmalloc always; use per-thread mode.
tcmalloc_cc_library(
  NAME "tcmalloc_deprecated_perthread"
   SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
    
  COPTS "-DTCMALLOC_DEPRECATED_PERTHREAD"
  LINKSTATIC
  DEPS ${tcmalloc_deps} 
        "tcmalloc::common_deprecated_perthread"
    
  ALWAYSLINK
)

# An opt tcmalloc build with ASSERTs forced on (by turning off
# NDEBUG). Useful for tracking down crashes in production binaries.
# To use add malloc "//tcmalloc:opt_with_assertions" in your
# target's build rule.
tcmalloc_cc_library(
  NAME "opt_with_assertions"
  SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
  PUBLIC
  COPTS 
        "-O2"
        "-UNDEBUG"
  LINKSTATIC
  DEPS ${tcmalloc_deps}  
        "tcmalloc::common"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "size_class_info"
  HDRS "size_class_info.h"
  DEPS 
        "tcmalloc::logging"
)

# List of common source files used by the various tcmalloc libraries.
set(common_srcs 
    "arena.cc"
    "arena.h"
    "background.cc"
    "central_freelist.cc"
    "central_freelist.h"
    "common.cc"
    "common.h"
    "cpu_cache.cc"
    "cpu_cache.h"
    "experimental_pow2_below64_size_class.cc"
    "experimental_pow2_size_class.cc"
    "legacy_size_classes.cc"
    "guarded_page_allocator.h"
    "guarded_page_allocator.cc"
    "huge_address_map.cc"
    "huge_allocator.cc"
    "huge_allocator.h"
    "huge_cache.cc"
    "huge_cache.h"
    "huge_region.h"
    "huge_page_aware_allocator.cc"
    "huge_page_aware_allocator.h"
    "huge_page_filler.h"
    "huge_pages.h"
    "page_allocator.cc"
    "page_allocator.h"
    "page_allocator_interface.cc"
    "page_allocator_interface.h"
    "page_heap.cc"
    "page_heap.h"
    "page_heap_allocator.h"
    "pagemap.cc"
    "pagemap.h"
    "parameters.cc"
    "peak_heap_tracker.cc"
    "sampler.cc"
    "sampler.h"
    "size_classes.cc"
    "span.cc"
    "span.h"
    "span_stats.h"
    "stack_trace_table.cc"
    "stack_trace_table.h"
    "static_vars.cc"
    "static_vars.h"
    "stats.cc"
    "system-alloc.cc"
    "system-alloc.h"
    "thread_cache.cc"
    "thread_cache.h"
    "tracking.h"
    "transfer_cache_stats.h"
    "transfer_cache.cc"
    "transfer_cache.h"
    "transfer_cache_internals.h"
)

set(common_hdrs 
    "arena.h"
    "central_freelist.h"
    "common.h"
    "cpu_cache.h"
    "guarded_page_allocator.h"
    "huge_address_map.h"
    "huge_allocator.h"
    "tcmalloc_policy.h"
    "huge_cache.h"
    "huge_page_filler.h"
    "huge_pages.h"
    "huge_region.h"
    "huge_page_aware_allocator.h"
    "page_allocator.h"
    "page_allocator_interface.h"
    "page_heap.h"
    "page_heap_allocator.h"
    "pages.h"
    "pagemap.h"
    "parameters.h"
    "peak_heap_tracker.h"
    "sampler.h"
    "span.h"
    "span_stats.h"
    "stack_trace_table.h"
    "stats.h"
    "static_vars.h"
    "system-alloc.h"
    "thread_cache.h"
    "tracking.h"
    "transfer_cache_stats.h"
    "transfer_cache.h"
    "transfer_cache_internals.h"
)

set(common_deps 
    "tcmalloc::experiment"
    "tcmalloc::malloc_extension"
    "tcmalloc::new_extension"
    "tcmalloc::noruntime_size_classes"
    "tcmalloc::size_class_info"
    "absl::algorithm_container"
    "absl::base"
    "absl::config"
    "absl::core_headers"
    "absl::dynamic_annotations"
    "absl::fixed_array"
    "absl::stacktrace"
    "absl::symbolize"
    "absl::synchronization"
    "absl::hash"
    "absl::memory"
    "absl::strings"
    "absl::str_format"
    "absl::time"
    "absl::optional"
    "absl::span"
    "tcmalloc::atomic_stats_counter"
    "absl::bits"
    "tcmalloc::config"
    "tcmalloc::declarations"
    "tcmalloc::environment"
    "tcmalloc::linked_list"
    "tcmalloc::logging"
    "tcmalloc::mincore"
    "tcmalloc::numa"
    "tcmalloc::cache_topology"
    "tcmalloc::optimization"
    "tcmalloc::parameter_accessors"
    "tcmalloc::percpu"
    "tcmalloc::percpu_tcmalloc"
    "tcmalloc::range_tracker"
    "tcmalloc::timeseries_tracker"
    "tcmalloc::util"
)

tcmalloc_cc_library(
  NAME "common"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  LINKSTATIC
  DEPS ${common_deps}
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "common_deprecated_perthread"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  COPTS "-DTCMALLOC_DEPRECATED_PERTHREAD"
  LINKSTATIC
  DEPS ${common_deps}
  ALWAYSLINK
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use hugepage-aware
# allocator.
tcmalloc_cc_library(
  NAME "want_hpaa"
  SRCS "want_hpaa.cc"
  COPTS "-g0"
  PUBLIC
  DEPS 
        "tcmalloc::config"
        "absl::core_headers"
  ALWAYSLINK
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use hugepage-aware
# allocator with hpaa_subrelease=true.
tcmalloc_cc_library(
  NAME "want_hpaa_subrelease"
  SRCS "want_hpaa_subrelease.cc"
  COPTS "-g0"
  PUBLIC
  DEPS 
        "tcmalloc::config"
        "absl::core_headers"
  ALWAYSLINK
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to not use hugepage-aware
# allocator.
tcmalloc_cc_library(
  NAME "want_no_hpaa"
  SRCS "want_no_hpaa.cc"
  COPTS "-g0"
  PUBLIC
  DEPS 
        "tcmalloc::config"
        "absl::core_headers"
  ALWAYSLINK
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use old span sizes.
tcmalloc_cc_library(
  NAME "want_legacy_spans"
  SRCS "want_legacy_spans.cc"
  COPTS "-g0"
  DEPS 
        "tcmalloc::config"
        "absl::core_headers"
  ALWAYSLINK
)

# Add a dep to this if you want your binary to enable NUMA awareness by
# default.
tcmalloc_cc_library(
  NAME "want_numa_aware"
  SRCS "want_numa_aware.cc"
  COPTS "-g0"
  DEPS 
        "tcmalloc::config"
        "absl::core_headers"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "runtime_size_classes"
  SRCS "runtime_size_classes.cc"
  HDRS "runtime_size_classes.h"
  DEPS 
        "tcmalloc::size_class_info"
        "tcmalloc::environment"
        "tcmalloc::logging"
        "absl::core_headers"
        "absl::strings"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "noruntime_size_classes"
  SRCS "noruntime_size_classes.cc"
  HDRS "runtime_size_classes.h"
  DEPS 
        "tcmalloc::size_class_info"
        "absl::core_headers"
        "absl::strings"
  ALWAYSLINK
)

# TCMalloc with large pages is usually faster but fragmentation is higher.  See
# https://github.com/google/tcmalloc/tree/master/docs/tuning.md for more details.
tcmalloc_cc_library(
  NAME "tcmalloc_large_pages"
  SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
    
  COPTS "-DTCMALLOC_LARGE_PAGES"
  LINKSTATIC
  PUBLIC
  DEPS ${tcmalloc_deps} 
        "tcmalloc::common_large_pages"
    
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "common_large_pages"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  COPTS "-DTCMALLOC_LARGE_PAGES"
  LINKSTATIC
  DEPS ${common_deps}
  ALWAYSLINK
)

# TCMalloc with 256k pages is usually faster but fragmentation is higher.  See
# https://github.com/google/tcmalloc/tree/master/docs/tuning.md for more details.
tcmalloc_cc_library(
  NAME "tcmalloc_256k_pages"
  SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
    
  COPTS "-DTCMALLOC_256K_PAGES"
  LINKSTATIC
  PUBLIC
  DEPS ${tcmalloc_deps}
        "tcmalloc::common_256k_pages"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "common_256k_pages"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  COPTS "-DTCMALLOC_256K_PAGES"
  LINKSTATIC
  DEPS ${common_deps}
    ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "tcmalloc_256k_pages_and_numa"
  SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
    
  COPTS 
        "-DTCMALLOC_256K_PAGES"
        "-DTCMALLOC_NUMA_AWARE"
    
  LINKSTATIC
  DEPS ${tcmalloc_deps} 
        "tcmalloc::common_256k_pages_and_numa"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "common_256k_pages_and_numa"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  COPTS 
        "-DTCMALLOC_256K_PAGES"
        "-DTCMALLOC_NUMA_AWARE"
    
  LINKSTATIC
  DEPS ${common_deps}
  ALWAYSLINK
)

# TCMalloc small-but-slow is a a version of TCMalloc that chooses to minimize
# fragmentation at a *severe* cost to performance.  It should be used by
# applications that have significant memory constraints but don't need to
# frequently allocate/free objects.
#
# See https://github.com/google/tcmalloc/tree/master/docs/tuning.md for more details.
tcmalloc_cc_library(
  NAME "tcmalloc_small_but_slow"
  SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
    
  COPTS "-DTCMALLOC_SMALL_BUT_SLOW"
  LINKSTATIC
  PUBLIC
  DEPS ${tcmalloc_deps} 
        "tcmalloc::common_small_but_slow"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "common_small_but_slow"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  COPTS "-DTCMALLOC_SMALL_BUT_SLOW"
  LINKSTATIC
  DEPS ${common_deps}
  ALWAYSLINK
)

# TCMalloc with NUMA awareness compiled in. Note that by default NUMA awareness
# will still be disabled at runtime - this default can be changed by adding a
# dependency upon want_numa_aware or overridden by setting the
# TCMALLOC_NUMA_AWARE environment variable.
tcmalloc_cc_library(
  NAME "tcmalloc_numa_aware"
  SRCS 
        "libc_override.h"
        "libc_override_gcc_and_weak.h"
        "libc_override_glibc.h"
        "libc_override_redefine.h"
        "tcmalloc.cc"
        "tcmalloc.h"
    
  COPTS "-DTCMALLOC_NUMA_AWARE"
  LINKSTATIC
  DEPS ${tcmalloc_deps}
        "tcmalloc::common_numa_aware"
  ALWAYSLINK
)

tcmalloc_cc_library(
  NAME "common_numa_aware"
  SRCS ${common_srcs}
  HDRS ${common_hdrs}
  COPTS "-DTCMALLOC_NUMA_AWARE"
  LINKSTATIC
  DEPS ${common_deps}
  ALWAYSLINK
)


tcmalloc_cc_library(
  NAME "malloc_extension"
  SRCS "malloc_extension.cc"
  HDRS 
        "internal_malloc_extension.h"
        "malloc_extension.h"
  PUBLIC
  DEPS 
        "tcmalloc::parameter_accessors"
        "absl::config"
        "absl::core_headers"
        "absl::malloc_internal"
        "absl::function_ref"
        "absl::memory"
        "absl::strings"
        "absl::time"
        "absl::optional"
        "absl::span"
    
)

tcmalloc_cc_library(
  NAME "new_extension"
  SRCS "new_extension.cc"
  HDRS "new_extension.h"
  DEPS 
        "absl::core_headers"
)