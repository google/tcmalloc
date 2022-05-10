// Copyright 2020 The TCMalloc Authors
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

#include "tcmalloc/internal/config.h"

#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

#ifdef __x86_64__
TEST(AddressBits, CpuVirtualBits) {
  // Check that kAddressBits is as least as large as either the number of bits
  // in a pointer or as the number of virtual bits handled by the processor.
  // To be effective this test must be run on each processor model.
  const int kPointerBits = 8 * sizeof(void*);

  // LLVM has a miscompile bug around %rbx, see
  // https://bugs.llvm.org/show_bug.cgi?id=17907
  int ret;
  asm("mov %%rbx, %%rdi\n"
      "cpuid\n"
      "xchg %%rdi, %%rbx\n"
      /* inputs */
      : "=a"(ret)
      /* outputs */
      : "a"(0x80000008)
      /* clobbers */
      : "rdi", "ecx", "edx");
  const int kImplementedVirtualBits = (ret >> 8) & ((1 << 8) - 1);

  ASSERT_GE(kAddressBits, std::min(kImplementedVirtualBits, kPointerBits));
}
#endif

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
