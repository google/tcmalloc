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

#include "tcmalloc/testing/smaps.h"

#include <cstring>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <limits>
#include <vector>

#include "gtest/gtest.h"
#include "absl/strings/string_view.h"

namespace tcmalloc {
namespace {

TEST(SmapsTest, ParseSmapsEntries) {
  constexpr absl::string_view kInput =
      "00400000-00452000 r-xp 00000000 08:02 173521      /usr/bin/foo\n"
      "Size:                328 kB\n"
      "Rss:                 132 kB\n"
      "VmFlags: rd ex mr mw me dw sd\n"
      "7f8a00000000-7f8a00200000 rw-p 00000000 00:00 0   [anon:slab]\n"
      "Size:               2048 kB\n"
      "Rss:                  64 kB\n"
      "VmFlags: rd wr mr mw me ac sd nh\n";

  std::vector<SmapEntry> entries;
  ParseSmaps(kInput, [&](const SmapEntry& entry) { entries.push_back(entry); });

  ASSERT_EQ(entries.size(), 2);

  EXPECT_EQ(entries[0].start, 0x00400000u);
  EXPECT_EQ(entries[0].end, 0x00452000u);
  EXPECT_EQ(entries[0].rss_bytes, 132u * 1024u);
  EXPECT_TRUE(entries[0].HasVmFlag("rd"));
  EXPECT_TRUE(entries[0].HasVmFlag("ex"));
  EXPECT_FALSE(entries[0].HasVmFlag("nh"));

  EXPECT_EQ(entries[1].start, 0x7f8a00000000u);
  EXPECT_EQ(entries[1].end, 0x7f8a00200000u);
  EXPECT_EQ(entries[1].rss_bytes, 64u * 1024u);
  EXPECT_TRUE(entries[1].HasVmFlag("nh"));
  EXPECT_FALSE(entries[1].HasVmFlag("ex"));
}

TEST(SmapsTest, ParseMalformedAndEmpty) {
  std::vector<SmapEntry> entries;
  ParseSmaps("", [&](const SmapEntry& entry) { entries.push_back(entry); });
  EXPECT_TRUE(entries.empty());

  constexpr absl::string_view kMalformedHeaders =
      "00400000-00452000\n"
      "00400000 r-xp 00000000 08:02 0\n"
      "zzzzzzzz-00452000 r-xp 00000000 08:02 0\n"
      "00400000-zzzzzzzz r-xp 00000000 08:02 0\n";
  ParseSmaps(kMalformedHeaders,
             [&](const SmapEntry& entry) { entries.push_back(entry); });
  EXPECT_TRUE(entries.empty());

  constexpr absl::string_view kMalformedRss =
      "00400000-00452000 r-xp 00000000 08:02 0\n"
      "Rss: 100\n"
      "Rss: abc kB\n"
      "Rss: 12 kB\n";
  ParseSmaps(kMalformedRss,
             [&](const SmapEntry& entry) { entries.push_back(entry); });
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].rss_bytes, 12u * 1024u);
}

TEST(SmapsTest, Overlaps) {
  SmapEntry entry;
  entry.start = 0x1000;
  entry.end = 0x3000;

  EXPECT_FALSE(entry.Overlaps(reinterpret_cast<const void*>(0x0), 0x1000));
  EXPECT_TRUE(entry.Overlaps(reinterpret_cast<const void*>(0x0), 0x1001));
  EXPECT_TRUE(entry.Overlaps(reinterpret_cast<const void*>(0x1000), 0x1000));
  EXPECT_TRUE(entry.Overlaps(reinterpret_cast<const void*>(0x2fff), 0x1000));
  EXPECT_FALSE(entry.Overlaps(reinterpret_cast<const void*>(0x3000), 0x1000));
  EXPECT_FALSE(entry.Overlaps(reinterpret_cast<const void*>(0x2000), 0));
  EXPECT_FALSE(entry.Overlaps(reinterpret_cast<const void*>(0x2000),
                              std::numeric_limits<size_t>::max()));
}

#ifdef __linux__
TEST(SmapsTest, LiveProcSelfSmaps) {
  const size_t page_size = sysconf(_SC_PAGESIZE);
  void* mapping = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mapping, MAP_FAILED);
  ASSERT_EQ(madvise(mapping, page_size, MADV_NOHUGEPAGE), 0);
  memset(mapping, 0, page_size);

  size_t total_rss = 0, mapping_rss = 0;
  bool found_mapping = false;
  bool has_nh = false;
  ASSERT_TRUE(ForEachSmapEntry([&](const SmapEntry& entry) {
    total_rss += entry.rss_bytes;
    if (entry.Overlaps(mapping, page_size)) {
      found_mapping = true;
      mapping_rss += entry.rss_bytes;
      has_nh |= entry.HasVmFlag("nh");
    }
  }));

  EXPECT_GT(total_rss, 0);
  EXPECT_TRUE(found_mapping);
  EXPECT_TRUE(has_nh);
  EXPECT_GT(mapping_rss, 0);

  ASSERT_EQ(munmap(mapping, page_size), 0);
}
#endif  // __linux__

}  // namespace
}  // namespace tcmalloc
