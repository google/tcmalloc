# Restartable Sequence Mechancism for TCMalloc

<!--*
# Document freshness: For more information, see go/fresh-source.
freshness: { owner: 'ckennelly' reviewed: '2020-06-18' }
*-->

## per-CPU Caches

TCMalloc implements its per-CPU caches using restartable sequences (`man
rseq(2)`) on Linux.  This kernel feature was developed by [Paul Turner and
Andrew Hunter at
Google](http://www.linuxplumbersconf.net/2013/ocw//system/presentations/1695/original/LPC%20-%20PerCpu%20Atomics.pdf)
and Mathieu Desnoyers at EfficiOS. Restartable sequences let us execute a region
to completion (atomically with respect to other threads on the same CPU) or to
be aborted if interrupted by the kernel by preemption, interrupts, or signal
handling.

Choosing to restart on migration across cores or preemption allows us to
optimize the common case - we stay on the same core - by avoiding atomics, over
the more rare case - we are actually preempted.  As a consequence of this
tradeoff, we need to make our code paths actually support being restarted.  The
entire sequence, except for its final store to memory which *commits* the
change, must be capable of starting over.

This carries a few implementation challenges:

*   We need fine-grained control over the generated assembly, to ensure stores
    are not reordered in unsuitable ways.
*   The restart sequence is triggered if the kernel detects a context switch
    occurred with the PC in the restartable sequence code. If this happens
    instead of restarting at this PC, it restarts the thread at an abort
    sequence, the abort sequence determines the interrupted restartable
    sequence, and then returns to control to the entry point of this sequence.

    We must preserve adequate state to successfully restart the code sequence.
    In particular, we must preserve the function parameters so that we can
    restart the sequence with the same conditions; next we must reload any
    parameters like the CPU ID, and recompute any necessary values.

## Structure of the `TcmallocSlab`

In per-CPU mode, we allocate an array of `N` `TcmallocSlab::Slabs`.  For all
operations, we index into the array with the logical CPU ID.

Each slab is has a header region of control data (one 8-byte header per-size
class).  These index into the remainder of the slab, which contains pointers to
free listed objects.

![Memory layout of per-cpu data structures](images/per-cpu-cache-internals.png "Memory layout of per-cpu data structures")

In [C++
code](https://github.com/google/tcmalloc/blob/master/tcmalloc/internal/percpu_tcmalloc.h),
these are represented as:

```
struct Slabs {
  std::atomic<int64_t> header[NumClasses];
  void* mem[((1ul << Shift) - sizeof(header)) / sizeof(void*)];
};

// Slab header (packed, atomically updated 64-bit).
struct Header {
  // All values are word offsets from per-CPU region start.
  // The array is [begin, end).
  uint16_t current;
  // Copy of end. Updated by Shrink/Grow, but is not overwritten by Drain.
  uint16_t end_copy;
  // Lock updates only begin and end with a 32-bit write.
  uint16_t begin;
  uint16_t end;

  // Lock is used by Drain to stop concurrent mutations of the Header.
  // Lock sets begin to 0xffff and end to 0, which makes Push and Pop fail
  // regardless of current value.
  bool IsLocked() const;
  void Lock();
};

```

The atomic `header` allows us to read the state (esp. for telemetry purposes) of
a core without undefined behavior.

The fields in `Header` are indexed in `sizeof(void*)` strides into the slab.
For the default value of `Shift=18`, this allows us to cache nearly 32K objects
per CPU.

We have allocated capacity for `end-begin` objects for a given size class.
`begin` is chosen via static partitioning at initialization time.  `end` is
chosen dynamically at a higher-level (in `tcmalloc::CPUCache`), as to:

* Avoid running into the next size classes' `begin`
* Balance cached object capacity across size classes, according to the specified
  byte limit.

## Usage: Allocation

As the first operation, we can look at allocation, which needs to read the
pointer at index `current-1`, return that object, and decrement `current`.
Decrementing `current` is the *commit* operation.

In psuedo-C++, this looks like:

```
void* TcmallocSlab_Pop(
    void *slabs,
    size_t cl,
    UnderflowHandler f) {
  // Expanded START_RSEQ macro...
restart:
  __rseq_abi.rseq_cs = &__rseq_cs_TcmallocSlab_Pop;
start:
  // Actual sequence
  uint64_t cpu_id = __rseq_abi.cpu_id;
  Header* hdr = &slabs[cpu_id].header[cl];
  uint64_t current = hdr->current;
  uint64_t begin = hdr->begin;
  if (ABSL_PREDICT_FALSE(current <= begin)) {
    goto underflow;
  }

  void* next = *(&slabs[cpu_id] + current * sizeof(void*) - 2 *sizeof(void*))
  prefetcht0(next);

  void* ret = *(&slabs[cpu_id] + current * sizeof(void*) - sizeof(void*));
  current--;
  hdr->current = current;
commit:
  return ret;
underflow:
  return f(cpu_id, cl);
}

// This is implemented in assembly, but for exposition.
ABSL_CONST_INIT kernel_rseq_cs __rseq_cs_TcmallocSlab_Pop = {
  .version = 0,
  .flags = 0,
  .start_ip = &&start,
  .post_commit_offset = &&commit - &&start,
  .abort_ip = &&abort,
};
```

`__rseq_cs_TcmallocSlab_Pop` is a read-only data structure, which contains
metadata about this particular restartable sequence. When the kernel preempts
the current thread, it examines this data structure. If the current instruction
pointer is between `[start, commit)`, it returns control to a specified,
per-sequence restart header at `abort`.

Since the *next* object is frequently allocated soon after the current object,
so the allocation path prefetches the pointed-to object.  To avoid prefetching a
wild address, we populate `slabs[cpu][begin]` for each CPU/size class with a
pointer-to-self.

This sequence terminates with the *single* committing store to `hdr->current`.
If we are migrated or otherwise interrupted, we restart the preparatory steps,
as the values of `cpu_id`, `current`, `begin` may have changed.

As these operations work on a single core's data and are executed on that core.
From a memory ordering perspective, loads and stores need to appear on that core
in program order.

### Restart Handling

The `abort` label is distinct from `restart`.  The `rseq` API provided by the
kernel requires a "signature" (typically an intentionally invalid opcode) in the
4 bytes prior to the restart handler, we form a small trampoline - properly
signed - to jump back to `abort`.

In x86 assembly, this looks like:

```
  // Encode nop with RSEQ_SIGNATURE in its padding.
  .byte 0x0f, 0x1f, 0x05
  .long RSEQ_SIGNATURE
  .local TcmallocSlab_Push_trampoline
  .type TcmallocSlab_Push_trampoline,@function
  TcmallocSlab_Push_trampoline:
abort:
  jmp restart
```

This ensures that the 4 bytes prior to `abort` match up with the signature that
was configured with the `rseq` syscall.

On x86, we can represent this with a nop which would allow for interleaving in
the main implementation.  On other platforms - with fixed width instructions -
the signature is often chosen to be an illegal/trap instruction, so it has to be
disjoint from the function's body.

## Usage:  Deallocation

Deallocation uses two stores, one to store the deallocated object and another to
update `current`.  This is still compatible with the restartable sequence
technique, as there is a *single* commit step, updating `current`.  Any
preempted sequences will overwrite the value of the deallocated object until a
successful sequence commits it by updating `current`.

```
int TcmallocSlab_Push(
    void *slab,
    size_t cl,
    void* item,
    OverflowHandler f) {
  // Expanded START_RSEQ macro...
abort:
  __rseq_abi.rseq_cs = &__rseq_cs_TcmallocSlab_Push;
start:
  // Actual sequence
  uint64_t cpu_id = __rseq_abi.cpu_id;
  Header* hdr = &slabs[cpu_id].header[cl];
  uint64_t current = hdr->current;
  uint64_t end = hdr->end;
  if (ABSL_PREDICT_FALSE(current >= end)) {
    goto overflow;
  }

  *(&slabs[cpu_id] + current * sizeof(void*) - sizeof(void*)) = item;
  current++;
  hdr->current = current;
commit:
  return;
overflow:
  return f(cpu_id, cl, item);
}
```

## Initialization of the Slab

To reduce metadata demands, we lazily initialize the slabs, relying on the
kernel to provide zeroed pages from the `mmap` call to obtain memory for the
slab metadata.

At startup, this leaves the `Header` of each initialized to `current = begin =
end = 0`.  The initial push or pop will trigger the overflow or underflow paths
(respectively), so that we can populate these values.

## More Complex Operations:  Batches

When the cache under or overflows, we populate or remove a full batch of objects
obtained from inner caches.  This amortizes some of the lock acquisition/logic
for those caches.  Using a similar approach to push and pop, we update a batch
of `N` items and we update `current to commit the update.
