// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_MEMFS_MALLOC_H_
#define TCMALLOC_MEMFS_MALLOC_H_

#include <string>

#include "absl/flags/declare.h"

ABSL_DECLARE_FLAG(std::string, memfs_malloc_path);

#endif  // TCMALLOC_MEMFS_MALLOC_H_
