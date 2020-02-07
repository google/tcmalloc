// Copyright 2019 The TCMalloc Authors
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
//
// This .h file imports the code that causes tcmalloc to override libc
// versions of malloc/free/new/delete/etc.  That is, it provides the
// logic that makes it so calls to malloc(10) go through tcmalloc,
// rather than the default (libc) malloc.
//
// This file also provides a method: ReplaceSystemAlloc(), that every
// libc_override_*.h file it #includes is required to provide.  This
// is called when first setting up tcmalloc -- that is, when a global
// constructor in tcmalloc.cc is executed -- to do any initialization
// work that may be required for this OS.  (Note we cannot entirely
// control when tcmalloc is initialized, and the system may do some
// mallocs and frees before this routine is called.)  It may be a
// noop.
//
// Every libc has its own way of doing this, and sometimes the compiler
// matters too, so we have a different file for each libc, and often
// for different compilers and OS's.

#ifndef TCMALLOC_LIBC_OVERRIDE_H_
#define TCMALLOC_LIBC_OVERRIDE_H_

#include <features.h>

#include "tcmalloc/tcmalloc.h"

static void ReplaceSystemAlloc();  // defined in the .h files below

#if defined(OS_WINDOWS)
// We don't do any overriding on windows.  Just provide a dummy function.
static void ReplaceSystemAlloc() { }

#elif defined(__GLIBC__)
#include "tcmalloc/libc_override_glibc.h"

#else
#error Need to add support for your libc/OS here

#endif

#endif  // TCMALLOC_LIBC_OVERRIDE_H_
