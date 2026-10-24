// Copyright 2022 The TCMalloc Authors
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

// Extra extensions exported by some malloc implementations.  These
// extensions are accessed through a virtual base class so an
// application can link against a malloc that does not implement these
// extensions, and it will get default versions that do nothing.

#include "tcmalloc/malloc_tracing_extension.h"

#include <stddef.h>
#include <stdint.h>

#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

#ifndef MALLOC_TRACING_EXTENSION_NOT_SUPPORTED
#include <atomic>
#include <new>
#include <optional>
#include <thread>

#include "gmock/gmock.h"
#include "absl/cleanup/cleanup.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_hook.h"
#endif

namespace {

#ifdef MALLOC_TRACING_EXTENSION_NOT_SUPPORTED
TEST(MallocTracingExtension, GetAllocatedAddressRanges) {
  // Act.
  absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
      allocated =
          tcmalloc::malloc_tracing_extension::GetAllocatedAddressRanges();

  // Assert.
  ASSERT_FALSE(allocated.ok());
  EXPECT_EQ(allocated.status().code(), absl::StatusCode::kUnimplemented);
}
#else

using ::tcmalloc::malloc_tracing_extension::AllocatedAddressRanges;
using ::testing::AllOf;
using ::testing::Each;
using ::testing::Field;
using ::testing::Gt;

bool InRange(const AllocatedAddressRanges::SpanDetails& span, void* obj_start,
             size_t obj_size) {
  return span.start_addr <= (uintptr_t)obj_start &&
         ((uintptr_t)obj_start + obj_size) <= (span.start_addr + span.size);
}

std::optional<AllocatedAddressRanges::SpanDetails> GetSpanDetailsForObject(
    const AllocatedAddressRanges& allocated, void* obj_start, size_t obj_size) {
  for (const AllocatedAddressRanges::SpanDetails& span : allocated.spans) {
    if (InRange(span, obj_start, obj_size)) return span;
  }
  return std::nullopt;
}

TEST(MallocTracingExtension, GetAllocatedAddressRanges) {
  // Arrange.
  const int kArrCount = 3;
  size_t size[] = {2, 1000, 1000000};
  void* arr[kArrCount];
  for (int i = 0; i < kArrCount; i++) {
    arr[i] = ::operator new(size[i]);
  }
  absl::Cleanup cleanup = [arr] {
    for (int i = 0; i < kArrCount; i++) {
      ::operator delete(arr[i]);
    }
  };

  // Act.
  absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
      allocated =
          tcmalloc::malloc_tracing_extension::GetAllocatedAddressRanges();

  // Assert.
  ASSERT_TRUE(allocated.ok());
  EXPECT_GT(allocated.value().spans.size(), 0);
  EXPECT_THAT(
      allocated.value().spans,
      Each(AllOf(
          Field("start_addr", &AllocatedAddressRanges::SpanDetails::start_addr,
                Gt(0)),
          Field("size", &AllocatedAddressRanges::SpanDetails::size, Gt(0)))));
  for (int i = 0; i < kArrCount; i++) {
    std::optional<AllocatedAddressRanges::SpanDetails> span =
        GetSpanDetailsForObject(allocated.value(), arr[i], size[i]);
    ASSERT_TRUE(span.has_value())
        << " for the " << size[i] << "-byte object at index " << i;
    // Non-sizeclass objects that are larger than kMaxSize will have
    // object_size == 0.
    if (span.value().object_size != 0) {
      EXPECT_GE(span.value().object_size, size[i])
          << " for the " << size[i] << "-byte object at index " << i;
    }
  }
}

std::atomic<const void*> target_deleted_ptr{nullptr};
std::atomic<bool> in_sampled_delete_hook{false};
std::atomic<bool> snapshot_taken{false};

void OnSampledDeleteForTracingTest(
    const
    tcmalloc::MallocHook::SampledAlloc& sampled_alloc) {
  const void* target = target_deleted_ptr.load(std::memory_order_acquire);
  if (target != nullptr && sampled_alloc.ptr == target) {
    target_deleted_ptr.store(nullptr, std::memory_order_release);
    in_sampled_delete_hook.store(true, std::memory_order_release);
    while (!snapshot_taken.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
}

TEST(MallocTracingExtension,
     GetAllocatedAddressRangesDoesNotReadUnregisteredSample) {
  const int64_t old_profile_interval =
      tcmalloc::MallocExtension::GetProfileSamplingInterval();
  const int64_t old_guarded_interval =
      tcmalloc::MallocExtension::GetGuardedSamplingInterval();
  tcmalloc::MallocExtension::SetProfileSamplingInterval(1);
  tcmalloc::MallocExtension::SetGuardedSamplingInterval(-1);
  absl::Cleanup restore_intervals = [&] {
    tcmalloc::MallocExtension::SetProfileSamplingInterval(old_profile_interval);
    tcmalloc::MallocExtension::SetGuardedSamplingInterval(old_guarded_interval);
  };

  in_sampled_delete_hook.store(false, std::memory_order_relaxed);
  snapshot_taken.store(false, std::memory_order_relaxed);
  ASSERT_TRUE(tcmalloc::MallocHook::AddSampledDeleteHook(
      &OnSampledDeleteForTracingTest));
  absl::Cleanup remove_hook = [&] {
    (void)tcmalloc::MallocHook::RemoveSampledDeleteHook(
        &OnSampledDeleteForTracingTest);
  };

  constexpr size_t kLargeSize = 2 * 1024 * 1024;
  void* p1;
  do {
    p1 = ::operator new(kLargeSize);
    in_sampled_delete_hook.store(false, std::memory_order_relaxed);
    snapshot_taken.store(false, std::memory_order_relaxed);
    target_deleted_ptr.store(p1, std::memory_order_release);

    absl::StatusOr<AllocatedAddressRanges> ranges_during_delete;
    std::thread worker([&] {
      while (!in_sampled_delete_hook.load(std::memory_order_acquire) &&
             target_deleted_ptr.load(std::memory_order_acquire) != nullptr) {
        std::this_thread::yield();
      }
      if (!in_sampled_delete_hook.load(std::memory_order_acquire)) {
        return;
      }
      // Reuse the unregistered SampledAllocation node with a 64-byte allocation
      // while p1's free is paused in SampledDeleteHook.
      void* p2 = ::operator new(64);
      ranges_during_delete =
          tcmalloc::malloc_tracing_extension::GetAllocatedAddressRanges();
      ::operator delete(p2);
      snapshot_taken.store(true, std::memory_order_release);
    });

    ::operator delete(p1);
    target_deleted_ptr.store(nullptr, std::memory_order_release);
    worker.join();

    if (in_sampled_delete_hook.load(std::memory_order_acquire)) {
      ASSERT_TRUE(ranges_during_delete.ok());
      std::optional<AllocatedAddressRanges::SpanDetails> p1_span =
          GetSpanDetailsForObject(*ranges_during_delete, p1, 1);
      EXPECT_TRUE(!p1_span.has_value() || p1_span->size >= kLargeSize)
          << "p1 span reported corrupted size " << p1_span->size
          << " from recycled SampledAllocation (expected >= " << kLargeSize
          << ")";
      break;
    }
  } while (true);
}
#endif

}  // namespace
