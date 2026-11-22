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

#ifndef TCMALLOC_INTERNAL_PROFILE_BUILDER_H_
#define TCMALLOC_INTERNAL_PROFILE_BUILDER_H_

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif  // defined(__linux__)

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "tcmalloc/internal/profile.pb.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {

#if defined(__linux__)
[[nodiscard]] std::string GetBuildId(const dl_phdr_info* info);
#endif  // defined(__linux__)

// ProfileBuilder manages building up a profile.proto instance and populating
// common parts using the string/pointer table conventions expected by pprof.
class ProfileBuilder {
 public:
  ProfileBuilder();

  [[nodiscard]] perftools::profiles::Profile& profile() { return *profile_; }

  // Adds the current process mappings to the profile.
  void AddCurrentMappings();

  // Adds a single mapping to the profile and to lookup cache and returns the
  // resulting ID.
  int AddMapping(uintptr_t memory_start, uintptr_t memory_limit,
                 uintptr_t file_offset, absl::string_view filename,
                 absl::string_view build_id);

  // Add documentation URL.
  absl::Status SetDocURL(absl::string_view url);

  // Interns sv in the profile's string table and returns the resulting ID.
  [[nodiscard]] int InternString(absl::string_view sv);
  // Interns a location in the profile's location table and returns the
  // resulting ID.
  [[nodiscard]] int InternLocation(const void* ptr);

  // Interns a callstack and adds the IDs to the provided sample.
  void InternCallstack(absl::Span<const void* const> stack,
                       perftools::profiles::Sample& sample);

  [[nodiscard]] std::unique_ptr<perftools::profiles::Profile> Finalize() &&;

 private:
  std::unique_ptr<perftools::profiles::Profile> profile_;
  // mappings_ stores the start address of each mapping in profile_->mapping()
  // to its index.
  absl::btree_map<uintptr_t, int> mappings_;
  absl::flat_hash_map<std::string, int> strings_;
  absl::flat_hash_map<uintptr_t, int> locations_;
};

extern const absl::string_view kProfileDropFrames;

[[nodiscard]] absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>>
MakeProfileProto(const ::tcmalloc::Profile& profile);

class PageFlagsBase;
class PageFlags;
class Residency;

// Exposed to facilitate testing. Analyzes every compressibility candidate.
[[nodiscard]] absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>>
MakeProfileProto(const ::tcmalloc::Profile& profile, PageFlagsBase* pageflags,
                 Residency* residency);

// Exposed to facilitate testing. Analyzes each compressibility candidate with
// probability `compressibility_fraction` (see CompressibilityThrottle).
[[nodiscard]] absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>>
MakeProfileProto(const ::tcmalloc::Profile& profile, PageFlagsBase* pageflags,
                 Residency* residency, double compressibility_fraction);

// Bounds how often heap profile conversions read sampled spans for
// compressibility analysis, independently of how often heap profiles are
// requested.
//
// Compressibility analysis copies the contents of sampled resident spans, which
// sets their pages' accessed bits. kstaled then treats those pages as recently
// used and `stale_space` reports them as not stale for the next one or two scan
// periods. A process that converts heap profiles more often than the stale scan
// period would therefore hide the staleness of its sampled pages from every
// other heap profile. To keep the average rate at which a sampled span is read
// at about once per `period` regardless of the number of callers, each
// conversion analyzes each candidate with probability
// min(1, elapsed / period), where `elapsed` is the time since the previous
// conversion that collected compressibility in this process.
class CompressibilityThrottle {
 public:
  constexpr CompressibilityThrottle() = default;

  // Returns the fraction of compressibility candidates to analyze for a
  // conversion at `now`, and records `now` as the latest conversion. Returns 1
  // for the first conversion, when `period` is not positive, or when at least
  // `period` has elapsed; 0 if `now` is not after the previous conversion.
  double Fraction(absl::Time now, absl::Duration period);

 private:
  // absl::ToUnixNanos() of the previous conversion; 0 if there was none.
  std::atomic<int64_t> last_conversion_nanos_{0};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_PROFILE_BUILDER_H_
