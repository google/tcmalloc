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

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif  // defined(__linux__)
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "tcmalloc/internal/profile.pb.h"
#include "absl/base/attributes.h"
#include "absl/base/macros.h"
#include "absl/numeric/bits.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

#if defined(__linux__)
// Returns the Phdr of the first segment of the given type.
const ElfW(Phdr) *
    GetFirstSegment(const dl_phdr_info* const info, const int segment_type) {
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == segment_type) {
      return &info->dlpi_phdr[i];
    }
  }
  return nullptr;
}

// Extracts the linker provided build ID from the PT_NOTE segment found in info.
//
// On failure, returns an empty string.
std::string GetBuildId(const dl_phdr_info* const info) {
  const ElfW(Phdr)* pt_note = GetFirstSegment(info, PT_NOTE);
  if (pt_note == nullptr) {
    // Failed to find note segment.
    return "";
  }
  std::string result;

  // pt_note contains entries (of type ElfW(Nhdr)) starting at
  //   info->dlpi_addr + pt_note->p_vaddr
  // with length
  //   pt_note->p_memsz
  //
  // The length of each entry is given by
  //   sizeof(ElfW(Nhdr)) + AlignTo4Bytes(nhdr->n_namesz) +
  //   AlignTo4Bytes(nhdr->n_descsz)
  const char* note =
      reinterpret_cast<char*>(info->dlpi_addr + pt_note->p_vaddr);
  const char* const last = note + pt_note->p_memsz;
  while (note < last) {
    const ElfW(Nhdr)* const nhdr = reinterpret_cast<const ElfW(Nhdr)*>(note);
    if (nhdr->n_type == NT_GNU_BUILD_ID) {
      const char* const note_name = note + sizeof(*nhdr);
      // n_namesz is the length of note_name.
      if (nhdr->n_namesz == 4 && memcmp(note_name, "GNU\0", 4) == 0) {
        if (!result.empty()) {
          // Repeated build-ids.  Ignore them.
          return "";
        }
        const char* note_data = reinterpret_cast<const char*>(nhdr) +
                                sizeof(*nhdr) + nhdr->n_namesz;
        result = absl::BytesToHexString(
            absl::string_view(note_data, nhdr->n_descsz));
      }
    }
    // Both name and desc are 4-byte aligned (in 32 and 64-bit mode).
    const int name_size = (nhdr->n_namesz + 3) & ~3;
    const int desc_size = (nhdr->n_descsz + 3) & ~3;
    note += name_size + desc_size + sizeof(*nhdr);
  }
  return result;
}

// Return DT_SONAME for the given image.  If there is no PT_DYNAMIC or if
// PT_DYNAMIC does not contain DT_SONAME, return nullptr.
static const char* GetSoName(const dl_phdr_info* const info) {
  const ElfW(Phdr)* const pt_dynamic = GetFirstSegment(info, PT_DYNAMIC);
  if (pt_dynamic == nullptr) {
    return nullptr;
  }
  const ElfW(Dyn)* dyn =
      reinterpret_cast<ElfW(Dyn)*>(info->dlpi_addr + pt_dynamic->p_vaddr);
  const ElfW(Dyn)* dt_strtab = nullptr;
  const ElfW(Dyn)* dt_strsz = nullptr;
  const ElfW(Dyn)* dt_soname = nullptr;
  for (; dyn->d_tag != DT_NULL; ++dyn) {
    if (dyn->d_tag == DT_SONAME) {
      dt_soname = dyn;
    } else if (dyn->d_tag == DT_STRTAB) {
      dt_strtab = dyn;
    } else if (dyn->d_tag == DT_STRSZ) {
      dt_strsz = dyn;
    }
  }
  if (dt_soname == nullptr) {
    return nullptr;
  }
  CHECK_CONDITION(dt_strtab != nullptr);
  CHECK_CONDITION(dt_strsz != nullptr);
  const char* const strtab =
      reinterpret_cast<char*>(info->dlpi_addr + dt_strtab->d_un.d_val);
  CHECK_CONDITION(dt_soname->d_un.d_val < dt_strsz->d_un.d_val);
  return strtab + dt_soname->d_un.d_val;
}
#endif  // defined(__linux__)

uintptr_t RoundUpToPageSize(uintptr_t address) {
  const uintptr_t pagesize = sysconf(_SC_PAGESIZE);
  CHECK_CONDITION(absl::has_single_bit(pagesize));

  const uintptr_t pagesize_all_bits = pagesize - 1;

  address = address + pagesize_all_bits;
  return address & ~pagesize_all_bits;
}

}  // namespace

ABSL_CONST_INIT const absl::string_view kProfileDropFrames =
    // POSIX entry points.
    "calloc|"
    "cfree|"
    "malloc|"
    "free|"
    "memalign|"
    "do_memalign|"
    "(__)?posix_memalign|"
    "pvalloc|"
    "valloc|"
    "realloc|"

    // TCMalloc.
    "tcmalloc::.*|"
    "TCMallocInternalCalloc|"
    "TCMallocInternalCfree|"
    "TCMallocInternalMalloc|"
    "TCMallocInternalFree|"
    "TCMallocInternalMemalign|"
    "TCMallocInternalAlignedAlloc|"
    "TCMallocInternalPosixMemalign|"
    "TCMallocInternalPvalloc|"
    "TCMallocInternalValloc|"
    "TCMallocInternalRealloc|"
    "TCMallocInternalNew(Array)?(Aligned)?(Nothrow)?|"
    "TCMallocInternalDelete(Array)?(Sized)?(Aligned)?(Nothrow)?|"
    "TCMallocInternalSdallocx|"
    "(tcmalloc_)?size_returning_operator_new(_nothrow)?|"

    // libstdc++ memory allocation routines
    "__gnu_cxx::new_allocator::allocate|"
    "__malloc_alloc_template::allocate|"
    "_M_allocate|"

    // libc++ memory allocation routines
    "std::__u::__libcpp_allocate|"
    "std::__u::allocator::allocate|"
    "std::__u::allocator_traits::allocate|"

    // Other misc. memory allocation routines
    "(::)?do_malloc_pages|"
    "(::)?do_realloc|"
    "__builtin_(vec_)?delete|"
    "__builtin_(vec_)?new|"
    "__libc_calloc|"
    "__libc_malloc|"
    "__libc_memalign|"
    "__libc_realloc|"
    "(::)?slow_alloc|"
    "fast_alloc|"
    "(::)?AllocSmall|"
    "operator new(\\[\\])?";

ProfileBuilder::ProfileBuilder()
    : profile_(absl::make_unique<perftools::profiles::Profile>()) {
  // string_table[0] must be ""
  profile_->add_string_table("");
}

int ProfileBuilder::InternString(absl::string_view sv) {
  if (sv.empty()) {
    return 0;
  }

  const int index = profile_->string_table_size();
  const auto inserted = strings_.emplace(sv, index);
  if (!inserted.second) {
    // Failed to insert -- use existing id.
    return inserted.first->second;
  }
  profile_->add_string_table(inserted.first->first);
  return index;
}

int ProfileBuilder::InternLocation(const void* ptr) {
  uintptr_t address = absl::bit_cast<uintptr_t>(ptr);

  // Avoid assigning location ID 0 by incrementing by 1.
  const int index = profile_->location_size() + 1;
  const auto inserted = locations_.emplace(address, index);
  if (!inserted.second) {
    // Failed to insert -- use existing id.
    return inserted.first->second;
  }
  perftools::profiles::Location& location = *profile_->add_location();
  ASSERT(inserted.first->second == index);
  location.set_id(index);
  location.set_address(address);

  if (mappings_.empty()) {
    return index;
  }

  // Find the mapping ID.
  auto it = mappings_.upper_bound(address);
  if (it != mappings_.begin()) {
    --it;
  }

  // If *it contains address, add mapping to location.
  const int mapping_index = it->second;
  const perftools::profiles::Mapping& mapping =
      profile_->mapping(mapping_index);
  const int mapping_id = mapping.id();
  ASSERT(it->first == mapping.memory_start());

  if (it->first <= address && address < mapping.memory_limit()) {
    location.set_mapping_id(mapping_id);
  }

  return index;
}

void ProfileBuilder::InternCallstack(absl::Span<const void* const> stack,
                                     perftools::profiles::Sample& sample) {
  // Profile addresses are raw stack unwind addresses, so they should be
  // adjusted by -1 to land inside the call instruction (although potentially
  // misaligned).
  for (const void* frame : stack) {
    int id = InternLocation(
        absl::bit_cast<const void*>(absl::bit_cast<uintptr_t>(frame) - 1));
    sample.add_location_id(id);
  }
  ASSERT(sample.location_id().size() == stack.size());
}

void ProfileBuilder::AddCurrentMappings() {
#if defined(__linux__)
  auto dl_iterate_callback = +[](dl_phdr_info* info, size_t size, void* data) {
    // Skip dummy entry introduced since glibc 2.18.
    if (info->dlpi_phdr == nullptr && info->dlpi_phnum == 0) {
      return 0;
    }

    ProfileBuilder& builder = *static_cast<ProfileBuilder*>(data);
    const ElfW(Phdr)* pt_load = GetFirstSegment(info, PT_LOAD);
    CHECK_CONDITION(pt_load != nullptr);

    // Extract data.
    const size_t memory_start = info->dlpi_addr + pt_load->p_vaddr;
    const size_t memory_limit =
        RoundUpToPageSize(memory_start + pt_load->p_memsz);
    const size_t file_offset = pt_load->p_offset;

    // Storage for path to executable as dlpi_name isn't populated for the main
    // executable.  +1 to allow for the null terminator that readlink does not
    // add.
    char self_filename[PATH_MAX + 1];
    const char* filename = info->dlpi_name;
    if (filename == nullptr || filename[0] == '\0') {
      // This is either the main executable or the VDSO.  The main executable is
      // always the first entry processed by callbacks.
      if (builder.profile_->mapping_size() == 0) {
        // This is the main executable.
        ssize_t ret = readlink("/proc/self/exe", self_filename,
                               sizeof(self_filename) - 1);
        if (ret >= 0 && ret < sizeof(self_filename)) {
          self_filename[ret] = '\0';
          filename = self_filename;
        }
      } else {
        // This is the VDSO.
        filename = GetSoName(info);
      }
    }

    char resolved_path[PATH_MAX];
    absl::string_view resolved_filename;
    if (realpath(filename, resolved_path)) {
      resolved_filename = resolved_path;
    } else {
      resolved_filename = filename;
    }

    const std::string build_id = GetBuildId(info);

    // Add to profile.
    builder.AddMapping(memory_start, memory_limit, file_offset,
                       resolved_filename, build_id);

    // Keep going.
    return 0;
  };

  dl_iterate_phdr(dl_iterate_callback, this);
#endif  // defined(__linux__)
}

void ProfileBuilder::AddMapping(uintptr_t memory_start, uintptr_t memory_limit,
                                uintptr_t file_offset,
                                absl::string_view filename,
                                absl::string_view build_id) {
  perftools::profiles::Mapping& mapping = *profile_->add_mapping();
  mapping.set_id(profile_->mapping_size());
  mapping.set_memory_start(memory_start);
  mapping.set_memory_limit(memory_limit);
  mapping.set_file_offset(file_offset);
  mapping.set_filename(InternString(filename));
  mapping.set_build_id(InternString(build_id));

  mappings_.emplace(memory_start, mapping.id() - 1);
}

std::unique_ptr<perftools::profiles::Profile> ProfileBuilder::Finalize() && {
  return std::move(profile_);
}

absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>> MakeProfileProto(
    const ::tcmalloc::Profile& profile) {
  ProfileBuilder builder;
  builder.AddCurrentMappings();

  const int alignment_id = builder.InternString("alignment");
  const int bytes_id = builder.InternString("bytes");
  const int count_id = builder.InternString("count");
  const int objects_id = builder.InternString("objects");
  const int request_id = builder.InternString("request");
  const int space_id = builder.InternString("space");

  perftools::profiles::Profile& converted = builder.profile();

  perftools::profiles::ValueType& period_type =
      *converted.mutable_period_type();
  period_type.set_type(space_id);
  period_type.set_unit(bytes_id);
  converted.set_period(profile.Period());
  converted.set_drop_frames(builder.InternString(kProfileDropFrames));

  converted.set_duration_nanos(absl::ToInt64Nanoseconds(profile.Duration()));

  {
    perftools::profiles::ValueType& sample_type = *converted.add_sample_type();
    sample_type.set_type(objects_id);
    sample_type.set_unit(count_id);
  }

  {
    perftools::profiles::ValueType& sample_type = *converted.add_sample_type();
    sample_type.set_type(space_id);
    sample_type.set_unit(bytes_id);
  }

  int default_sample_type_id;
  switch (profile.Type()) {
    case tcmalloc::ProfileType::kFragmentation:
    case tcmalloc::ProfileType::kHeap:
    case tcmalloc::ProfileType::kPeakHeap:
      default_sample_type_id = space_id;
      break;
    case tcmalloc::ProfileType::kAllocations:
      default_sample_type_id = objects_id;
      break;
    default:
      return absl::InvalidArgumentError("Unexpected profile format");
  }

  converted.set_default_sample_type(default_sample_type_id);

  profile.Iterate([&](const tcmalloc::Profile::Sample& entry) {
    perftools::profiles::Profile& profile = builder.profile();
    perftools::profiles::Sample& sample = *profile.add_sample();

    CHECK_CONDITION(entry.depth <= ABSL_ARRAYSIZE(entry.stack));
    builder.InternCallstack(absl::MakeSpan(entry.stack, entry.depth), sample);

    sample.add_value(entry.count);
    sample.add_value(entry.sum);

    // add fields that are common to all memory profiles
    if (entry.count > 0) {
      perftools::profiles::Label& label = *sample.add_label();
      label.set_key(bytes_id);
      label.set_num(entry.allocated_size);
      label.set_num_unit(bytes_id);
    }

    auto add_positive_label = [&](int key, int unit, size_t value) {
      if (value == 0) return;
      perftools::profiles::Label& label = *sample.add_label();
      label.set_key(key);
      label.set_num(value);
      label.set_num_unit(unit);
    };

    add_positive_label(request_id, bytes_id, entry.requested_size);
    add_positive_label(alignment_id, bytes_id, entry.requested_alignment);
  });

  return std::move(builder).Finalize();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
