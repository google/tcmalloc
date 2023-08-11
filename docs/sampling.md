# How sampling in TCMalloc works.

## Introduction

TCMalloc uses sampling to get representative data on memory usage and
allocation.

## Sampling

We chose to sample an allocation every N bytes where N is a random value using
[Sampler::PickNextSamplingPoint()](https://github.com/google/tcmalloc/blob/master/tcmalloc/sampler.cc)
with a mean set by the profile sample rate using
[MallocExtension::SetProfileSamplingRate()](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h).

By default this is every 2MiB, and can be overridden in code.

## How We Sample Allocations

We'd like to sample each byte in memory with a uniform probability. The
granularity there is too fine; for better fast-path performance, we can use a
simple counter and sample an allocation if 1 or more bytes in it are sampled
instead. This causes a slight statistical skew; the larger the allocation, the
more likely it is that more than one byte of it should be sampled, which we
correct for, as well as the fact that requested and allocated size may be
different, in the weighting process.

[Sampler::RecordAllocationSlow()](https://github.com/google/tcmalloc/blob/master/tcmalloc/sampler.cc)
determines when we should sample an allocation; if we should, it returns a
*weight* that indicates how many bytes that the sampled allocation represents in
expectation.

We do some additional processing around that allocation using
[SampleifyAllocation()](https://github.com/google/tcmalloc/blob/master/tcmalloc/allocation_sampling.h)
to record the call stack, alignment, request size, and allocation size. Then we
go through all the active samplers using
[ReportMalloc()](https://github.com/google/tcmalloc/blob/master/tcmalloc/allocation_sample.h)
and tell them about the allocation.

We also tell the span that we're sampling it. We can do this because we do
sampling at tcmalloc page sizes, so each sample corresponds to a particular page
in the pagemap.

For small allocations, we make two allocations: the returned allocation (which
uses an entire tcmalloc page, not shared with any other allocations) and a proxy
allocation in a non-sampled span (the proxy object is used when computing
fragmentation profiles).

When allocations are sampled, the virtual addresses associated with the
allocation are
[`madvise`d with the `MADV_NOHUGEPAGE` flag](https://github.com/google/tcmalloc/blob/master/tcmalloc/system-alloc.cc).
This, combined with the whole-page behavior above, means that *every allocation
gets its own native (OS) page(s)* shared with no other allocations.

## How We Free Sampled Objects

Each sampled allocation is tagged. Using this, we can quickly test whether a
particular allocation might be a sample.

When we are done with the sampled span we release it using
[tcmalloc::Span::Unsample()](https://github.com/google/tcmalloc/blob/master/tcmalloc/span.cc).

## How Do We Handle Heap and Fragmentation Profiling

To handle heap and fragmentation profiling we just need to traverse the list of
sampled objects and compute either their degree of fragmentation (with the proxy
object), or the amount of heap they consume.

Each allocation gets additional metadata associated with it when it is exposed
in the heap profile. In the preparation for writing the heap profile,
[MergeProfileSamplesAndMaybeGetResidencyInfo()](https://github.com/google/tcmalloc/blob/master/tcmalloc/internal/profile_builder.cc)
probes the operating system for whether or not the underlying memory pages in
the sampled allocation are swapped or not resident at all (this can happen if
they've never been written to). We use
[`/proc/pid/pagemap`](https://www.kernel.org/doc/Documentation/vm/pagemap.txt)
to obtain this information for each underlying OS page.

The OS is more aggressive at swapping out pages for sampled allocations than the
statistics might otherwise indicate. Sampled allocations do not share memory
pages (either huge or otherwise) with any other allocations, so a sampled
rarely-accessed allocation becomes eligible for reclaim more readily than an
allocation that has not been sampled (which might share pages with other
allocations that are heavily accessed). This design is a fortunate consequence
of other aspects of sampling; we want to identify specific allocations as being
more readily swapped independent of our memory allocation behavior.

More information is available via pageflags, but these require `root` to access
`/proc/kpageflags`. To make this information available to tcmalloc,
[proposed kernel changes](https://patchwork.kernel.org/project/linux-mm/list/?series=572147)
would need to be merged.

## How Do We Handle Allocation Profiling

Allocation profiling reports a list of sampled allocations during a length of
time. We start an allocation profile using
[MallocExtension::StartAllocationProfiling()](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h),
then wait until time has elapsed, then call `Stop` on the token. and report the
profile.

While the allocation sampler is active it is added to the list of samplers for
allocations and removed from the list when it is claimed.

## How Do We Handle Lifetime Profiling

Lifetime profiling reports a list of object lifetimes as pairs of allocation and
deallocation records. Profiling is initiated by calling
[MallocExtension::StartLifetimeProfiling()](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h).
Profiling continues until `Stop` is invoked on the token. Lifetimes are only
reported for objects where allocation *and* deallocation are observed while
profiling is active. A description of the sampling based lifetime profiler can
be found in Section 4 of
["Learning-based Memory Allocation for C++ Server Workloads, ASPLOS 2020"](https://research.google/pubs/pub49008/).
