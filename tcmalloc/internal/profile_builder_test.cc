// Copyright 2021 The TCMalloc Authors
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

#include "tcmalloc/internal/profile_builder.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "tcmalloc/internal/profile.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/fake_profile.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::Pair;
using testing::UnorderedElementsAre;

// Returns the fully resolved path of this program.
std::string RealPath() {
  char path[PATH_MAX];
  if (realpath("/proc/self/exe", path)) {
    return path;
  }
  return "";
}

TEST(ProfileBuilderTest, Mappings) {
  ProfileBuilder builder;
  builder.AddCurrentMappings();
  auto profile = std::move(builder).Finalize();

  absl::flat_hash_set<std::string> filenames;
  absl::flat_hash_set<int> mapping_ids;
  for (const auto& mapping : profile->mapping()) {
    const int filename_id = mapping.filename();
    ASSERT_GE(filename_id, 0);
    ASSERT_LT(filename_id, profile->string_table_size());

    const absl::string_view filename = profile->string_table(filename_id);
    filenames.emplace(filename);
    mapping_ids.insert(mapping.id());
  }

  // Check for duplicates in mapping IDs.
  EXPECT_EQ(mapping_ids.size(), profile->mapping_size());
  EXPECT_THAT(filenames, testing::Contains(RealPath()));

  // Ensure that no mapping ID is ID "0".
  EXPECT_THAT(mapping_ids, testing::Not(testing::Contains(0)));
}

TEST(ProfileBuilderTest, LocationTableNoMappings) {
  const uintptr_t kAddress = uintptr_t{0x150};

  ProfileBuilder builder;
  const int loc1 =
      builder.InternLocation(absl::bit_cast<const void*>(kAddress));
  auto profile = std::move(builder).Finalize();

  // There should be no mappings.
  EXPECT_TRUE(profile->mapping().empty());

  // There should be 1 location.
  ASSERT_EQ(profile->location().size(), 1);
  const auto& location = profile->location(0);
  EXPECT_EQ(location.id(), loc1);
  EXPECT_EQ(location.mapping_id(), 0);
  EXPECT_EQ(location.address(), kAddress);
}

TEST(ProfileBuilderTest, LocationTable) {
  ProfileBuilder builder;

  // Verify we add mapping information to locations correctly.
  builder.AddMapping(uintptr_t{0x200}, uintptr_t{0x300}, uintptr_t{0x123},
                     "foo.so", "abababab");

  // loc1/loc3 should lack mappings, loc2 should have a mapping.
  const int loc1 =
      builder.InternLocation(absl::bit_cast<const void*>(uintptr_t{0x150}));
  const int loc2 =
      builder.InternLocation(absl::bit_cast<const void*>(uintptr_t{0x250}));
  const int loc3 =
      builder.InternLocation(absl::bit_cast<const void*>(uintptr_t{0x350}));

  auto profile = std::move(builder).Finalize();

  // There should be one mapping.
  ASSERT_EQ(profile->mapping().size(), 1);
  const auto mapping = profile->mapping(0);
  EXPECT_EQ(mapping.memory_start(), 0x200);
  EXPECT_EQ(mapping.memory_limit(), 0x300);
  EXPECT_EQ(mapping.file_offset(), 0x123);
  EXPECT_EQ(profile->string_table(mapping.filename()), "foo.so");
  EXPECT_EQ(profile->string_table(mapping.build_id()), "abababab");

  struct SimpleLocation {
    uint64_t id;
    uint64_t mapping_id;
    uint64_t address;

    bool operator==(const SimpleLocation& rhs) const {
      return std::tie(id, mapping_id, address) ==
             std::tie(rhs.id, rhs.mapping_id, rhs.address);
    }
  };
  std::vector<SimpleLocation> actual;
  for (auto location : profile->location()) {
    SimpleLocation& l = actual.emplace_back();
    l.id = location.id();
    l.mapping_id = location.mapping_id();
    l.address = location.address();
  }
  std::vector<SimpleLocation> expected = {
      {static_cast<uint64_t>(loc1), 0, 0x150},
      {static_cast<uint64_t>(loc2), mapping.id(), 0x250},
      {static_cast<uint64_t>(loc3), 0, 0x350},
  };

  EXPECT_THAT(actual, testing::UnorderedElementsAreArray(expected));
}

TEST(ProfileBuilderTest, StringTable) {
  auto profile = ProfileBuilder().Finalize();

  ASSERT_FALSE(profile->string_table().empty());
  // The first entry should be the empty string.
  EXPECT_EQ(profile->string_table(0), "");

  // There should be no duplicates.
  absl::flat_hash_set<std::string> strings;
  strings.reserve(profile->string_table_size());
  strings.insert(profile->string_table().begin(),
                 profile->string_table().end());
  EXPECT_EQ(strings.size(), profile->string_table_size());
}

TEST(ProfileConverterTest, Profile) {
  constexpr int kPeriod = 1000;
  constexpr absl::Duration kDuration = absl::Milliseconds(1500);

  auto fake_profile = absl::make_unique<FakeProfile>();
  fake_profile->SetPeriod(kPeriod);
  fake_profile->SetType(ProfileType::kHeap);
  fake_profile->SetDuration(kDuration);

  std::vector<Profile::Sample> samples;

  {
    auto& sample = samples.emplace_back();

    sample.sum = 1234;
    sample.count = 2;
    sample.requested_size = 2;
    sample.requested_alignment = 4;
    sample.allocated_size = 16;
    // This stack is mostly artificial, but we include a real symbol from the
    // binary to confirm that at least one location was indexed into its
    // mapping.
    sample.depth = 5;
    sample.stack[0] = absl::bit_cast<void*>(uintptr_t{0x12345});
    sample.stack[1] = absl::bit_cast<void*>(uintptr_t{0x23451});
    sample.stack[2] = absl::bit_cast<void*>(uintptr_t{0x34512});
    sample.stack[3] = absl::bit_cast<void*>(uintptr_t{0x45123});
    sample.stack[4] = reinterpret_cast<void*>(&ProfileAccessor::MakeProfile);
  }

  {
    auto& sample = samples.emplace_back();

    sample.sum = 2345;
    sample.count = 5;
    sample.requested_size = 4;
    sample.requested_alignment = 0;
    sample.allocated_size = 8;
    // This stack is mostly artificial, but we include a real symbol from the
    // binary to confirm that at least one location was indexed into its
    // mapping.
    sample.depth = 4;
    sample.stack[0] = absl::bit_cast<void*>(uintptr_t{0x12345});
    sample.stack[1] = absl::bit_cast<void*>(uintptr_t{0x23451});
    sample.stack[2] = absl::bit_cast<void*>(uintptr_t{0x45123});
    sample.stack[3] = reinterpret_cast<void*>(&RealPath);
  }

  fake_profile->SetSamples(std::move(samples));

  Profile profile = ProfileAccessor::MakeProfile(std::move(fake_profile));
  auto converted_or = MakeProfileProto(profile);
  ASSERT_TRUE(converted_or.ok());
  const auto& converted = **converted_or;

  // Two sample types: [objects, count] and [space, bytes]
  std::vector<std::pair<std::string, std::string>> extracted_sample_type;
  for (const auto& s : converted.sample_type()) {
    auto& labels = extracted_sample_type.emplace_back();
    labels.first = converted.string_table(s.type());
    labels.second = converted.string_table(s.unit());
  }

  EXPECT_THAT(
      extracted_sample_type,
      UnorderedElementsAre(Pair("objects", "count"), Pair("space", "bytes")));

  // Strings
  ASSERT_FALSE(converted.string_table().empty());

  // Mappings: Build a lookup table from mapping ID to index in mapping array.
  ASSERT_FALSE(converted.mapping().empty());
  absl::flat_hash_map<uint64_t, int> mappings;
  for (int i = 0, n = converted.mapping().size(); i < n; i++) {
    mappings.emplace(converted.mapping(i).id(), i);
  }

  // Locations
  ASSERT_FALSE(converted.location().empty());
  absl::flat_hash_map<int, const void*> addresses;
  absl::flat_hash_set<int> interned_addresses;
  int location_with_mapping_found = 0;
  for (const auto& location : converted.location()) {
    uintptr_t address = location.address();
    if (location.mapping_id() > 0) {
      ASSERT_THAT(
          mappings,
          testing::Contains(testing::Key(testing::Eq(location.mapping_id()))));
      const int mapping_index = mappings.at(location.mapping_id());
      ASSERT_LT(mapping_index, converted.mapping_size());
      const auto& mapping = converted.mapping(mapping_index);

      location_with_mapping_found++;

      // Confirm address actually falls within [mapping.memory_start(),
      // mapping.memory_limit()).
      EXPECT_LE(mapping.memory_start(), address);
      EXPECT_LT(address, mapping.memory_limit());
    }

    EXPECT_TRUE(interned_addresses.insert(location.id()).second)
        << "Duplicate interned location ID found";
  }
  // Expect that we find at least 2 locations with a mapping.
  EXPECT_GE(location_with_mapping_found, 2);
  // Expect that no location has ID "0."
  EXPECT_THAT(interned_addresses, testing::Not(testing::Contains(0)));

  // Samples
  std::vector<std::vector<std::pair<std::string, int>>> extracted;
  for (const auto& s : converted.sample()) {
    EXPECT_FALSE(s.location_id().empty());
    // No duplicates
    EXPECT_THAT(
        absl::flat_hash_set<int>(s.location_id().begin(), s.location_id().end())
            .size(),
        s.location_id().size());
    // Interned locations should appear in the location list.
    EXPECT_THAT(s.location_id(), testing::IsSubsetOf(interned_addresses));

    EXPECT_EQ(converted.sample_type().size(), s.value().size());
    extracted.emplace_back();
    auto& labels = extracted.back();
    for (const auto& l : s.label()) {
      if (l.str() != 0) {
        // Skip string labels for now.
        continue;
      }

      labels.emplace_back(converted.string_table(l.key()), l.num());
    }
  }
  EXPECT_THAT(extracted,
              UnorderedElementsAre(
                  UnorderedElementsAre(Pair("bytes", 16), Pair("request", 2),
                                       Pair("alignment", 4)),
                  UnorderedElementsAre(Pair("bytes", 8), Pair("request", 4))));

  // The addresses for the samples at stack[0], stack[1] should match.
  ASSERT_GE(converted.sample().size(), 2);
  ASSERT_GE(converted.sample(0).location_id().size(), 2);
  ASSERT_GE(converted.sample(1).location_id().size(), 2);
  EXPECT_EQ(converted.sample(0).location_id(0),
            converted.sample(1).location_id(0));
  EXPECT_EQ(converted.sample(0).location_id(1),
            converted.sample(1).location_id(1));

  EXPECT_THAT(converted.string_table(converted.drop_frames()),
              testing::HasSubstr("TCMallocInternalNew"));
  // No keep frames.
  EXPECT_EQ(converted.string_table(converted.keep_frames()), "");

  EXPECT_EQ(converted.duration_nanos(), absl::ToInt64Nanoseconds(kDuration));

  // Period type [space, bytes]
  EXPECT_EQ(converted.string_table(converted.period_type().type()), "space");
  EXPECT_EQ(converted.string_table(converted.period_type().unit()), "bytes");

  // Period
  EXPECT_EQ(converted.period(), kPeriod);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
