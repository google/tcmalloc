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

#include "tcmalloc/internal/profile_builder.h"

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif  // defined(__linux__)

#include <cstddef>

#include "fuzztest/fuzztest.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

void ParseBuildID(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();
#if defined(__linux__)
  ElfW(Phdr) note;
  note.p_type = PT_NOTE;
  note.p_vaddr = reinterpret_cast<ElfW(Addr)>(nullptr);
  note.p_filesz = size;
  note.p_memsz = size;
  note.p_align = 4;

  dl_phdr_info info = {};
  info.dlpi_name = "test";
  info.dlpi_addr = reinterpret_cast<ElfW(Addr)>(data);
  info.dlpi_phdr = &note;
  info.dlpi_phnum = 1;

  GetBuildId(&info);
#endif  // defined(__linux__)
}

FUZZ_TEST(ProfileBuilderTest, ParseBuildID)
    ;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
