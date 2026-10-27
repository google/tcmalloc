# Understanding Malloc Stats

## Getting Malloc Stats

Human-readable statistics can be obtained by calling
`tcmalloc::MallocExtension::GetStats()`.

## Understanding Malloc Stats Output

### It's A Lot Of Information

The output contains a lot of information. Much of it is debug telemetry for
engineers working on TCMalloc internals, while the top-level summary and cache
breakdowns are the most useful sections for application debugging.

### Summary Section

The most generally useful section is the first few lines:

```
See https://github.com/google/tcmalloc/tree/master/docs/stats.md for an explanation of this page
------------------------------------------------
MALLOC:    10858234672 (10355.2 MiB) Bytes in use by application
MALLOC: +    827129856 (  788.8 MiB) Bytes in page heap freelist
MALLOC: +    386098400 (  368.2 MiB) Bytes in central cache freelist
MALLOC: +    105330688 (  100.5 MiB) Bytes in per-CPU cache freelist
MALLOC: +            0 (    0.0 MiB) Bytes in Sharded cache freelist
MALLOC: +      9095680 (    8.7 MiB) Bytes in transfer cache freelist
MALLOC: +       660976 (    0.6 MiB) Bytes in thread cache freelists
MALLOC: +     49333930 (   47.0 MiB) Bytes in malloc metadata
MALLOC: +       629440 (    0.6 MiB) Bytes in malloc metadata Arena unallocated
MALLOC: +      1599704 (    1.5 MiB) Bytes in malloc metadata Arena unavailable
MALLOC:   ------------
MALLOC: =  12238113346 (11671.2 MiB) Actual memory used (physical + swap)
MALLOC: +    704643072 (  672.0 MiB) Bytes released to OS (aka unmapped)
MALLOC:   ------------
MALLOC: =  12942756418 (12343.2 MiB) Virtual address space used
```

*   **Bytes in use by application:** Number of bytes that the application is
    actively using to hold data. This is computed by the bytes requested from
    the OS minus any bytes that are held in caches and other internal data
    structures.
*   **Bytes in page heap freelist:** The page heap holds memory ready for
    TCMalloc to use. This memory is not actively being used, and could be
    returned to the OS. [See TCMalloc tuning](tuning.md).
*   **Bytes in central cache freelist:** The amount of memory currently held in
    the central freelist. This structure holds partially used
    "[spans](#more-detail-on-metadata)" of memory that have some objects
    allocated from them and some free objects remaining.
*   **Bytes in per-CPU cache freelist:** In per-CPU mode (which is the default),
    each CPU holds memory ready to quickly hand to the application. The maximum
    size of this per-CPU cache is tunable. [See TCMalloc tuning](tuning.md).
*   **Bytes in Sharded cache freelist:** When the sharded transfer cache is
    active, each L3 cache shard maintains its own freelist of objects to reduce
    cross-domain cache traffic. This reports the total bytes held across all
    active shards.
*   **Bytes in transfer cache freelist:** The transfer cache sits in front of
    the central freelist and holds batches of same-sized objects ready to be
    provided to the frontend
    caches.
*   **Bytes in thread cache freelists:** The TC in TCMalloc stands for thread
    cache. Originally each thread held its own cache of memory to provide to the
    application. Since per-CPU caches became the default, thread caches are used
    mainly before a process transitions into per-CPU mode or when per-CPU caches
    are disabled.
*   **Bytes in malloc metadata:** The size of the data structures used for
    tracking memory allocation. This grows as the amount of tracked memory
    grows.
*   **Bytes in malloc metadata Arena unallocated:** Metadata is allocated in an
    internal Arena. Memory requests to the OS are made in blocks to amortize
    several Arena allocations, and this captures memory in the current block
    that is not yet allocated but can be used by future Arena allocations.
*   **Bytes in malloc metadata Arena unavailable:** The Arena allocator leaves
    the tail of a block unused when a subsequent Arena allocation request is
    larger than the block's remaining space. This memory is unavailable for
    future Arena allocations.

The summary block ends with three totals:

*   **Actual memory used:** The total amount of memory that TCMalloc accounts as
    backed across the categories above. Its actual contribution to RSS may be
    smaller if pages have not yet been faulted in, or larger if small regions
    are mapped with transparent hugepages. This does not include memory outside
    TCMalloc, such as memory-mapped files, thread stacks, or text segments.
*   **Bytes released to OS:** TCMalloc releases unused page heap and metadata
    arena memory back to the OS with `madvise`, as described in
    [TCMalloc tuning](tuning.md). This value sums unmapped page heap bytes and
    non-resident metadata arena bytes, while the OS decides when to reclaim the
    underlying physical pages.
*   **Virtual address space used:** The total virtual address space managed by
    TCMalloc, equal to actual memory used plus bytes released to the OS.

### More Detail On Metadata

The next lines break down internal metadata structures, per-CPU slab residency,
and metadata arena allocations:

```
MALLOC:         236176               Spans in use
MALLOC:         238709 (   10.9 MiB) Spans created
MALLOC:              8               Thread heaps in use
MALLOC:             46 (    0.0 MiB) Thread heaps created
MALLOC:          13517               Stack traces in use
MALLOC:          13742 (    7.2 MiB) Stack traces created
MALLOC:              0               Table buckets in use
MALLOC:           2808 (    0.0 MiB) Table buckets created
MALLOC:       11665416 (   11.1 MiB) Pagemap bytes used
MALLOC:        4067336 (    3.9 MiB) Pagemap root resident bytes
MALLOC:        4194304 (    4.0 MiB) Pagemap root size
MALLOC:       33554796 (   32.0 MiB) per-CPU slab bytes used
MALLOC:       33554432 (   32.0 MiB) per-CPU slab resident bytes
MALLOC:       83886080 (   80.0 MiB) malloc metadata Arena non-resident bytes
```

*   **Spans:** Structures that track contiguous runs of [pages](#page-sizes).
*   **Thread heaps:** Per-thread cache structures used in per-thread mode.
*   **Stack traces:** Metadata records holding allocation call stacks for
    sampled live objects.
*   **Table buckets:** Hash table bucket entries used to aggregate stack traces
    when generating heap profiles.
*   **Pagemap:** Radix tree mapping page addresses to span and size-class
    metadata. The three pagemap lines report the total memory allocated for
    pagemap nodes, the resident physical bytes of the root array, and the total
    virtual size of the root array.
*   **per-CPU slab:** The allocated virtual size and resident physical size of
    the per-CPU cache slab region.
*   **malloc metadata Arena non-resident bytes:** Metadata arena pages, such as
    released per-CPU slab metadata, that are not resident in physical memory and
    are counted under bytes released to the OS.

### Realized Fragmentation

```
MALLOC:    12238113346 (11671.2 MiB) Actual memory used at peak
MALLOC:    11626207678 (11087.6 MiB) Estimated in-use at peak
MALLOC:              5.2632          Realized fragmentation (%)
```

Memory overhead at peak demand is more important than off-peak, since a process
must be provisioned with enough memory to survive its peak requirements without
running out of memory. After a peak in demand, memory may be deallocated and
held in caches in anticipation of future reuse; overhead as a fraction of the
remaining live allocations rises, but no additional memory is required.

This metric is called "realized fragmentation" and is described in
["Adaptive Hugepage Subrelease for Non-moving Memory Allocators in Warehouse-Scale Computers"](https://research.google/pubs/pub50436/).
The summary block reports a lifetime snapshot taken when TCMalloc reached its
peak backed memory, using periodic allocation sampling to estimate live
application usage at that peak.

### Page Sizes

There are three relevant page sizes for systems and TCMalloc:

*   **System default page size:** The OS base page size, such as 4 KiB on x86.
    It is not reported directly here and differs from TCMalloc's logical page
    size.
*   **TCMalloc page size:** The basic unit of memory management for TCMalloc.
    Objects on the same TCMalloc page share the same size class. TCMalloc
    supports 4 KiB, 8 KiB (the default), 32 KiB, and 256 KiB pages:
    *   Smaller page sizes reduce internal fragmentation and make it easier for
        all objects on a page to become free so the page can be reused for a
        different size class.
    *   Larger page sizes require fewer fetches from the page heap and keep
        same-sized objects in closer spatial proximity.
*   **TCMalloc hugepage size:** The hardware hugepage size on the system, such
    as 2 MiB on x86, used as a management unit by [Temeraire](#temeraire).

```
MALLOC:           8192               Tcmalloc page size
MALLOC:        2097152               Tcmalloc hugepage size
MALLOC:             64               CPUs Allowed in Mask
MALLOC:             11               Arena blocks
MALLOC:              0               Arena freelist blocks
MALLOC:   Arena per-type allocations:
MALLOC:     Span:                     10.927 MiB
MALLOC:     SampledAllocation:         7.210 MiB
MALLOC:     ThreadCache:               0.044 MiB
MALLOC:     StackTraceTable:           0.043 MiB
MALLOC:     FillerTracker:             1.512 MiB
MALLOC:     HugeRegion:                0.150 MiB
MALLOC:     PageMap:                  11.125 MiB
MALLOC:     PageAllocator:             0.002 MiB
MALLOC:     GuardedPageAllocator:      0.133 MiB
MALLOC:     HugePageMetadata:          0.001 MiB
MALLOC:     TransferCache:             1.085 MiB
MALLOC:     CpuCache:                112.450 MiB
```

Following the page sizes, TCMalloc reports:

*   **CPUs Allowed in Mask:** The number of logical CPUs permitted in the
    process's CPU affinity mask.
*   **Arena blocks / Arena freelist blocks:** The total number of metadata Arena
    blocks allocated from the system and the number of free blocks on the
    Arena's internal freelist.
*   **Arena per-type allocations:** The cumulative MiB allocated from the
    metadata Arena for each internal component with a non-zero total.

### Experiments

There is an experiment framework embedded into TCMalloc.
The status of each known experiment is reported as 1 when active and 0 when
inactive:

```
MALLOC EXPERIMENTS: TCMALLOC_DEMAND_CYCLE_120S=0 TCMALLOC_PGHO_EXPERIMENT=0
```

### Hooks and Sampled Profiles

The state of TCMalloc allocation hooks and sampled heap profiles is reported
next:

```
MALLOC HOOKS: NEW=0 DELETE=0 SAMPLED_NEW=0 SAMPLED_DELETE=0
MALLOC SAMPLED PROFILES: 202167097 bytes (current), 9996851 bytes (internal fragmentation), 404936892 bytes (peak), 1485097 count (total)
```

The sampled profiles line reports the current estimated live heap bytes, the
estimated internal fragmentation bytes from size-class rounding, the peak
sampled live heap size in bytes, and the cumulative count of sampled allocations
since startup.

### Actual Memory Footprint

The output next reports process-wide memory metrics recorded by the OS from
`/proc/self/statm` and `/proc/self/status`:

*   **Bytes resident:** Physical memory currently resident in RAM for the entire
    process, including non-malloc sources such as program text, shared
    libraries, and memory-mapped files.
*   **Bytes mapped:** Total virtual address space mapped by the process.
*   **Bytes via VmPTE:** Memory consumed by kernel page table entries for the
    process.

```
Total process stats (inclusive of non-malloc sources):
TOTAL:  86880677888 (82855.9 MiB) Bytes resident (physical memory used)
TOTAL:  89124790272 (84996.0 MiB) Bytes mapped (virtual memory used)
TOTAL:      1187840 (    1.1 MiB) Bytes via VmPTE
------------------------------------------------
Call ReleaseMemoryToSystem() to release freelist memory to the OS (via madvise()).
Bytes released to the OS take up virtual address space but no physical memory.
```

When the internal stats buffer is smaller than 10,000 bytes, output stops after
this banner and skips directly to
[Memory Requested From The OS](#memory-requested-from-the-os). Calls to
`tcmalloc::MallocExtension::GetStats()` continue with the detailed sections
below.

### Per Size-Class Information

Allocation requests up to 256 KiB are rounded up to discrete size classes. For
example, a 15-byte request is rounded to the 16-byte size class. Each row in the
per-size-class table reports the following columns from left to right:

*   **class and bytes:** The size class index and object size in bytes.
*   **objs and MiB:** The free object count and total free bytes in MiB held
    across the per-CPU, per-thread, sharded transfer, transfer, and central
    caches.
*   **cum MiB:** The cumulative cached free size in MiB across this size class
    and all smaller size classes.
*   **live pages:** The number of live TCMalloc pages currently dedicated to
    spans of this size class.
*   **spans ret / req:** The cumulative number of spans returned to and
    requested from the page allocator, followed by their ratio.
*   **objs/span:** The number of objects packed into each span of this size
    class.

```
------------------------------------------------
Total size of freelists for per-thread and per-CPU caches,
transfer cache, and central cache, as well as number of
live pages, returned/requested spans by size class
------------------------------------------------
class   1 [        8 bytes ] :    45645 objs;   0.3 MiB;    0.3 cum MiB;       73 live pages; spans:         19 ret /         92 req = 0.2065;       1024 objs/span;
class   2 [       16 bytes ] :    39942 objs;   0.6 MiB;    1.0 cum MiB;      120 live pages; spans:          3 ret /        123 req = 0.0244;        512 objs/span;
class   3 [       24 bytes ] :    84130 objs;   1.9 MiB;    2.9 cum MiB;      807 live pages; spans:       1330 ret /       2137 req = 0.6224;        341 objs/span;
class   4 [       32 bytes ] :   107271 objs;   3.3 MiB;    6.2 cum MiB;     1048 live pages; spans:        420 ret /       1468 req = 0.2861;        256 objs/span;
class   5 [       40 bytes ] :    82230 objs;   3.1 MiB;    9.3 cum MiB;      790 live pages; spans:        962 ret /       1752 req = 0.5491;        204 objs/span;
...
```

### Central Cache Free List Span Utilization

The central cache freelist manages memory in spans of one or more TCMalloc pages
and reports three per-size-class histograms.

First, the span utilization histogram prints two lines per size class:

1.  A 16-bucket power-of-two histogram from `< 2` through `< 65536` showing the
    non-cumulative number of spans by how many allocated objects they currently
    hold.
2.  The number of partially populated spans in each of the central freelist's
    eight priority lists, indexed from 0 to 7, where higher list indices hold
    more densely allocated spans.

```
------------------------------------------------
Central cache freelist: Span utilization histogram
Non-cumulative number of spans with allocated objects < N
------------------------------------------------
class   1 [        8 bytes ] :      0 < 2,     0 < 4,     0 < 8,     0 < 16,     1 < 32,     0 < 64,     1 < 128,     1 < 256,     1 < 512,     0 < 1024,     0 < 2048,     4 < 4096,    16 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
class   1 [        8 bytes ] :      0: 0,     1: 0,     2: 0,     3: 1,     4: 1,     5: 2,     6: 4,     7: 16
class   2 [       16 bytes ] :      0 < 2,     0 < 4,     0 < 8,     0 < 16,     0 < 32,     0 < 64,     0 < 128,     0 < 256,     0 < 512,     1 < 1024,     0 < 2048,    47 < 4096,     0 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
class   2 [       16 bytes ] :      0: 0,     1: 0,     2: 0,     3: 0,     4: 0,     5: 1,     6: 0,     7: 47
...
```

Second, the spans-used histogram counts how many distinct spans the central
freelist had to touch to satisfy a single batch allocation request, bucketed by
powers of two from 1 through 128:

```
------------------------------------------------
Central cache freelist: Number of spans used histogram
Number of spans used to fill a batch of allocations, from 0 to N
------------------------------------------------
class   1 [        8 bytes ] : 1 :      112, 2 :       14, 4 :        3, 8 :        0, 16 :        0, 32 :        0, 64 :        0, 128 :        0
class   2 [       16 bytes ] : 1 :      120, 2 :        3, 4 :        0, 8 :        0, 16 :        0, 32 :        0, 64 :        0, 128 :        0
...
```

Third, the same-span returns histogram measures batch coalescing when freed
objects are returned to the central freelist. Returned batches are sorted by
span so objects belonging to the same span are returned in a single run. The
eight columns bucket each batch by the number of extra objects coalesced into
existing span runs, using power-of-two buckets for 0, 1, 2 to 3, 4 to 7, 8 to
15, 16 to 31, 32 to 63, and 64 to 127 objects:

```
------------------------------------------------
Central cache freelist: Same-span returns
------------------------------------------------
class   1 [        8 bytes ] :     45     82    130     95     41     12      0      0
class   2 [       16 bytes ] :     38     64    110     72     19      4      0      0
...
```

### Transfer Cache Information

The transfer cache sits between the frontend caches and the central freelist.
For each size class, TCMalloc reports:

*   **objs, MiB, and cum MiB:** The number of free objects currently held in the
    transfer cache, their size in MiB, and the cumulative size across this and
    all smaller size classes.
*   **capacity and max_capacity:** The current capacity and maximum allowed
    capacity of the transfer cache ring buffer, measured in objects.
*   **insert hits, insert misses, and object misses:** Batch-level insert hits
    and misses, followed by the number of individual objects that could not fit
    in the transfer cache and were forwarded to the backing freelist.
*   **remove hits, remove misses, and object misses:** Batch-level remove hits
    and misses, followed by the number of individual objects that had to be
    fetched from the backing freelist.
*   **frontline hit rate:** The estimated percentage of frontend allocations in
    this size class satisfied by the per-CPU or per-thread caches without
    reaching this transfer cache layer.

```
------------------------------------------------
Used bytes, current capacity, and maximum allowed capacity
of the transfer cache freelists.
It also reports insert/remove hits/misses by size class.
------------------------------------------------
class   1 [        8 bytes ] :     1472 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;      935 insert hits;     8543 insert misses (    273376 object misses);      889 remove hits;     6612 remove misses (    211584 object misses); 98.12345 frontline hit rate
class   2 [       16 bytes ] :      608 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;      575 insert hits;     3739 insert misses (    119648 object misses);      556 remove hits;     3368 remove misses (    107776 object misses); 97.45120 frontline hit rate
class   3 [       24 bytes ] :      864 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;     1533 insert hits;    15594 insert misses (    499008 object misses);     1506 remove hits;    11939 remove misses (    382048 object misses); 96.81040 frontline hit rate
...
```

Immediately following the main transfer cache table, TCMalloc prints cumulative
statistics for the sharded transfer cache. This block reports whether the
sharded transfer cache is active or inactive, the number of initialized shards
across L3 cache domains, and the same per-size-class columns summed across all
active shards:

```
------------------------------------------------
Cumulative sharded transfer cache stats.
Used bytes, current capacity, and maximum allowed capacity
of the sharded transfer cache freelists.
It also reports insert/remove hits/misses by size class.
------------------------------------------------
Sharded transfer cache state: ACTIVE
Number of active sharded transfer caches:   2
------------------------------------------------
class   1 [        8 bytes ] :        0 objs;   0.0 MiB;    0.0 cum MiB;     0 capacity;        0 max_capacity;        0 insert hits;        0 insert misses (         0 object misses);        0 remove hits;        0 remove misses (         0 object misses); 0.00000 frontline hit rate
...
```

### Per-CPU Information

If the per-CPU cache is enabled, TCMalloc reports the memory cached on each
logical CPU.

The header line reports the maximum byte limit per CPU, configured via
`tcmalloc::MallocExtension::GetMaxPerCpuCacheSize()` and described in
[TCMalloc tuning](tuning.md). Each CPU row then reports:

*   **cpu:** The logical CPU ID.
*   **bytes:** The bytes and MiB of free objects currently cached in that CPU's
    slab.
*   **unallocated:** The spare bytes remaining in that CPU's cache budget.
*   **active and populated:** Trailing status flags indicating whether the CPU
    is in the process's CPU affinity mask and whether its slab has been
    populated.

Each size class on a CPU has a dynamic capacity up to a static per-size-class
header limit. Summing each size class's dynamic capacity multiplied by its
object size gives the total memory reserved for size-class capacities on that
CPU. The unallocated column reports the spare budget left over after subtracting
that reserved capacity sum from the per-CPU byte limit, whereas the first column
reports the bytes of objects actually stored in those slots right now.

```
------------------------------------------------
Bytes in per-CPU caches (per cpu limit: 3145728 bytes)
------------------------------------------------
cpu   0:      2168200 bytes (    2.1 MiB) with       52536 bytes unallocated  active populated
cpu   1:      1734880 bytes (    1.7 MiB) with      258944 bytes unallocated  active populated
cpu   2:      1779352 bytes (    1.7 MiB) with        8384 bytes unallocated  active populated
cpu   3:      1414224 bytes (    1.3 MiB) with      112432 bytes unallocated  active populated
cpu   4:            0 bytes (    0.0 MiB) with     3145728 bytes unallocated
...
```

### Size Class Capacity Information in Per-CPU Caches

For each size class across all populated per-CPU caches, TCMalloc summarizes the
allocated freelist capacity in objects and recent miss timing:

*   **class and bytes:** Size class index and object size in bytes.
*   **minimum, average, and maximum:** The minimum, average, and maximum
    capacity currently assigned to this size class across populated per-CPU
    caches.
*   **maximum allowed capacity:** The static upper limit on the number of
    objects that can be cached in a single CPU's slab for that size class.
*   **maximum capacity misses:** The number of times a per-CPU cache tried to
    grow its capacity for this size class while already at the maximum allowed
    capacity.
*   **underflow and overflow:** The minimum and maximum elapsed time in
    microseconds since the last underflow and overflow across populated CPUs for
    this size class, paired with the CPU ID where each extremum occurred, or -1
    when none has been recorded.

```
------------------------------------------------
Size class capacity statistics in per-cpu caches
------------------------------------------------
class   1 [        8 bytes ] :      0 (minimum),   133.1 (average),    636 (maximum),   2048 maximum allowed capacity, maximum capacity misses        0, (underflow: [0 us CPU -1, 0 us CPU -1]; overflow [0 us CPU -1, 0 us CPU -1]
class   2 [       16 bytes ] :      0 (minimum),    51.8 (average),    378 (maximum),   2048 maximum allowed capacity, maximum capacity misses        0, (underflow: [0 us CPU -1, 0 us CPU -1]; overflow [0 us CPU -1, 0 us CPU -1]
class   3 [       24 bytes ] :      0 (minimum),   119.3 (average),    510 (maximum),   2048 maximum allowed capacity, maximum capacity misses        0, (underflow: [0 us CPU -1, 0 us CPU -1]; overflow [0 us CPU -1, 0 us CPU -1]
class   4 [       32 bytes ] :      0 (minimum),   100.0 (average),    542 (maximum),   2048 maximum allowed capacity, maximum capacity misses        0, (underflow: [0 us CPU -1, 0 us CPU -1]; overflow [0 us CPU -1, 0 us CPU -1]
class   5 [       40 bytes ] :      0 (minimum),    80.6 (average),    467 (maximum),   2048 maximum allowed capacity, maximum capacity misses        0, (underflow: [0 us CPU -1, 0 us CPU -1]; overflow [0 us CPU -1, 0 us CPU -1]
...
```

### Number of per-CPU cache underflows, overflows, and drains

TCMalloc also tracks per-CPU cache miss, drain, and resize counts:

*   **underflows:** The application allocated an object when the per-CPU
    freelist for that size class was empty, requiring a refill from the
    middle-end.
*   **overflows:** The application freed an object when the per-CPU freelist for
    that size class was full, requiring a batch return to the middle-end. A high
    ratio of overflows to underflows approaching 1.0 indicates that objects are
    cycling between the per-CPU cache and middle-end because the cache is too
    small.
*   **drains (reclaims):** Number of times an idle CPU's cache was reclaimed and
    drained back to the middle-end.
*   **resizes:** Number of dynamic size-class capacity resize operations on that
    CPU.

```
------------------------------------------------
Number of per-CPU cache underflows, overflows, and drains
------------------------------------------------
Total  :         242 underflows,          12 overflows, overflows / underflows:  0.05,          168 drains (reclaims),           34 resizes
cpu   0:          69 underflows,           5 overflows, overflows / underflows:  0.07,           46 drains (reclaims),           10 resizes
cpu   1:          58 underflows,           0 overflows, overflows / underflows:  0.00,           42 drains (reclaims),            8 resizes
cpu   2:          62 underflows,           7 overflows, overflows / underflows:  0.11,           42 drains (reclaims),            9 resizes
cpu   3:          40 underflows,           0 overflows, overflows / underflows:  0.00,           27 drains (reclaims),            5 resizes
cpu   4:          13 underflows,           0 overflows, overflows / underflows:  0.00,           11 drains (reclaims),            2 resizes
cpu   5:           0 underflows,           0 overflows, overflows / underflows:  0.00,            0 drains (reclaims),            0 resizes
...
```

### Per-CPU Cache Slab Resizing Information

TCMalloc can dynamically adjust the per-CPU slab shift at runtime, where shift
`N` gives `2^N` bytes per CPU. This section reports the active shift and per-CPU
slab size in KiB, the cumulative number of slab growths and shrinkages recorded
at each shift level, and the number of bytes on retired slabs where
`MADV_DONTNEED` failed:

```
------------------------------------------------
Per-CPU cache slab resizing info:
------------------------------------------------
Current shift:  18 (slab size:  256 KiB)
shift  14:           0 growths,            0 shrinkages
shift  15:           0 growths,            0 shrinkages
shift  16:           0 growths,            0 shrinkages
shift  17:           0 growths,            0 shrinkages
shift  18:           0 growths,            0 shrinkages
           0 bytes for which MADVISE_DONTNEED failed
```

## Temeraire

### Pageheap Information

The page allocator, known as Temeraire or `HugePageAwareAllocator`, manages
memory at TCMalloc page and hardware hugepage granularity. It supplies pages to
the middle-end caches and directly serves allocations larger than 256 KiB. See
the [Temeraire design doc](temeraire.md) for architectural details.

**Note:** TCMalloc uses *unmapped* to mean that it has advised the OS via
`madvise` that the memory is no longer needed; actual physical reclamation is up
to the kernel.

TCMalloc prints the page allocator sections below first for the default normal
heap, and then repeats them wrapped in
`>>>>>>> Begin <LABEL> page allocator <<<<<<<` and
`>>>>>>> End <LABEL> page allocator <<<<<<<` banners for any additional active
heap partitions:

*   **NORMAL_P1:** Second normal partition when NUMA awareness or security heap
    partitioning is active.
*   **SAMPLED_OR_COLD:** Dedicated page allocator for sampled and cold-hinted
    allocations.
*   **SAMPLED_OR_COLD_P1:** Second sampled and cold partition when full heap
    partitioning is active.

### Summary Statistics

The initial table summarizes free and unmapped spans held by the page allocator
by span length in TCMalloc pages:

```
------------------------------------------------
HugePageAware: 75 sizes;  938.8 MiB free; 1154.0 MiB unmapped
------------------------------------------------
     1 pages *  86655 spans ~  677.0 MiB;  677.0 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
     2 pages *   3632 spans ~   56.8 MiB;  733.7 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
     3 pages *    288 spans ~    6.8 MiB;  740.5 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
     4 pages *    250 spans ~    7.8 MiB;  748.3 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
...
 >=128 large *    577 spans ~ 1154.0 MiB; 2092.8 MiB cum; unmapped: 1154.0 MiB; 1154.0 MiB cum
```

The header line reports the number of distinct non-empty small span sizes under
128 pages, the total backed free MiB, and the total unmapped MiB. Each
subsequent line reports:

*   **pages / large:** The span length in contiguous TCMalloc pages, or
    `>=128 large` for all spans of 128 or more pages.
*   **spans:** The number of free mapped and unmapped spans of that length.
*   **MiB and cum:** The total and cumulative memory covered by those spans.
*   **unmapped and cum:** The unmapped and cumulative unmapped memory for spans
    of that length and smaller.

### Per Component Information

The hugepage-aware allocator divides its managed address space across four
components described in the [Temeraire design doc](temeraire.md):

*   **filler:** Packs allocations smaller than a hugepage into backed hugepages.
*   **region:** Best-fits medium multi-page allocations of 1 MiB or more into 1
    GiB slabs.
*   **cache:** Caches whole backed hugepages ready for reuse.
*   **alloc:** Tracks unbacked hugepage address ranges requested from the OS.

```
Huge page aware allocator components:
------------------------------------------------
HugePageAware: breakdown of used / free / unmapped space:
HugePageAware: filler  38825.2 MiB used,  938.8 MiB free,    0.0 MiB unmapped
HugePageAware: region      0.0 MiB used,    0.0 MiB free,    0.0 MiB unmapped
HugePageAware: cache     908.0 MiB used,    0.0 MiB free,    0.0 MiB unmapped
HugePageAware: alloc       0.0 MiB used,    0.0 MiB free, 1154.0 MiB unmapped

HugePageAware: filler donations 120 (384 pages from abandoned donations)
HugePageAware: filler reused broken hugepages 0
```

The component table reports the MiB allocated to the application or upper
caches, free backed MiB available in that component, and unmapped MiB released
to the OS for each component, followed by two counters:

*   **filler donations:** Cumulative number of tail hugepages from
    multi-hugepage allocations donated to the filler so their unused tail pages
    can serve small allocations. The trailing count reports pages from abandoned
    donations where the original large allocation was freed before the donated
    hugepage became empty.
*   **filler reused broken hugepages:** Cumulative number of previously
    subreleased hugepages whose unmapped pages were later reallocated.

### Filler Cache

`HugePageFiller` packs allocations of one or more TCMalloc pages into hugepages.
Its output is organized into three parts.

The first part summarizes hugepage counts, subrelease totals, and background
hugepage collapse and subrelease activity:

```
HugePageFiller: densely pack small requests into hugepages
HugePageFiller: Overall, 19882 total, 8083 full, 11799 partial, 0 released (0 partially), 0 quarantined
HugePageFiller: those with sparsely-accessed spans, 14200 total, 5100 full, 9100 partial, 0 released (0 partially), 0 quarantined
HugePageFiller: those with densely-accessed spans, 5682 total, 2983 full, 2699 partial, 0 released (0 partially), 0 quarantined
HugePageFiller: 120168 pages free in 19882 hugepages, 0.0236 free
HugePageFiller: among non-fulls, 0.0398 free
HugePageFiller: 499 used pages in subreleased hugepages (0 of them in partially released)
HugePageFiller: 0 hugepages partially released, 0.0000 released
HugePageFiller: 1.0000 of used pages hugepageable
HugePageFiller: Since startup, 26159 pages subreleased, 345 hugepages broken, (0 pages, 0 hugepages due to reaching tcmalloc limit)
HugePageFiller: Out of 120 eligible hugepages, 95 were attempted, and 90 were collapsed (4 donated).
HugePageFiller: Of the failed collapse operations, number of operations that failed per error type, None: 0, Invalid: 0, NoMem: 0, Busy: 5, Again: 0,Unknown: 0
HugePageFiller: Latency of collapse operations: 14.250000 ms (total), 410.500000 us (maximum)
HugePageFiller: Backoff delay for collapse currently is 1 interval(s), number of intervals skipped due to backoff is 0
HugePageFiller: In the previous treatment interval, subreleased 0 pages.
HugePageFiller: In the previous treatment interval, subreleased 0 stale pages.
HugePageFiller: In the previous treatment interval, marked 0 unbacked pages as subreleased. Since startup, 0.
```

*   **Hugepage counts:** Hugepages in the filler are reported overall and
    segregated into sparsely-accessed and densely-accessed span groups. For each
    group, TCMalloc reports the total hugepages, full hugepages with no free
    TCMalloc pages, partial hugepages that are fully backed with some free
    pages, and released hugepages that have been partially unmapped via
    subrelease. The partially released count tracks hugepages where free pages
    were eagerly released on deallocation, while the quarantined count is
    always 0.
*   **Free and subreleased ratios:** The next lines report the total free backed
    TCMalloc pages in the filler and their fraction of all filler capacity, the
    free fraction across non-full hugepages, the live and unmapped pages on
    subreleased hugepages, and the fraction of used filler pages residing on
    fully backed hugepages.
*   **Cumulative subrelease:** The `Since startup` line reports total TCMalloc
    pages subreleased and intact hugepages broken since startup, along with how
    many occurred to satisfy a soft or hard memory limit.
*   **Collapse and treatment statistics:** Tracks background userspace hugepage
    collapse via `MADV_COLLAPSE`, including eligibility, attempts, successes,
    collapsed donated hugepages, per-error-type failure counts, total and
    maximum collapse latency, adaptive backoff delay, and pages subreleased
    during the most recent background treatment interval.

The second part prints fullness histograms across seven tracker categories:

*   **sparsely-accessed regular and densely-accessed regular:** Fully backed
    hugepages managed normally by the filler.
*   **donated:** Tail hugepages donated from multi-hugepage allocations. Donated
    hugepages start with a single tail allocation and transition into regular
    hugepages once additional allocations are placed on them, so only the free
    page count histogram is shown for this category.
*   **sparsely-accessed partial released and densely-accessed partial
    released:** Subreleased hugepages where free pages are eagerly released.
*   **sparsely-accessed released and densely-accessed released:** Subreleased
    hugepages from which free pages have been returned to the OS and which are
    reused for new allocations only as a last resort.

```
HugePageFiller: fullness histograms

HugePageFiller: # of sparsely-accessed regular hps with a<= # of free pages <b
HugePageFiller: <  0<=  5100 <  1<=     6 <  2<=     1 <  3<=     1 <  4<=     0 < 16<=   103
HugePageFiller: < 32<=     1 < 48<=     0 < 64<=     3 < 80<=     1 < 96<=     0 <112<=     0
HugePageFiller: <128<=    28 <144<=     0 <160<=     0 <176<=     1 <192<=     0 <208<=     0
HugePageFiller: <224<=     2 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0
...
HugePageFiller: # of sparsely-accessed regular hps with a<= longest free range <b
...
HugePageFiller: # of sparsely-accessed regular hps with a<= # of allocations <b
...
HugePageFiller: # of sparsely-accessed regular hps with live lifetime a <= # hps < b
...
```

Each bucket `< a<= count` reports how many hugepages in that category have a
value at least `a` and less than the next bucket's lower bound:

*   **# of free pages:** Available TCMalloc pages in the hugepage.
*   **longest free range:** Longest contiguous run of free TCMalloc pages.
*   **# of allocations:** Number of live allocations on the hugepage, starting
    at 1 because empty hugepages leave the filler.
*   **Live lifetime histograms:** Lifetime distributions in milliseconds and
    allocation counts for long-lived or low-occupancy hugepages.
*   **Native page-flag histograms and summaries:** For each tracker category,
    TCMalloc reads `/proc/self/pageflags` when available to report histograms
    and totals of unbacked, swapped, and stale native pages, the number of
    hugepages backed by kernel transparent hugepages, free and used TCMalloc
    pages on non-hugepage-backed versus hugepage-backed trackers, treatment and
    collapse skip counts, and sampled tracker snapshots.

After the seven per-category blocks, the filler prints:

*   The number of previously released hugepages that subsequently became full,
    and how many of them are currently hugepage backed.
*   Completed lifetime histograms for densely-accessed and sparsely-accessed
    hugepages.
*   A broken-hugepage histogram showing how many TCMalloc pages were free in a
    hugepage at the moment it was first broken by subrelease.

The third part summarizes the filler's demand and skip-subrelease time series
over a 5-minute window and subrelease totals over the last 10 minutes:

```
HugePageFiller: time series over 5 min interval

HugePageFiller: realized fragmentation: 0.0 MiB
HugePageFiller: minimum free pages: 0 (0 backed)
HugePageFiller: at peak demand: 1774 pages (and 261 free, 13 unmapped)

HugePageFiller: Since the start of the execution, 0 subreleases (0 pages) were skipped due to either recent (0s) peaks, or the sum of short-term (60s) fluctuations and long-term (300s) trends.
HugePageFiller: 100.0000% of decisions confirmed correct, 0 pending (100.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
HugePageFiller: Subrelease stats last 10 min: total 0 pages subreleased.
```

*   **realized fragmentation and minimum free pages:** The minimum number of
    free filler pages and backed free pages observed over the 5-minute window,
    indicating how much backed free memory sat idle for the entire interval.
*   **at peak demand:** The number of used, free backed, and unmapped filler
    pages at the highest demand point within the window.
*   **Skip-subrelease evaluation:** Adaptive hugepage subrelease skips releasing
    pages needed to cover recent demand, computed from the sum of short-term
    demand fluctuations and long-term demand trends and capped by the largest
    demand peak in the 10-minute history. Decisions are evaluated 5 minutes
    later to verify whether demand rose again, followed by the total pages
    subreleased over the last 10 minutes.

### Region Cache

`HugeRegionSet` manages 1 GiB regions used to best-fit medium-sized allocations
of 1 MiB and larger that cross hugepage boundaries without rounding up to whole
hugepages:

```
HugeRegionSet: 1 MiB+ allocations best-fit into 1024 MiB slabs
HugeRegionSet: 1 total regions
HugeRegion: 12288 KiB used, 5120 KiB free, 4096 KiB contiguous space, 1007 MiB unbacked, 1420 MiB unbacked lifetime
HugeRegionSet: 17 hugepages backed, 0 backed and free, 0 low water mark free backed, out of 512 total
HugeRegionSet: 640 pages free in backed region, 0.1471 free
HugeRegionSet: recent usage range: 0 min - 0 curr - 0 max MiB
HugeRegionSet: recent free backed range: 0 min - 0 curr - 0 max MiB
```

*   **Total regions:** The first two lines report the slab size in MiB and the
    number of active regions.
*   **HugeRegion:** Each active region prints a line reporting the used KiB,
    free KiB inside backed hugepages, longest contiguous free space in KiB,
    currently unbacked MiB, and cumulative unbacked MiB released from that
    region over its lifetime.
*   **Aggregate region occupancy:** Lines four and five report the total backed
    hugepages, completely free backed hugepages, low-water-mark free backed
    hugepages, and total hugepage capacity across all regions, followed by the
    number and fraction of free TCMalloc pages within the backed portion of the
    regions.
*   **Recent usage and free backed ranges:** The minimum, current, and maximum
    backed in-use memory and free backed memory in MiB across the regions over
    the recent 10-minute window.

### Huge Cache

`HugeCache` holds recently freed, backed single hugepages for fast reuse and
dynamically adjusts its cache limit based on demand fluctuations over its cache
window:

```
HugeCache: contains unused, backed hugepage(s) (cache_time = 1000ms)
HugeCache: 0 / 10 hugepages cached / cache limit (0.053 hit rate, 0.436 overflow rate)
HugeCache: 88880 MiB fast unbacked, 6814 MiB periodic
HugeCache: recent usage range: 40672 min - 40672 curr -  40672 max MiB
HugeCache: recent offpeak range: 0 min - 0 curr - 0 max MiB
HugeCache: recent cache range: 0 min - 0 curr - 0 max MiB
HugeCache: epoch length 1000ms, number of records 600
HugeCache: Usage timeseries 0:1000:0:0,24:1000:40672:40672
```

*   **Cached / cache limit:** Number of backed hugepages currently cached versus
    the dynamic limit, along with the allocation hit rate and overflow rate.
*   **Fast / periodic unbacked:** Cumulative MiB unbacked immediately when
    exceeding the cache limit versus unbacked by periodic background shrinking.
*   **Recent usage, offpeak, and cache ranges:** Minimum, current, and maximum
    MiB over the recent cache window for total hugepage usage drawn through the
    cache, off-peak usage below the recent maximum, and cached hugepage bytes.
*   **Usage timeseries:** Non-empty epochs from the 600-slot ring buffer,
    formatted as
    `<epoch_offset>:<duration_ms>:<min_free_backed_mib>:<max_free_backed_mib>`.

### Huge Allocator

`HugeAllocator` manages unbacked virtual address space in hugepage multiples,
using a treap to coalesce and split contiguous unbacked ranges:

```
HugeAllocator: contiguous, unbacked hugepage(s)
HugeAddressMap: treap 5 / 10 nodes used / created
HugeAddressMap: 256 contiguous hugepages available
HugeAllocator: 20913 requested - 20336 in use = 577 hugepages free
```

*   **treap nodes used / created:** Active and total allocated treap nodes
    tracking free unbacked address ranges.
*   **contiguous hugepages available:** Size of the largest contiguous unbacked
    hugepage range currently available in the treap.
*   **requested - in use = hugepages free:** Cumulative hugepages requested from
    the system allocator, hugepages currently in use by upper components, and
    unbacked hugepages free in the huge allocator.

### Pageheap Summary Information

At the end of each page allocator section, TCMalloc reports summary statistics
on page-level allocations:

```
HugePageAware: stats on allocation sizes
HugePageAware: 4969003 pages live small allocation
HugePageAware: 659 pages of slack on large allocations
HugePageAware: largest seen allocation 45839 pages
```

*   **pages live small allocation:** Total live TCMalloc pages in allocations
    smaller than 128 TCMalloc pages.
*   **pages of slack on large allocations:** Unused trailing TCMalloc pages on
    large allocations that were rounded up to whole hugepages.
*   **largest seen allocation:** Largest single allocation request in TCMalloc
    pages seen since startup.

### Per Size Range Info:

Next, the per-size breakdown reports span allocations by page count, using exact
page counts from 1 to 128 pages and power-of-two page ranges for larger
allocations, followed by per-heap page release totals and parameters:

```
HugePageAware: per-size information:
HugePageAware:                     1 page info:    5817510 /    3863506 a/f,  1954004 (15265.7 MiB) live,   16.000 allocs/s (   0.1 MiB/s)
HugePageAware:                     2 page info:    1828473 /    1254096 a/f,   574377 ( 8974.6 MiB) live,    5.030 allocs/s (   0.1 MiB/s)
HugePageAware:                     3 page info:    1464568 /    1227253 a/f,   237315 ( 5562.1 MiB) live,    4.030 allocs/s (   0.1 MiB/s)
...
HugePageAware: [     129 ,     256 ] page info:       1240 /       1190 a/f,       50 (   82.5 MiB) live,    0.003 allocs/s (   0.0 MiB/s)
HugePageAware: 26159 pages ( 204.4 MiB) released in total
HugePageAware: 0 pages (   0.0 MiB) released from ReleaseMemoryToSystem
HugePageAware: 26159 pages ( 204.4 MiB) released from ProcessBackgroundActions
HugePageAware: 0 pages (   0.0 MiB) released from soft malloc limit hits
HugePageAware: 0 pages (   0.0 MiB) released from hard malloc limit hits
PARAMETER use_huge_region_more_often 1
PARAMETER hpaa_subrelease 1
```

For each non-zero size row:

*   **page info:** The span size in TCMalloc pages, or `[ min , max ]` page
    range for allocations above 128 pages.
*   **a/f:** Cumulative number of span allocations and deallocations in that
    bucket.
*   **live:** Currently live spans and their total live size in MiB.
*   **allocs/s and MiB/s:** Span allocation rate and byte allocation rate since
    process startup.
*   **Released pages and parameters:** Cumulative TCMalloc pages and MiB
    released from this page allocator partition in total and by trigger,
    followed by whether huge regions and adaptive subrelease are enabled on the
    partition.

### GWP-ASan Status

The GWP-ASan section displays telemetry for allocations guarded by
[GWP-ASan](gwp-asan.md):

*   **Successful / Failed Allocations:** Sampled allocations that were
    successfully placed in a guarded slot versus failed after passing
    eligibility checks. If both are 0, GWP-ASan is likely disabled or no guarded
    sample has triggered yet. See
    [GWP-ASan sampling rate](gwp-asan.md#what-should-i-set-the-sampling-rate-to).
*   **Skipped Allocations:** Sampled allocations that bypassed GWP-ASan because
    all guarded slots were in use, because the allocation's memory tag or
    alignment was filtered out, or because the requested size exceeded a single
    guarded page.
*   **Currently Allocated / Allocated High-Watermark:** Number of guarded slots
    currently holding live allocations and the peak concurrent guarded slots
    used since startup, out of the maximum allowed concurrent slots.
*   **Object Pages Touched:** Number of distinct guarded slots that have been
    allocated at least once since startup, out of the total guarded page pool.
*   **Currently Quarantined:** Number of slots in the guarded page pool not
    currently holding a live allocation, whether quarantined after free or not
    yet used.
*   **Guard Region Fallbacks:** Number of times `mprotect` fallback was required
    when arming or disarming guard pages.
*   **tcmalloc_guarded_sample_parameter:** The configured GWP-ASan chained
    sampling interval, selecting roughly 1 in N sampled allocations.

```
------------------------------------------------
GWP-ASan Status
------------------------------------------------
Successful Allocations: 1823
Failed Allocations: 0
Skipped Allocations (No Slots): 0
Skipped Allocations (Filtered): 0
Skipped Allocations (Too Large): 0
Currently Allocated: 33 / 64
Allocated High-Watermark: 51 / 64
Object Pages Touched: 55 / 128
Currently Quarantined: 95
Guard Region Fallbacks: 0
PARAMETER tcmalloc_guarded_sample_parameter 50
```

### Configured Limits and Parameters

After the GWP-ASan section, TCMalloc prints configured heap limits, aggregate
page release counters, and active runtime parameters:

```
------------------------------------------------
Configured limits and related statistics
------------------------------------------------
PARAMETER desired_usage_limit_bytes 18446744073709551615
PARAMETER hard_usage_limit_bytes 18446744073709551615
Number of times soft limit was hit: 0
Number of times hard limit was hit: 0
Number of times memory shrank below soft limit: 0
Number of times memory shrank below hard limit: 0
Total number of pages released: 26159 (  204.4 MiB)
Number of pages released by ReleaseMemoryToSystem: 0 (    0.0 MiB)
Number of pages released by ProcessBackgroundActions: 26159 (  204.4 MiB)
Number of pages released after soft limit hits: 0 (    0.0 MiB)
Number of pages released after hard limit hits: 0 (    0.0 MiB)
Number of active partitions: 1
------------------------------------------------
Parameters
------------------------------------------------
PARAMETER tcmalloc_per_cpu_caches 1
PARAMETER tcmalloc_max_per_cpu_cache_size 1572864
PARAMETER tcmalloc_max_total_thread_cache_bytes 33554432
PARAMETER malloc_release_bytes_per_sec 5242880
PARAMETER tcmalloc_skip_subrelease_short_interval 1m
PARAMETER tcmalloc_skip_subrelease_long_interval 5m
PARAMETER tcmalloc_skip_subrelease_cold_short_interval 1m
PARAMETER tcmalloc_skip_subrelease_cold_long_interval 5m
PARAMETER tcmalloc_release_partial_alloc_pages 1
PARAMETER tcmalloc_release_pages_from_huge_region 1
PARAMETER tcmalloc_huge_region_adaptive_release 1
PARAMETER madvise_cold_regions_nohugepage 0
PARAMETER tcmalloc_release_max_filler_pages 0
PARAMETER tcmalloc_track_broken_hugepages 1
PARAMETER tcmalloc_preserve_donated_on_collapse 0
PARAMETER tcmalloc_collapse_released_hugepages 0
PARAMETER tcmalloc_madvise_sampled_allocations 1
PARAMETER tcmalloc_use_wider_slabs 1
PARAMETER heap_partitioning 0
PARAMETER size_class_config POW2_BELOW_64
PARAMETER percpu_vcpu_type RSEQ_VCPU
PARAMETER madvise MADVISE_DONTNEED
PARAMETER tcmalloc_resize_size_class_max_capacity 1
PARAMETER tcmalloc_dense_trackers_sorted_on_spans_allocated 1
PARAMETER min_hot_access_hint 2
PARAMETER tcmalloc_usermode_hugepage_collapse 0
PARAMETER tcmalloc_subrelease_unbacked_hugepages 0
PARAMETER tcmalloc_back_small_allocations 0
PARAMETER tcmalloc_back_size_threshold_bytes 2097152
PARAMETER background_process_sleep_interval_ns 1000000000
PARAMETER tcmalloc_cpu_cache_touched 16
PARAMETER max_tcmalloc_cpu_cache_touched 31
PARAMETER tcmalloc_enable_unfiltered_collapse 0
PARAMETER tcmalloc_release_stale_pages 0
PARAMETER tcmalloc_release_drained_slab_metadata 0
PARAMETER tcmalloc_cfl_subbucket_prioritization 0
```

*   **Configured limits and related statistics:** Shows the configured soft and
    hard usage limits in bytes, the number of times each limit was exceeded and
    how often TCMalloc reclaimed enough pages to drop back below the limit, the
    total pages released across all page allocator partitions by trigger, and
    the number of active heap partitions.
*   **Parameters:** Reports current runtime tuning parameters, including cache
    size caps, background release rate, skip-subrelease windows, subrelease and
    userspace collapse settings, size-class table configuration, per-CPU rseq
    mode, and the count and highest index of per-CPU caches touched on the slow
    path during the most recent background interval.

### Memory Requested From The OS

Finally, the low-level allocator section reports system-level memory release
errors and the total virtual address space reserved from the OS via `mmap`:

```
Low-level allocator stats:
Memory Release Failures: 0
MmapSysAllocator: 18083741696 bytes (17246.0 MiB) reserved
```

*   **Memory Release Failures:** Cumulative number of failed `madvise` calls
    when attempting to release physical pages back to the OS.
*   **MmapSysAllocator:** Total bytes of virtual address space reserved from the
    OS by TCMalloc's system allocator. Because released pages remain reserved in
    virtual address space, this value tracks TCMalloc's virtual address space
    footprint rather than physical RSS.
