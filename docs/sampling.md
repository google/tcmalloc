# How sampling in TCMalloc works.

## Introduction

TCMalloc uses sampling to get representative data on memory usage and
allocation. How this works is not well documented. This doc attempts to at least
partially fix this.

## Sampling

We chose to sample an allocation every N bytes where N is a
[random value](https://github.com/google/tcmalloc/blob/master/tcmalloc/sampler.cc)
with a mean set by the
[profile sample rate](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h).
By default this is every
[2MiB](https://github.com/google/tcmalloc/blob/master/tcmalloc/common.h).

## How We Sample Allocations

When we
[pick an allocation](https://github.com/google/tcmalloc/blob/master/tcmalloc/sampler.cc)
to sample we do some
[additional processing around that allocation](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc) -
recording stack, alignment, request size, and allocation size. Then we go
[through all the active samplers](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc)
and tell them about the allocation. We also tell the
[span that we're sampling it](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc) -
we can do this because we do sampling at tcmalloc page sizes, so each sample
corresponds to a particular page in the pagemap.

## How We Free Sampled Objects

Each sampled allocation is tagged. So we can
quickly[ test whether a particular allocation might be a sample](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc).

When we are done with the sampled span
[we release it](https://github.com/google/tcmalloc/blob/master/tcmalloc/span.cc).

## How Do We Handle Heap and Fragmentation Profiling

To handle
[heap](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc)
and
[fragmentation](https://github.com/google/tcmalloc/blob/master/tcmalloc/tcmalloc.cc)
profiling we just need to traverse the list of sampled objects and compute
either their degree of fragmentation, or the amount of heap they consume.

## How Do We Handle Allocation Profiling

Allocation profiling reports a list of sampled allocations during a length of
time. We start an
[allocation profile](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h),
then wait until time has elapsed, then call `Stop` on the token. and report the
profile.

While the allocation sampler is active it is added to the list of samplers for
allocations and removed from the list when it is claimed.
