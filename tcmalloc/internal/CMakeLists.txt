
tcmalloc_cc_library(
    NAME "atomic_danger"
    HDRS "atomic_danger.h"
    DEPS "tcmalloc::config"
)

tcmalloc_cc_library(
    NAME "atomic_stats_counter"
    HDRS "atomic_stats_counter.h"
    DEPS 
        "tcmalloc::config"
        "absl::core_headers"
)

tcmalloc_cc_library(
    NAME "clock"
    HDRS "clock.h"
    DEPS 
        "tcmalloc::config"
        "absl::base"
        "absl::function_ref"
        "absl::time"
)

tcmalloc_cc_library(
    NAME "config"
    HDRS "config.h"
    DEPS 
        "absl::config"
)

tcmalloc_cc_library(
    NAME "declarations"
    HDRS "declarations.h"    
)

tcmalloc_cc_library(
    NAME "environment"
    SRCS "environment.cc"
    HDRS "environment.h"
    DEPS "tcmalloc::config"
)

tcmalloc_cc_library(
    NAME "lifetime_predictions"
    HDRS "lifetime_predictions.h"
    DEPS 
        "tcmalloc::linked_list"
        "absl::algorithm_container"
        "absl::base"
        "absl::core_headers"
        "absl::malloc_internal"
        "absl::stacktrace"
        "absl::hash"
        "absl::time"
)

tcmalloc_cc_library(
    NAME "lifetime_tracker"
    HDRS "lifetime_tracker.h"   
    DEPS 
        "tcmalloc::clock"
        "tcmalloc::lifetime_predictions"
        "tcmalloc::linked_list"
        "absl::algorithm_container"
        "absl::base"
        "absl::core_headers"
        "absl::malloc_internal"
        "absl::stacktrace"
        "absl::hash"
        "absl::memory"
        "absl::time"
)

tcmalloc_cc_library(
    NAME "linked_list"
    HDRS "linked_list.h"   
    DEPS 
        "tcmalloc::logging"
        "absl::core_headers"
    
)

tcmalloc_cc_library(
    NAME "linux_syscall_support"
    HDRS "linux_syscall_support.h"
)

tcmalloc_cc_library(
    NAME "logging"
    SRCS "logging.cc"
    HDRS "logging.h"    
    DEPS 
        "tcmalloc::config"
        "tcmalloc::parameter_accessors"
        "tcmalloc::malloc_extension"
        "absl::base"
        "absl::core_headers"
        "absl::stacktrace"
        "absl::strings"
        "absl::str_format"
)

tcmalloc_cc_library(
    NAME "memory_stats"
    SRCS "memory_stats.cc"
    HDRS "memory_stats.h"
    DEPS 
        "tcmalloc::config"
        "tcmalloc::logging"
        "tcmalloc::util"
        "absl::strings"
)

tcmalloc_cc_library(
    NAME "mincore"
    SRCS "mincore.cc"
    HDRS "mincore.h"
    LINKSTATIC
    DEPS "tcmalloc::config"
)

tcmalloc_cc_library(
    NAME "mock_span"
    TESTONLY
    HDRS "mock_span.h"
    DEPS 
        "tcmalloc::linked_list"   
)

tcmalloc_cc_library(
    NAME "cache_topology"
    SRCS "cache_topology.cc"
    HDRS "cache_topology.h"    
    DEPS 
        "tcmalloc::config"
        "tcmalloc::logging"
        "tcmalloc::util"
        "absl::base"
        "absl::core_headers"
        "absl::function_ref"
        "absl::strings"
)

tcmalloc_cc_library(
    NAME "numa"
    SRCS "numa.cc"
    HDRS "numa.h"    
    DEPS 
        "tcmalloc::config"
        "tcmalloc::environment"
        "tcmalloc::logging"
        "tcmalloc::percpu"
        "tcmalloc::util"
        "absl::base"
        "absl::core_headers"
        "absl::function_ref"
        "absl::strings"
)

tcmalloc_cc_library(
    NAME "optimization"
    HDRS "optimization.h"    
    DEPS 
        "tcmalloc::logging"
)

tcmalloc_cc_library(
    NAME "parameter_accessors"
    HDRS "parameter_accessors.h"    
    DEPS 
        "absl::core_headers"
        "absl::time"
)

tcmalloc_cc_library(
    NAME "percpu"
    SRCS 
        "percpu.cc"
        "percpu_rseq_asm.S"
        "percpu_rseq_unsupported.cc"
    HDRS 
      "percpu.h"
    DEPS 
        "tcmalloc::atomic_danger"
        "tcmalloc::config"
        "tcmalloc::linux_syscall_support"
        "tcmalloc::logging"
        "tcmalloc::optimization"
        "tcmalloc::util"
        "absl::base"
        "absl::core_headers"
        "absl::dynamic_annotations"
)

tcmalloc_cc_library(
    NAME "percpu_tcmalloc"
    HDRS "percpu_tcmalloc.h"
    DEPS 
        "tcmalloc::mincore"
        "tcmalloc::percpu"
        "absl::base"
        "absl::dynamic_annotations"
)

tcmalloc_cc_library(
    NAME "proc_maps"
    SRCS "proc_maps.cc"
    HDRS "proc_maps.h"
    DEPS 
        "tcmalloc::config"
        "tcmalloc::logging"
        "tcmalloc::util"
        "absl::core_headers"
        "absl::str_format"
    
)

tcmalloc_cc_library(
    NAME "range_tracker"
    HDRS "range_tracker.h"
    DEPS 
        "tcmalloc::logging"
        "tcmalloc::optimization"
        "absl::bits"
)

# An empty rule to force libc malloc instead of TCMalloc.
tcmalloc_cc_library(
    NAME "system_malloc"
    COPTS TCMALLOC_DEFAULT_COPTS
    LINKSTATIC    
)

tcmalloc_cc_library(
    NAME "timeseries_tracker"
    HDRS "timeseries_tracker.h"
    DEPS 
        "tcmalloc::clock"
        "tcmalloc::logging"
        "absl::base"
        "absl::function_ref"
        "absl::bits"
        "absl::int128"
        "absl::time"
)

tcmalloc_cc_library(
    NAME "util"
    SRCS "util.cc"
    HDRS "util.h"
    DEPS 
        "tcmalloc::config"
        "tcmalloc::logging"
        "absl::base"
        "absl::core_headers"
        "absl::time"
        "absl::span"
)