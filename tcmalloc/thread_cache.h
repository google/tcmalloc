// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_THREAD_CACHE_H_
#define TCMALLOC_THREAD_CACHE_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <atomic>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu_state.h"
#include "tcmalloc/metadata_object_allocator.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

//-------------------------------------------------------------------
// Data kept per thread
//-------------------------------------------------------------------

class ABSL_CACHELINE_ALIGNED ThreadCache {
 public:
  explicit ThreadCache(pthread_t tid);
  void Cleanup();

  // Allocate an object of the given size class.
  // Returns nullptr when allocation fails.
  [[nodiscard]] void* absl_nullable Allocate(size_t size_class);

  void Deallocate(void* absl_nonnull ptr, size_t size_class);

  static void InitTSD();
  [[nodiscard]] static ThreadCache* absl_nonnull GetCache();
  [[nodiscard]] static ThreadCache* absl_nullable GetCacheIfPresent();
  static void BecomeIdle();

  // Adds to *total_bytes the total number of bytes used by all thread heaps.
  // Also, if class_count is not NULL, it must be an array of size kNumClasses,
  // and this function will increment each element of class_count by the number
  // of items in all thread-local freelists of the corresponding size class.
  [[nodiscard]] static AllocatorStats GetStats(
      uint64_t* absl_nonnull total_bytes, uint64_t* absl_nullable class_count)
      ABSL_LOCKS_EXCLUDED(threadcache_lock_);

  // Sets the total thread cache size to new_size, recomputing the
  // individual thread cache sizes as necessary.
  static void set_overall_thread_cache_size(int64_t new_size)
      ABSL_LOCKS_EXCLUDED(threadcache_lock_);

  [[nodiscard]] static int64_t overall_thread_cache_size()
      ABSL_LOCKS_EXCLUDED(threadcache_lock_) {
    return overall_thread_cache_size_.load(std::memory_order_relaxed);
  }

 private:
  friend void TCMalloc_Internal_DestroyThreadCache(ThreadCache* absl_nullable);

  class FreeList {
   private:
    void* list_ = nullptr;             // Linked list.
    std::atomic<uint32_t> length_{0};  // Current length.
    uint32_t lowater_;                 // Low water mark for list length.
    uint32_t max_length_;  // Dynamic max list length based on usage.
    // Tracks the number of times a deallocation has caused
    // length_ > max_length_.  After the kMaxOverages'th time, max_length_
    // shrinks and length_overages_ is reset to zero.
    uint32_t length_overages_;

   public:
    FreeList() = default;

    // Not copy constructible or movable.
    FreeList(const FreeList&) = delete;
    FreeList(FreeList&&) = delete;
    FreeList& operator=(const FreeList&) = delete;
    FreeList& operator=(FreeList&&) = delete;

    void Init() {
      list_ = nullptr;
      length_.store(0, std::memory_order_relaxed);
      lowater_ = 0;
      max_length_ = 1;
      length_overages_ = 0;
    }

    // Return current length of list
    [[nodiscard]] size_t length() const {
      return length_.load(std::memory_order_relaxed);
    }

    // Is list empty?
    [[nodiscard]] bool empty() const { return list_ == nullptr; }

    // Return the maximum length of the list.
    [[nodiscard]] size_t max_length() const { return max_length_; }

    // Set the maximum length of the list.  If 'new_max' > length(), the
    // client is responsible for removing objects from the list.
    void set_max_length(size_t new_max) { max_length_ = new_max; }

    // Return the number of times that length() has gone over max_length().
    [[nodiscard]] size_t length_overages() const { return length_overages_; }

    void set_length_overages(size_t new_count) { length_overages_ = new_count; }

    // Low-water mark management
    [[nodiscard]] int lowwatermark() const { return lowater_; }
    void clear_lowwatermark() { lowater_ = length(); }

    [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE size_t Push(void* ptr) {
      SLL_Push(&list_, ptr);
      const uint32_t length = length_.load(std::memory_order_relaxed) + 1;
      length_.store(length, std::memory_order_relaxed);
      return length;
    }

    [[nodiscard]] ABSL_ATTRIBUTE_ALWAYS_INLINE bool TryPop(void** ret) {
      void* obj = list_;
      if (ABSL_PREDICT_FALSE(obj == nullptr)) {
        return false;
      }

      void* next = SLL_Next(obj);
      list_ = next;
      const uint32_t len = length_.load(std::memory_order_relaxed) - 1;
      length_.store(len, std::memory_order_relaxed);

#if defined(__GNUC__)
      if (ABSL_PREDICT_TRUE(next)) {
        __builtin_prefetch(next, 0, 0);
      }
#endif

      *ret = obj;
      if (ABSL_PREDICT_FALSE(len < lowater_)) {
        lowater_ = len;
      }
      return true;
    }

    void PushBatch(int N, void** batch) {
      TC_ASSERT_GT(N, 0);
      for (int i = 0; i < N - 1; ++i) {
        SLL_SetNext(batch[i], batch[i + 1]);
      }
      SLL_SetNext(batch[N - 1], list_);
      list_ = batch[0];
      length_.store(length_.load(std::memory_order_relaxed) + N,
                    std::memory_order_relaxed);
    }

    void PopBatch(int N, void** batch) {
      void* p = list_;
      for (int i = 0; i < N; ++i) {
        batch[i] = p;
        p = SLL_Next(p);
      }
      list_ = p;
      const uint32_t len = length_.load(std::memory_order_relaxed);
      TC_ASSERT_GE(len, N);
      const uint32_t new_len = len - N;
      length_.store(new_len, std::memory_order_relaxed);
      if (new_len < lowater_) lowater_ = new_len;
    }
  };

  // Gets and returns an object from the transfer cache, and, if possible,
  // also adds some objects of that size class to this thread cache.
  [[nodiscard]] void* FetchFromTransferCache(size_t size_class,
                                             size_t byte_size);

  // Releases some number of items from src.  Adjusts the list's max_length
  // to eventually converge on num_objects_to_move(size_class).
  void ListTooLong(FreeList* list, size_t size_class);

  void DeallocateSlow(void* ptr, FreeList* list, size_t size_class);

  // Releases N items from this thread cache.
  void ReleaseToTransferCache(FreeList* src, size_t size_class, int N);

  // Increase max_size_ by reducing unclaimed_cache_space_ or by
  // reducing the max_size_ of some other thread.  In both cases,
  // the delta is kStealAmount.
  void IncreaseCacheLimit();

  static absl::base_internal::SpinLock threadcache_lock_;

  // Same as above but called with threadcache_lock_ held.
  void IncreaseCacheLimitLocked()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(threadcache_lock_);

  void Scavenge();
  [[nodiscard]] static ThreadCache* CreateCacheIfNecessary();

  // If TLS is available, we also store a copy of the per-thread object
  // in a __thread variable since __thread variables are faster to read
  // than pthread_getspecific().  We still need pthread_setspecific()
  // because __thread variables provide no way to run cleanup code when
  // a thread is destroyed.
  //
  // We also give a hint to the compiler to use the "initial exec" TLS
  // model.  This is faster than the default TLS model, at the cost that
  // you cannot dlopen this library.  (To see the difference, look at
  // the CPU use of __tls_get_addr with and without this attribute.)
  //
  // Since using dlopen on a malloc replacement is asking for trouble in any
  // case, that's a good tradeoff for us.
  ABSL_CONST_INIT static thread_local ThreadCache* thread_local_data_
      ABSL_ATTRIBUTE_INITIAL_EXEC;

  // Thread-specific key.  Initialization here is somewhat tricky
  // because some Linux startup code invokes malloc() before it
  // is in a good enough state to handle pthread_keycreate().
  // Therefore, we use TSD keys only after tsd_inited is set to true.
  // Until then, we use a slow path to get the heap object.
  static bool tsd_inited_;

  // Linked list of heap objects.
  static ThreadCache* thread_heaps_ ABSL_GUARDED_BY(threadcache_lock_);
  static int thread_heap_count_ ABSL_GUARDED_BY(threadcache_lock_);

  // A pointer to one of the objects in thread_heaps_.  Represents
  // the next ThreadCache from which a thread over its max_size_ should
  // steal memory limit.  Round-robin through all of the objects in
  // thread_heaps_.
  static ThreadCache* next_memory_steal_ ABSL_GUARDED_BY(threadcache_lock_);

  // Overall thread cache size.
  static std::atomic<int64_t> overall_thread_cache_size_;

  // Global per-thread cache size.
  static size_t per_thread_cache_size_ ABSL_GUARDED_BY(threadcache_lock_);

  // Represents overall_thread_cache_size_ minus the sum of max_size_
  // across all ThreadCaches.
  static int64_t unclaimed_cache_space_ ABSL_GUARDED_BY(threadcache_lock_);

  // This class is laid out with the most frequently used fields
  // first so that hot elements are placed on the same cache line.

  FreeList list_[kNumClasses];  // Array indexed by size-class

  std::atomic<size_t> size_;      // Combined size of data
  std::atomic<size_t> max_size_;  // size_ > max_size_ --> Scavenge()

  pthread_t tid_;
  bool in_setspecific_;

  // Allocate a new heap.
  [[nodiscard]] static ThreadCache* NewHeap(pthread_t tid)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(threadcache_lock_);

  // Use only as pthread thread-specific destructor function.
  static void DestroyThreadCache(ThreadCache* ptr);

  static void DeleteCache(ThreadCache* heap);
  static void RecomputePerThreadCacheSize()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(threadcache_lock_);

  // All ThreadCache objects are kept in a linked list (for stats collection)
  ThreadCache* next_ ABSL_GUARDED_BY(threadcache_lock_);
  ThreadCache* prev_ ABSL_GUARDED_BY(threadcache_lock_);
};

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* ThreadCache::Allocate(
    size_t size_class) {
  const size_t allocated_size = tc_globals.sizemap().class_to_size(size_class);

  FreeList* list = &list_[size_class];
  void* ret;
  if (ABSL_PREDICT_TRUE(list->TryPop(&ret))) {
    size_.store(size_.load(std::memory_order_relaxed) - allocated_size,
                std::memory_order_relaxed);
    return ret;
  }

  return FetchFromTransferCache(size_class, allocated_size);
}

inline void ABSL_ATTRIBUTE_ALWAYS_INLINE
ThreadCache::Deallocate(void* ptr, size_t size_class) {
  FreeList* list = &list_[size_class];
  const size_t size = size_.load(std::memory_order_relaxed) +
                      tc_globals.sizemap().class_to_size(size_class);
  size_.store(size, std::memory_order_relaxed);
  ssize_t size_headroom = max_size_.load(std::memory_order_relaxed) - size - 1;

  const size_t list_length = list->Push(ptr);
  ssize_t list_headroom =
      static_cast<ssize_t>(list->max_length()) - list_length;

  // There are two relatively uncommon things that require further work.
  // In the common case we're done, and in that case we need a single branch
  // because of the bitwise-or trick that follows.
  if ((list_headroom | size_headroom) < 0) {
    DeallocateSlow(ptr, list, size_class);
  }
}

inline ThreadCache* ABSL_ATTRIBUTE_ALWAYS_INLINE
ThreadCache::GetCacheIfPresent() {
  return thread_local_data_;
}

inline ThreadCache* ThreadCache::GetCache() {
  ThreadCache* tc = GetCacheIfPresent();
  return (ABSL_PREDICT_TRUE(tc != nullptr)) ? tc : CreateCacheIfNecessary();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_THREAD_CACHE_H_
