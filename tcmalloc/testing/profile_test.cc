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

#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "tcmalloc/internal/profile.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/config.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/profile_marshaler.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(ProfileTest, HeapProfile) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipping heap profile test under sanitizers.";
#endif

  constexpr int64_t kSamplingRate = 1024 * 1024;
  ScopedProfileSamplingRate s(kSamplingRate);

  // Tweak alloc_size to make it more likely we can distinguish it from others.
  constexpr int kAllocs = 32;
  const size_t alloc_size = 64 * kSamplingRate + 123;

  auto deleter = [](void* ptr) { ::operator delete(ptr); };
  std::vector<std::unique_ptr<void, decltype(deleter)>> allocs;
  for (int i = 0; i < kAllocs; i++) {
    allocs.emplace_back(::operator new(alloc_size), deleter);
  }

  // Grab profile, encode, then decode to look for the allocations.
  Profile profile = MallocExtension::SnapshotCurrent(ProfileType::kHeap);
  absl::StatusOr<std::string> encoded_or = Marshal(profile);
  ASSERT_TRUE(encoded_or.ok());

  const absl::string_view encoded = *encoded_or;

  google::protobuf::io::ArrayInputStream stream(encoded.data(), encoded.size());
  google::protobuf::io::GzipInputStream gzip_stream(&stream);
  google::protobuf::io::CodedInputStream coded(&gzip_stream);

  perftools::profiles::Profile converted;
  ASSERT_TRUE(converted.ParseFromCodedStream(&coded));

  size_t count = 0, bytes = 0;
  for (const auto& sample : converted.sample()) {
    count += sample.value(0);
    bytes += sample.value(1);
  }

  ASSERT_GT(count, 0);
  ASSERT_GE(bytes, alloc_size * kAllocs);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
