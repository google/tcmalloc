# Mismatched Sized Delete

[Chris Kennelly](ckennelly@google.com)

<!--*
# Document freshness: For more information, see go/fresh-source.
freshness: { owner: 'ckennelly' reviewed: '2024-12-05' }
*-->

TCMalloc or Address Sanitizer told me there was a mismatch in sized delete's
argument? What does this mean?

## What does the error look like?

TCMalloc checks purportedly large and [sampled object](sampling.md)
deallocations. When it detects an erroneous size argument, it reports it:

```
2024-11-23 04:02:02.316406-0800480   387 tcmalloc/allocation_sampling.cc:339] *** GWP-ASan (https://google.github.io/tcmalloc/gwp-asan.html) has detected a memory error ***
2024-11-23 04:02:02.316420-0800480   387 tcmalloc/allocation_sampling.cc:344] Mismatched-size-delete of 92342141241600 bytes (expected at most 8192 bytes) for 0x53fc22595b00 at:
```

Address Sanitizer's checks apply to all deallocations. Its errors look like:

```
==23968==ERROR: AddressSanitizer: new-delete-type-mismatch on 0x611001413140 in thread T0:
  object passed to delete has wrong type:
  size of the allocated type:   216 bytes;
  size of the deallocated type: 24 bytes.
```

For small objects, it's too expensive to validate the size argument at the time
of deallocation. Instead, tcmalloc validates the sizes only at some later point
when objects are being transferred back to tcmalloc's central freelist. In these
cases, the errors look like:

```
Mismatched-size-class (https://github.com/google/tcmalloc/tree/master/docs/mismatched-sized-delete.md) discovered for pointer 0x4e9f7fa89440: this pointer was recently freed with a size argument in the range [1, 8], but the associated span of allocated memory is for allocations with sizes [9, 16].
```

## TCMalloc is buggy?

TIP: Memory safety bugs can have a delayed effect. The checks described above,
and other related crashes, often occur in a different call stack than the one
that caused the bug. Before scrutinizing the call stack attached to this crash,
start by looking for related crashes of the same process.

It is not a bug in TCMalloc. It is detecting an erroneous argument provided to
`::operator delete`.

The typical failure modes are caused by a memory safety bug (a buffer overrun, a
double-free, etc.) or a bitflip corrupting the size stored with a
variable-length structure like a `std::vector` or `std::string`.

Less common failure modes since they are typically detected by presubmits and
fixed before check-in include:

*   You're deleting an object through a pointer to its base class, but you don't
    have a virtual destructor.

    ```
    class Base {
      ...
    };

    class Derived : public Base {
      ...
      ~Derived();  // note--no virtual!
      // or no explicit destructor definition at all...
    }

    Derived *d = new Derived;
    Base *b = static_cast<Base *>(d);
    ...
    delete b;
    ```

    This is undefined behavior by the C++ standard, and a particularly bad idea
    in practice, because Derived's destructor will never be called. Even if you
    don't have explicit code to be called in a destructor, Derived's members
    will not be destroyed; if it contains a string or vector, you will leak
    memory. Thankfully, there is an extremely easy fix:

    ```
    class Base {
      virtual ~Base();
    };
    ```

    This is required by the style guide if your hierarchy has any virtual
    methods. If you don't, it's probably a bad idea to be storing Derived
    objects in Base pointers anyway.

*   You're deleting an object created in a too-large block by placement new

    This case is more subtle (and rarer). Your code probably has something like
    this:

    ```
    class Object {
      ...
      int array[0];  // flexible array member
    };

    // allocate an Object with space for 10 members of array[].
    void *storage = ::operator new(   // or malloc...
        sizeof(Object) + sizeof(int) * 10));

    Object *o = new (storage) Object;
    ...
    delete o;
    ```

    This can be fixed in one of two ways. Use destroying operator delete or
    suppress sized delete.

    ```
    class Object {
      static void operator delete(Object* p, std::destroying_delete_t) {
        size_t size = p->elements;
        p->~Object();
        ::operator delete(p, sizeof(Object) + sizeof(int) * size);
      }

      int elements;
      int array[0];
    }
    ```

    ```
    class Object {
      void operator delete(void* ptr) { ::operator delete(ptr); }
    }
    ```

## What types of errors can we detect?

Address Sanitizer catches mismatches for all sized deallocations.

TCMalloc detects:

*   Sampled object deallocations: These cover any size and any error.
    Size-returning allocations allow
    [some slack](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p0901r11.html#sizeddelete).

    Since these objects were sampled at allocation time, we have a callstack for
    the allocation.

*   Large object sizes (`>kMaxSize`, typically 256KB): When a deallocation is
    for a large number of bytes, we expect to deallocate that object to a single
    object [span](design.md#spans). The deallocated object should be no larger
    than the span. It cannot be any smaller than the size less a
    [TCMalloc page](design.md#pagesize) (typically 8KB), since we would have
    otherwise allocated a smaller span.

*   Other object sizes, but only at the point when the object is returned to the
    central freelist. In these cases, the stack trace of the crash is likely
    unrelated to the root cause of the bug but prevents further downstream
    corruption from accumulating after an earlier corruption of state.

## Is aborting the right behavior?

Yes.

Consider a small (8 byte) object that is deallocated with "256KB+8 bytes" as the
size argument. Purportedly large deallocations cause the owning span to be
immediately freed to the page heap, even though other 1023 objects from that
Span may be live. This memory might be wiped if the page is returned to the
operating system with `madvise(MADV_DONTNEED)` or it might be reused to allocate
another object. These can cause further memory corruption.

Stopping the program immediately noisily helps to narrow the time window between
the corruption event and the program crashing as a result of it (due to a wild
pointer dereference, etc.).

While we successfully detected the corruption for the deallocation in question
and could try to recover within TCMalloc (consulting extra bookkeeping to get
the "true" state, etc.), we cannot validate every deallocation in this way,
since we use sized delete as a performance optimization to ordinarily *avoid*
those extra lookups.
