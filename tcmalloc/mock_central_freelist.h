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

#ifndef TCMALLOC_MOCK_CENTRAL_FREELIST_H_
#define TCMALLOC_MOCK_CENTRAL_FREELIST_H_

#include <stddef.h>

#include "gmock/gmock.h"

namespace tcmalloc {

class MockCentralFreeList {
 public:
  MockCentralFreeList() : size_class_(0) {
    ON_CALL(*this, Init).WillByDefault([this](size_t size_class) {
      size_class_ = size_class;
    });
    ON_CALL(*this, size_class).WillByDefault([this]() { return size_class_; });
    ON_CALL(*this, length).WillByDefault([]() { return 0; });
    ON_CALL(*this, OverheadBytes).WillByDefault([]() { return 0; });
  }

  MockCentralFreeList(const MockCentralFreeList&) = delete;
  MockCentralFreeList& operator=(const MockCentralFreeList&) = delete;

  MOCK_METHOD(void, Init, (size_t cl));
  MOCK_METHOD(void, InsertRange, (void** batch, int N));
  MOCK_METHOD(int, RemoveRange, (void** batch, int N));
  MOCK_METHOD(size_t, length, ());
  MOCK_METHOD(size_t, OverheadBytes, ());
  MOCK_METHOD(size_t, size_class, (), (const));

 private:
  size_t size_class_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_MOCK_CENTRAL_FREELIST_H_
