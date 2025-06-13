#![no_std]
//! `tcmalloc_rs` provides bindings for [`google/tcmalloc`](https://github.com/google/tcmalloc) to make it usable as a global allocator for rust.
//!
//! To use `tcmalloc_rs` add it as a bazel dependency:
//! ```Starlark
//! //tcmalloc_rs
//! ```
//!
//! To set `TCMalloc` as the global allocator add this to your project:
//! ```rust
//! use tcmalloc_rs;
//!
//! #[global_allocator]
//! static GLOBAL: tcmalloc_rs::TCMalloc = tcmalloc_rs::TCMalloc;
//! ```


use core::{
    alloc::{GlobalAlloc, Layout},
    ptr::NonNull,
};
use tcmalloc_sys;

#[derive(Debug, Copy, Clone)]
#[repr(C)]
pub struct TCMalloc;

unsafe impl Send for TCMalloc {}
unsafe impl Sync for TCMalloc {}

impl TCMalloc {
    #[inline(always)]
    pub const fn new() -> Self {
        Self
    }

    /// Returns the available bytes in a memory block.
    #[inline(always)]
    pub fn usable_size<U>(&self, ptr: *const u8) -> Option<usize> {
        match ptr.is_null() {
            true => None,
            false => Some(unsafe { tcmalloc_sys::malloc_usable_size(ptr.cast::<U>() as *mut _) })
        }
    }

    /// Allocates memory with the given layout, returning a non-null pointer on success
    #[inline(always)]
    pub fn alloc_aligned(&self, layout: Layout) -> Option<NonNull<u8>> {
        match layout.size() {
            0 => NonNull::new(layout.align() as *mut u8),
            _ => NonNull::new(unsafe { tcmalloc_sys::malloc(layout.size()) }.cast())
        }
    }
}

unsafe impl GlobalAlloc for TCMalloc {
    /// Allocate the memory with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// On failure, it returns a null pointer.
    /// The client must assure the following things:
    /// - `alignment` is greater than zero
    /// - Other constrains are the same as the rust standard library.
    ///
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        match layout.size() {
            0 => layout.align() as *mut u8,
            _ => tcmalloc_sys::malloc(layout.size()).cast()
        }
    }

    /// De-allocate the memory at the given address with the given alignment and size.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position.
    /// - Other constrains are the same as the rust standard library.
    ///
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.size() != 0 {
            tcmalloc_sys::free(ptr as *mut _)
        }
    }

    /// Behaves like alloc, but also ensures that the contents are set to zero before being returned.
    #[inline(always)]
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        match layout.size() {
            0 => layout.align() as *mut u8,
            size => tcmalloc_sys::calloc(layout.align(), size).cast()
        }
    }

    /// Re-allocate the memory at the given address with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// The memory content within the `new_size` will remains the same as previous.
    /// On failure, it returns a null pointer. In this situation, the previous memory is not returned to the allocator.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position
    /// - `alignment` fulfills all the requirements as `rust_alloc`
    /// - Other constrains are the same as the rust standard library.
    ///
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        match new_size {
            0 => {
                self.dealloc(ptr, layout);
                layout.align() as *mut u8
            }
            new_size if layout.size() == 0 => {
                self.alloc(Layout::from_size_align_unchecked(new_size, layout.align()))
            }
            _ => tcmalloc_sys::realloc(ptr.cast(), new_size).cast()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::TCMalloc;
    use core::alloc::{GlobalAlloc, Layout};
    #[test]
    fn allocation_lifecycle() {
        let alloc = TCMalloc::new();
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();

            // Test regular allocation
            let ptr = alloc.alloc(layout);
            alloc.dealloc(ptr, layout);

            // Test zeroed allocation
            let ptr = alloc.alloc_zeroed(layout);
            alloc.dealloc(ptr, layout);

            // Test reallocation
            let ptr = alloc.alloc(layout);
            let ptr = alloc.realloc(ptr, layout, 16);
            alloc.dealloc(ptr, layout);

            // Test large allocation
            let large_layout = Layout::from_size_align(1 << 20, 32).unwrap();
            let ptr = alloc.alloc(large_layout);
            alloc.dealloc(ptr, large_layout);
        }
    }
    #[test]
    fn it_frees_allocated_memory() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = TCMalloc;

            let ptr = alloc.alloc(layout);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn it_frees_zero_allocated_memory() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = TCMalloc;

            let ptr = alloc.alloc_zeroed(layout);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn it_frees_reallocated_memory() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = TCMalloc;

            let ptr = alloc.alloc(layout);
            let ptr = alloc.realloc(ptr, layout, 16);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn it_frees_large_alloc() {
        unsafe {
            let layout = Layout::from_size_align(1 << 20, 32).unwrap();
            let alloc = TCMalloc;

            let ptr = alloc.alloc(layout);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn test_usable_size() {
        let alloc = TCMalloc::new();
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let ptr = alloc.alloc(layout);
            let usz = alloc.usable_size::<Layout>(ptr).expect("usable_size returned None");
            alloc.dealloc(ptr, layout);
            assert!(usz >= 8);
        }
    }
}