
# Lightweight UB Checks

<!--*
# Document freshness: For more information, see go/fresh-source.
freshness: { owner: 'ckennelly' reviewed: '2025-12-23' }
*-->

TCMalloc is optimized for performance, but it also adds a number of lightweight
checks for undefined behavior (UB) as part of its implementation. These
supplement [GWP-ASan](gwp-asan.md), which checks for additional classes of UB
such as use-after-free, underflow, and overflow. Imposing detailed checks is
costly; the lightweight checks can run on every (de)allocation, allowing better
detection of problems.

## Deallocation Information and Failures

TCMalloc has a few pieces of information that it can cross-check with its own
metadata:

*   The deallocated pointer itself (`ptr`).
*   If present (sized `delete` and `free_sized`), the `size` argument.
*   The type of deallocation (`free` versus `delete`)
*   If present, the `alignment` argument.
*   Whether the object is [sampled](sampling.md).

We use these to identify several types of UB:

*   Invalid frees: These are deallocations that could not have ever been
    correctly allocated (the result of `malloc` or `new`). These are typically
    for address ranges that are not managed by TCMalloc or where least
    significant bits are non-zero due to misalignment. TCMalloc-allocated memory
    is almost always aligned to `kAlignment` (as of December 2025, this is 8).

*   Mismatched frees: These are mismatches for `malloc` deallocated with
    `delete`, `new` deleted with `free`, aligned `new` deallocated with
    unaligned `delete`, and so on.

*   Mismatched sizes: These are mismatches where TCMalloc detected a mismatch
    between the caller provided size and its own metadata.

These checks are exercised in
google3/tcmalloc/testing/memory_errors_fuzz.cc and
google3/tcmalloc/testing/memory_errors_test.cc

## Flowchart

Checks are imposed at various points in the deallocation lifecycle,
strategically using other required lookups to minimize their cost.

TCMalloc classifies objects as "normal" and non-normal (sampled, cold, etc.).
Most objects allocated are normal, so the fast path is heavily biased towards
this common case. We apply additional scrutiny on the non-normal deallocation
path.

*   Check for implausible set pointer bits (`do_free` and `do_free_with_size`
    with `kNormalOrBadDeallocationMask`). This checks all pointers for values
    where the most significant bits are set (could never be allocated by
    TCMalloc) or where the least significant bits are set. The least significant
    bit may only be set for a fraction of GWP-ASan managed allocations that are
    "right-padded" to look for overflows.

    This overloads an existing check for `nullptr` and `IsNormalMemory`
    respectively to avoid additional conditionals. Failures are handled by
    `do_unsized_free_irregular` and `free_non_normal`.

*   Unsized deallocations treat empty page metadata as having `sizeclass=0` and
    handle this as if it were a large deallocation, invoking
    `InvokeHooksAndFreePages`.

*   Non-normal/non-cold allocations handled by `InvokeHooksAndFreePages`. These
    deallocations are for objects that are large (either in practice or
    according to the proported `size` argument) or sampled.

    If TCMalloc's metadata is absent (`span == nullptr`), the object never
    existed.

    If TCMalloc's metadata matches `span == &tc_globals.invalid_span()`, the
    object was previously deallocated but the address has not been reused
    (indicating a possible double-free).

    Large, unsampled objects provide the least amount of metadata. For these, we
    check:

    *   In `MaybeUnsampleAllocation`, we can bound the size with the `Span`
        size, so within `kPageSize` bytes.
    *   In `InvokeHooksAndFreePages`, we can confirm the least significant bits
        are 0 such that the pointer is aligned to `kPageSize` bytes.

    Sampled objects objects have more information. For these, we can check:

    *   The exact value of `size` the memory was allocated with, or a range of
        sizes if using `__size_returning_new`.
    *   The type of allocation versus the type of deallocation (`malloc-free`,
        `new-delete`).
    *   The alignment parameter matches exactly (present/absent and value) from
        when it was allocated.

*   These checks can have false negatives by design. Our common case is sized
    deallocations of "normal" pointers and these have minimal validation.

    We apply a final line of checks in `MapObjectsToSpans` to validate that
    objects have been placed in their correct size class. The failures here are
    very distant from the actual deallocation site and may only be noticed when
    TCMalloc's background thread moves objects between caches and so on.
