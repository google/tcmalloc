// Copyright 2026 The TCMalloc Authors
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

#include "tcmalloc/internal/heap.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <new>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/random/random.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

struct Element {
  int priority = 0;
  size_t index = 0;

  using PriorityType = int;

  Element() = default;
  /*implicit*/ Element(int p) : priority(p) {}
  operator int() const { return priority; }

  PriorityType Priority() const { return priority; }

  static bool Compare(PriorityType a, PriorityType b) { return a < b; }
  void SetIndex(size_t idx) { index = idx; }
};

template <typename H>
bool IsHeap(H& heap) {
  for (size_t i = 1; i < heap.size(); ++i) {
    size_t parent = (i - 1) / 2;
    if (std::decay_t<decltype(heap[0])>::Compare(heap[i].Priority(),
                                                 heap[parent].Priority())) {
      return false;
    }
  }
  return true;
}

template <typename T>
struct Vector : std::vector<T> {
  using std::vector<T>::vector;

  void Init(size_t n) { this->resize(n); }

  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const T& val : *this) {
      fn(val);
    }
  }
};

TEST(HeapTest, EmptyHeap) {
  Heap<Element, Vector<Element>> heap;
  EXPECT_TRUE(heap.empty());
  EXPECT_EQ(heap.size(), 0);
  EXPECT_TRUE(IsHeap(heap));
}

TEST(HeapTest, PushAndPopMinAscending) {
  Heap<Element, Vector<Element>> heap;
  for (int i = 1; i <= 10; ++i) {
    (void)heap.Push(i, i);
    EXPECT_TRUE(IsHeap(heap));
    EXPECT_EQ(heap[0], 1);
    EXPECT_EQ(heap.size(), i);
  }

  for (int i = 1; i <= 10; ++i) {
    EXPECT_EQ(heap[0], i);
    heap.Remove(0);
    EXPECT_TRUE(IsHeap(heap));
    EXPECT_EQ(heap.size(), 10 - i);
  }
  EXPECT_TRUE(heap.empty());
}

TEST(HeapTest, PushAndPopMinDescending) {
  Heap<Element, Vector<Element>> heap;
  for (int i = 10; i >= 1; --i) {
    (void)heap.Push(i, i);
    EXPECT_TRUE(IsHeap(heap));
    EXPECT_EQ(heap[0], i);
  }

  for (int i = 1; i <= 10; ++i) {
    heap.Remove(0);
    EXPECT_TRUE(IsHeap(heap));
  }
  EXPECT_TRUE(heap.empty());
}

TEST(HeapTest, PushAndPopMinDuplicates) {
  Heap<Element, Vector<Element>> heap;
  std::vector<int> values = {5, 1, 3, 5, 2, 1, 3, 2, 5};
  for (int v : values) {
    (void)heap.Push(v, v);
    EXPECT_TRUE(IsHeap(heap));
  }

  std::sort(values.begin(), values.end());
  for (int expected : values) {
    EXPECT_EQ(heap[0], expected);
    heap.Remove(0);
    EXPECT_TRUE(IsHeap(heap));
  }
  EXPECT_TRUE(heap.empty());
}

TEST(HeapTest, PopAtRoot) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50}) {
    (void)heap.Push(v, v);
  }
  heap.Remove(0);
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(heap[0], 20);
  EXPECT_EQ(heap.size(), 4);
}

TEST(HeapTest, PopAtLast) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50}) {
    (void)heap.Push(v, v);
  }
  heap.Remove(heap.size() - 1);
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(heap.size(), 4);
}

TEST(HeapTest, BackAndPopBack) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50}) {
    (void)heap.Push(v, v);
  }
  while (!heap.empty()) {
    int expected = heap[heap.size() - 1];
    EXPECT_EQ(heap.Back(), expected);
    heap.PopBack();
    EXPECT_TRUE(IsHeap(heap));
  }
}

TEST(HeapTest, PopAtSiftDown) {
  // Construct heap where removing an intermediate node causes SiftDown:
  // Tree:
  //          10
  //        /    \
  //      20      30
  //     /  \    /
  //    40  50  60
  // If we remove index 1 (value 20), the last element (60) is placed at index
  // 1, and must sift down since it is larger than its children 40 and 50.
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50, 60}) {
    (void)heap.Push(v, v);
  }
  EXPECT_TRUE(IsHeap(heap));

  // Find index of element with value 20.
  size_t idx20 = 0;
  for (size_t i = 0; i < heap.size(); ++i) {
    if (heap[i] == 20) {
      idx20 = i;
      break;
    }
  }
  heap.Remove(idx20);
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(heap.size(), 5);

  // Remaining elements must pop in sorted order: 10, 30, 40, 50, 60.
  std::vector<int> remaining;
  while (!heap.empty()) {
    remaining.push_back(heap[0]);
    heap.Remove(0);
  }
  EXPECT_EQ(remaining, (std::vector<int>{10, 30, 40, 50, 60}));
}

TEST(HeapTest, PopAtSiftUp) {
  // Construct a heap where the last element is smaller than an ancestor in the
  // left branch, so replacing an element with the last element requires SiftUp.
  // Explicitly build array:
  // Index 0: 1
  // Index 1: 20
  // Index 2: 2
  // Index 3: 25
  // Index 4: 26
  // Index 5: 3
  // Here 3 (index 5) is child of 2 (index 2).
  // If index 3 (value 25) is removed, 3 is placed at index 3.
  // Its parent is index 1 (value 20).
  // Since 3 < 20, it must sift UP to index 1!
  Vector<Element> arr = {1, 20, 2, 25, 26, 3};
  Heap<Element, Vector<Element>> heap(arr);
  ASSERT_TRUE(IsHeap(heap));

  heap.Remove(3);
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(heap.size(), 5);

  std::vector<int> remaining;
  while (!heap.empty()) {
    remaining.push_back(heap[0]);
    heap.Remove(0);
  }
  EXPECT_EQ(remaining, (std::vector<int>{1, 2, 3, 20, 26}));
}

TEST(HeapTest, ChangePrioritySiftUp) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50, 60, 70}) {
    (void)heap.Push(v, v);
  }
  EXPECT_TRUE(IsHeap(heap));

  heap[6] = 5;
  auto [new_ptr, new_idx] = heap.SiftUp(6, heap[6].Priority());
  EXPECT_EQ(new_idx, 0);
  EXPECT_EQ(new_ptr, &heap[0]);
  EXPECT_EQ(heap[0], 5);
  EXPECT_TRUE(IsHeap(heap));
}

TEST(HeapTest, ChangePrioritySiftDown) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50, 60, 70}) {
    (void)heap.Push(v, v);
  }
  EXPECT_TRUE(IsHeap(heap));

  heap[0] = 100;
  auto [new_ptr, new_idx] = heap.SiftDown(0);
  EXPECT_GT(new_idx, 0);
  EXPECT_EQ(new_ptr, &heap[new_idx]);
  EXPECT_NE(heap[0], 100);
  EXPECT_TRUE(IsHeap(heap));
}

TEST(HeapTest, ChangePriorityInPlace) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {10, 20, 30, 40, 50, 60, 70}) {
    (void)heap.Push(v, v);
  }

  heap[4] = 8;
  auto [new_ptr, new_idx] = heap.SiftUp(4, heap[4].Priority());
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(new_ptr, &heap[new_idx]);
  EXPECT_EQ(heap[new_idx], 8);
  EXPECT_EQ(heap[0], 8);
}

TEST(HeapTest, ForEach) {
  Heap<Element, Vector<Element>> heap;
  for (int v : {40, 20, 10, 50, 30}) {
    (void)heap.Push(v, v);
  }
  std::vector<int> collected;
  heap.ForEach([&](const Element& e) { collected.push_back(e.priority); });
  EXPECT_EQ(collected.size(), 5);
}

// A custom physical array type that is non-contiguous, segmented into
// fixed-size chunks. This tests that Heap works with an abstract array with a
// more complex physical layout than std::vector.
template <typename T, size_t ChunkSize = 4>
class ChunkedArray {
 public:
  ChunkedArray() : size_(0) {}

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  T& operator[](size_t index) {
    size_t chunk_idx = index / ChunkSize;
    size_t offset = index % ChunkSize;
    return chunks_[chunk_idx][offset];
  }

  const T& operator[](size_t index) const {
    size_t chunk_idx = index / ChunkSize;
    size_t offset = index % ChunkSize;
    return chunks_[chunk_idx][offset];
  }

  void push_back(const T& val) {
    size_t offset = size_ % ChunkSize;
    if (offset == 0) {
      chunks_.push_back(std::make_unique<T[]>(ChunkSize));
    }
    chunks_.back()[offset] = val;
    ++size_;
  }

  void push_back(T&& val) {
    size_t offset = size_ % ChunkSize;
    if (offset == 0) {
      chunks_.push_back(std::make_unique<T[]>(ChunkSize));
    }
    chunks_.back()[offset] = std::move(val);
    ++size_;
  }

  void pop_back() {
    --size_;
    if (size_ % ChunkSize == 0) {
      chunks_.pop_back();
    }
  }

 private:
  std::vector<std::unique_ptr<T[]>> chunks_;
  size_t size_;
};

TEST(HeapTest, CustomChunkedPhysicalArray) {
  Heap<Element, ChunkedArray<Element, 4>> heap;

  // Insert 30 elements spanning across ~8 physical chunks.
  std::vector<int> values = {45, 12, 89, 3,  77, 23, 67, 1,  99, 14,
                             55, 8,  34, 71, 19, 62, 5,  41, 83, 29,
                             91, 16, 50, 37, 7,  73, 2,  64, 48, 25};
  for (int v : values) {
    (void)heap.Push(v, v);
    EXPECT_TRUE(IsHeap(heap));
  }
  EXPECT_EQ(heap.size(), 30);
  EXPECT_EQ(heap[0], 1);

  // Test Remove in chunked array.
  heap.Remove(10);
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(heap.size(), 29);

  // Test Change in chunked array.
  heap[5] = 0;  // Set to minimum value.
  (void)heap.SiftUp(5, heap[5].Priority());
  EXPECT_TRUE(IsHeap(heap));
  EXPECT_EQ(heap[0], 0);

  // Pop everything and verify ordered output.
  int prev = -1;
  while (!heap.empty()) {
    int cur = heap[0];
    heap.Remove(0);
    EXPECT_GE(cur, prev);
    prev = cur;
    EXPECT_TRUE(IsHeap(heap));
  }
}

TEST(HeapTest, RandomizedFuzzOperations) {
  absl::BitGen rng;
  Heap<Element, Vector<Element>> heap;
  std::multiset<int> shadow;

  const int kNumOps = 5000;
  for (int op = 0; op < kNumOps; ++op) {
    int choice = absl::Uniform(rng, 0, 100);
    if (choice < 40 || heap.empty()) {
      int val = absl::Uniform(rng, -1000, 1000);
      (void)heap.Push(val, val);
      shadow.insert(val);
    } else if (choice < 65) {
      int heap_min = heap[0];
      heap.Remove(0);
      int shadow_min = *shadow.begin();
      shadow.erase(shadow.begin());
      EXPECT_EQ(heap_min, shadow_min);
    } else if (choice < 85) {
      size_t idx = absl::Uniform(rng, size_t{0}, heap.size());
      int val = heap[idx];
      int popped = heap[idx];
      heap.Remove(idx);
      EXPECT_EQ(popped, val);
      auto it = shadow.find(val);
      ASSERT_NE(it, shadow.end());
      shadow.erase(it);
    } else {
      size_t idx = absl::Uniform(rng, size_t{0}, heap.size());
      int old_val = heap[idx];
      int new_val = absl::Uniform(rng, -1000, 1000);
      auto it = shadow.find(old_val);
      ASSERT_NE(it, shadow.end());
      shadow.erase(it);
      shadow.insert(new_val);
      heap[idx] = new_val;
      if (idx > 0) {
        auto [ptr, new_idx] = heap.SiftUp(idx, new_val);
        if (new_idx == idx) {
          (void)heap.SiftDown(idx);
        }
      } else {
        (void)heap.SiftDown(idx);
      }
    }

    ASSERT_EQ(heap.size(), shadow.size());
    ASSERT_TRUE(IsHeap(heap));
    if (!heap.empty()) {
      ASSERT_EQ(heap[0], *shadow.begin());
    }
  }
}

TEST(HeapTest, ReturnElementAndIndex) {
  Heap<Element, Vector<Element>> heap;
  auto [p1, idx1] = heap.Push(30, 30);
  EXPECT_EQ(idx1, 0);
  EXPECT_EQ(p1, &heap[0]);
  EXPECT_EQ(p1->priority, 30);

  auto [p2, idx2] = heap.Push(10, 10);
  EXPECT_EQ(idx2, 0);
  EXPECT_EQ(p2, &heap[0]);
  EXPECT_EQ(p2->priority, 10);

  auto [p3, idx3] = heap.Push(20, 20);
  EXPECT_EQ(idx3, 2);
  EXPECT_EQ(p3, &heap[2]);
  EXPECT_EQ(p3->priority, 20);

  heap[1] = 5;
  auto [p4, idx4] = heap.SiftUp(1, heap[1].Priority());
  EXPECT_EQ(idx4, 0);
  EXPECT_EQ(p4, &heap[0]);
  EXPECT_EQ(p4->priority, 5);
}

struct TrackedItem {
  int id = 0;
  int priority = 0;
  size_t* index_tracker = nullptr;

  using PriorityType = int;
  PriorityType Priority() const { return priority; }

  static bool Compare(PriorityType a, PriorityType b) { return a < b; }
  void SetIndex(size_t index) {
    if (index_tracker != nullptr) {
      *index_tracker = index;
    }
  }
};

TEST(HeapTest, IndexTracking) {
  Heap<TrackedItem, std::vector<TrackedItem>> heap;
  std::vector<size_t> indices(7, ~size_t{0});
  std::vector<int> priorities = {50, 20, 80, 10, 40, 70, 30};

  for (int i = 0; i < 7; ++i) {
    auto [ptr, idx] = heap.Push(
        TrackedItem{
            .id = i,
            .priority = priorities[i],
            .index_tracker = &indices[i],
        },
        priorities[i]);
    indices[i] = idx;
  }

  for (int i = 0; i < 7; ++i) {
    ASSERT_LT(indices[i], heap.size());
    EXPECT_EQ(heap[indices[i]].id, i);
  }

  size_t idx0 = indices[0];
  heap[idx0].priority = 5;
  auto [ptr, new_idx] = heap.SiftUp(idx0, heap[idx0].Priority());
  ptr->SetIndex(new_idx);
  indices[0] = new_idx;
  for (int i = 0; i < 7; ++i) {
    ASSERT_LT(indices[i], heap.size());
    EXPECT_EQ(heap[indices[i]].id, i);
  }

  size_t idx_to_remove = indices[2];
  TrackedItem removed = heap[idx_to_remove];
  heap.Remove(idx_to_remove);
  EXPECT_EQ(removed.id, 2);
  indices[2] = ~size_t{0};
  for (int i = 0; i < 7; ++i) {
    if (i == 2) continue;
    ASSERT_LT(indices[i], heap.size());
    EXPECT_EQ(heap[indices[i]].id, i);
  }
}

TEST(HeapTest, PrependPriority) {
  struct PriorityItem {
    int id = 0;
    uint16_t priority = 0;

    using PriorityType = uint16_t;
    PriorityType Priority() const { return priority; }

    static bool Compare(PriorityType a, PriorityType b) { return a < b; }
    void SetIndex(size_t index) {}
  };

  Heap<PriorityItem, std::vector<PriorityItem>> heap;
  // Push item 0 with priority 16 and prepend = false.
  (void)heap.Push(PriorityItem{.id = 0, .priority = 16}, /*priority=*/15);
  EXPECT_EQ(heap[0].id, 0);
  EXPECT_EQ(heap[0].priority, 16);

  // Push item 1 with priority 16 and prepend = true -> not sifted, stays at
  // Back().
  (void)heap.Push(PriorityItem{.id = 1, .priority = 16}, /*priority=*/16);
  // Min-heap: item 0 stays at root, item 1 at Back().
  EXPECT_EQ(heap[0].id, 0);
  EXPECT_EQ(heap[0].priority, 16);
  EXPECT_EQ(heap.Back().id, 1);
  EXPECT_EQ(heap.Back().priority, 16);

  // Push item 2 with priority 20 and prepend = false.
  (void)heap.Push(PriorityItem{.id = 2, .priority = 20}, /*priority=*/20);
  EXPECT_EQ(heap.size(), 3);

  // SiftUp item 2 with temporary priority 15 without modifying item's stored
  // priority.
  heap[2].priority = 16;
  auto [ptr, new_idx] = heap.SiftUp(2, /*priority=*/15);
  EXPECT_EQ(ptr->priority, 16);
  EXPECT_EQ(new_idx, 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
