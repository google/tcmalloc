# TCMalloc Latency Injection

**NOTE:** This is an experimental feature for latency injection and is planned
to be removed in the future (see b/29448043).

TCMalloc provides an internal mechanism to inject delays into critical
allocation paths to simulate locking contentions and overhead. This allows
experiments to observe the impact of increased latency at various levels of the
allocator.

## Using the Latency Injection Build

To enable this feature, your target needs to link against the latency injection
variant of TCMalloc. In your `BUILD` file or where you select the TCMalloc
variant, use the target: `//third_party/tcmalloc:tcmalloc_latency_injection`

## Flags

Once built with the latency injection variant, the following flags can be
controlled dynamically at runtime:

-   `tcmalloc_page_heap_delay`: Controls the delay injected when acquiring the
    page heap lock.
-   `tcmalloc_central_freelist_delay`: Controls the delay injected when
    acquiring a central freelist lock.
-   `tcmalloc_update_max_capacities_delay`: Controls the delay injected during
    `UpdateMaxCapacities` in the per-CPU cache.
-   `tcmalloc_resize_slabs_delay`: Controls the delay injected during
    `ResizeSlabs` in the per-CPU cache.

All of these flags take an `absl::Duration` formatted string (e.g., `"10us"`,
`"1ms"`).

## Example

```
--tcmalloc_page_heap_delay=500ns \
--tcmalloc_central_freelist_delay=200ns
```

This will inject 500 nanoseconds of spin-wait time whenever the page heap lock
is acquired, and 200 nanoseconds when a central freelist lock is acquired.
