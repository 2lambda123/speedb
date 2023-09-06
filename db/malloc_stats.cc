//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/malloc_stats.h"

#include <string.h>

#include <atomic>
#include <memory>

#include "port/jemalloc_helper.h"

namespace ROCKSDB_NAMESPACE {
struct spd_alloc_info {
  std::atomic<uint64_t> mem;    // Memory currently allocated
  std::atomic<uint64_t> count;  // Count of current allocations
  std::atomic<uint64_t> total;  // total number of allocations
};

static spd_alloc_info spd_alloc;

#ifdef ROCKSDB_JEMALLOC

struct MallocStatus {
  char* cur;
  char* end;
};

static void GetJemallocStatus(void* mstat_arg, const char* status) {
  MallocStatus* mstat = reinterpret_cast<MallocStatus*>(mstat_arg);
  size_t status_len = status ? strlen(status) : 0;
  size_t buf_size = (size_t)(mstat->end - mstat->cur);
  if (!status_len || status_len > buf_size) {
    return;
  }

  snprintf(mstat->cur, buf_size, "%s", status);
  mstat->cur += status_len;
}
void DumpMallocStats(std::string* stats) {
  if (!HasJemalloc()) {
    return;
  }
  MallocStatus mstat;
  const unsigned int kMallocStatusLen = 1000000;
  std::unique_ptr<char[]> buf{new char[kMallocStatusLen + 1]};
  mstat.cur = buf.get();
  mstat.end = buf.get() + kMallocStatusLen;
  malloc_stats_print(GetJemallocStatus, &mstat, "");
  stats->append(buf.get());
}
#else
void DumpMallocStats(std::string* str) {
  char buf[100];

  sprintf(buf, "count=%ld memory=%ldMB total=%ld", spd_alloc.count.load(),
          spd_alloc.mem.load() / 1024 / 1024, spd_alloc.total.load());

  *str = buf;
}
#endif  // ROCKSDB_JEMALLOC
static inline void spd_accounting_(void* p, bool alloc) {
  uint64_t [[maybe_unused]] real_size = (*((uintptr_t*)p - 1)) & ~7;
  auto info = &ROCKSDB_NAMESPACE::spd_alloc;

  if (alloc) {
    info->mem += real_size;
    info->count++;
    info->total++;
  } else {
    info->mem -= real_size;
    info->count--;
  }
}

}  // namespace ROCKSDB_NAMESPACE

#if defined(OS_LINUX) && !defined(ROCKSDB_JEMALLOC)

void* operator new(size_t size) {
  void* p = malloc(size);
  ROCKSDB_NAMESPACE::spd_accounting_(p, true);
  return p;
}
void* operator new[](size_t size) {
  void* p = malloc(size);
  ROCKSDB_NAMESPACE::spd_accounting_(p, true);
  return p;
}

void operator delete(void* p) {
  ROCKSDB_NAMESPACE::spd_accounting_(p, false);
  free(p);
}

void operator delete[](void* p) {
  ROCKSDB_NAMESPACE::spd_accounting_(p, false);
  free(p);
}

void operator delete(void* p, [[maybe_unused]] size_t size) {
  ROCKSDB_NAMESPACE::spd_accounting_(p, false);
  free(p);
}

void operator delete[](void* p, [[maybe_unused]] size_t size) {
  ROCKSDB_NAMESPACE::spd_accounting_(p, false);
  free(p);
}
#endif  // OS_LINUX