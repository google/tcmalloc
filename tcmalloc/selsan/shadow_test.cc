// Copyright 2024 The TCMalloc Authors
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

// The test won't work in the actual SelSan build b/c the test uses fake shadow.
#if !defined(TCMALLOC_INTERNAL_SELSAN) && \
    (defined(__x86_64__) || defined(__aarch64__))
#define TCMALLOC_INTERNAL_SELSAN 1
#define TCMALLOC_SELSAN_TEST_SHADOW_OVERRIDE 1

#include <stdint.h>
#include <string.h>

#include "gtest/gtest.h"
#include "tcmalloc/selsan/selsan.h"

namespace tcmalloc::tcmalloc_internal::selsan {

bool enabled = true;
unsigned char test_shadow[1024];
uintptr_t kShadowBase = reinterpret_cast<uintptr_t>(test_shadow);

namespace {

TEST(Unit, ResetTag) {
  {
    memset(test_shadow, 0xff, sizeof(test_shadow));
    void* res = ResetTag(reinterpret_cast<void*>(0x5a00000000000000), 16);
    EXPECT_EQ(res, reinterpret_cast<void*>(0));
    EXPECT_EQ(test_shadow[0], 0);
    EXPECT_EQ(test_shadow[1], 0xff);
    EXPECT_EQ(test_shadow[2], 0xff);
  }
  {
    memset(test_shadow, 0x11, sizeof(test_shadow));
    void* res = ResetTag(reinterpret_cast<void*>(0x1800000000000010), 32);
    EXPECT_EQ(res, reinterpret_cast<void*>(0x10));
    EXPECT_EQ(test_shadow[0], 0x11);
    EXPECT_EQ(test_shadow[1], 0);
    EXPECT_EQ(test_shadow[2], 0);
    EXPECT_EQ(test_shadow[3], 0x11);
  }
  {
    memset(test_shadow, 0x11, sizeof(test_shadow));
    void* res = ResetTag(reinterpret_cast<void*>(0x1800000000000100), 0x50);
    EXPECT_EQ(res, reinterpret_cast<void*>(0x100));
    EXPECT_EQ(test_shadow[0], 0x11);
    EXPECT_EQ(test_shadow[15], 0x11);
    EXPECT_EQ(test_shadow[16], 0);
    EXPECT_EQ(test_shadow[17], 0);
    EXPECT_EQ(test_shadow[18], 0);
    EXPECT_EQ(test_shadow[19], 0);
    EXPECT_EQ(test_shadow[20], 0);
    EXPECT_EQ(test_shadow[21], 0x11);
  }
}

TEST(Unit, UpdateTag) {
  constexpr uintptr_t kTagInc = 1ul << (kTagShift + 1);
  constexpr uintptr_t kShadowInc = kTagInc >> 56;
  {
    memset(test_shadow, 0x5a, sizeof(test_shadow));
    void* res = UpdateTag(reinterpret_cast<void*>(0x5a00000000000000), 1);
    EXPECT_EQ(res, reinterpret_cast<void*>(0x5a00000000000000 + kTagInc));
    EXPECT_EQ(test_shadow[0], 0x5a + kShadowInc);
    EXPECT_EQ(test_shadow[1], 0x5a);
    EXPECT_EQ(test_shadow[2], 0x5a);
  }
  {
    memset(test_shadow, 0x5a, sizeof(test_shadow));
    void* res = UpdateTag(reinterpret_cast<void*>(0x5a00000000000000), 16);
    EXPECT_EQ(res, reinterpret_cast<void*>(0x5a00000000000000 + kTagInc));
    EXPECT_EQ(test_shadow[0], 0x5a + kShadowInc);
    EXPECT_EQ(test_shadow[1], 0x5a);
  }
  {
    memset(test_shadow, 0x5a, sizeof(test_shadow));
    void* res = UpdateTag(reinterpret_cast<void*>(0x5a00000000000010), 17);
    EXPECT_EQ(res, reinterpret_cast<void*>(0x5a00000000000010 + kTagInc));
    EXPECT_EQ(test_shadow[0], 0x5a);
    EXPECT_EQ(test_shadow[1], 0x5a + kShadowInc);
    EXPECT_EQ(test_shadow[2], 0x5a + kShadowInc);
    EXPECT_EQ(test_shadow[3], 0x5a);
  }
  {
    memset(test_shadow, 0x7e, sizeof(test_shadow));
    void* res = UpdateTag(reinterpret_cast<void*>(0x7e00000000000000), 16);
    // Since LAM requires the top bit to be 0, this will overflow differently.
#if defined(__x86_64__)
    EXPECT_EQ(res, reinterpret_cast<void*>(0x0200000000000000));
    EXPECT_EQ(test_shadow[0], 0x02);
#elif defined(__aarch64__)
    EXPECT_EQ(res, reinterpret_cast<void*>(0x8000000000000000));
    EXPECT_EQ(test_shadow[0], 0x80);
#else
#error "Unsupported platform."
#endif
    EXPECT_EQ(test_shadow[1], 0x7e);
  }
#if defined(__aarch64__)
  {
    memset(test_shadow, 0xff, sizeof(test_shadow));
    void* res = UpdateTag(reinterpret_cast<void*>(0xff00000000000000), 16);
    // Since LAM requires the top bit to be 0, this will overflow differently.
    EXPECT_EQ(res, reinterpret_cast<void*>(0x0100000000000000));
    EXPECT_EQ(test_shadow[0], 0x01);
    EXPECT_EQ(test_shadow[1], 0xff);
  }
#endif
}

TEST(Unit, SetTag) {
  for (size_t size = 1; size < (sizeof(test_shadow) - 2) * kShadowScale;
       size++) {
    SCOPED_TRACE(size);
    memset(test_shadow, 0x55, sizeof(test_shadow));
    SetTag(0x1800000000000000 + kShadowScale, size, 0x77);
    size_t n = (size + kShadowScale - 1) / kShadowScale;
    ASSERT_EQ(test_shadow[0], 0x55);
    ASSERT_EQ(test_shadow[n + 1], 0x55);
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(test_shadow[i + 1], 0x77);
    }
  }
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::selsan
#endif
