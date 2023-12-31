// Copyright (C) 2022 Speedb Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include "rocksdb/filter_policy.h"

namespace ROCKSDB_NAMESPACE {

// Forward Declarations
class ObjectLibrary;
struct FilterBuildingContext;

// In the default cache-local bloom filter in RocksDB
// (FastLocalBloomFilterPolicy) the trade-off between memory and false positive
// rate is significantly worse than the theoretical standard bloom filter,
// however it is significantly faster in terms of CPU. This trade-off
// deteriorates performance/memory footprint especially in use cases in which
// large accuracy of the filter is needed (typically from ~20 bits-per-key).
//
// For really high bits-per-key there could be orders of magnitude difference in
// the false positive rate. Ribbon filter is generally better than bloom filter
// in the trade-off (takes ~30% less memory to obtain the same false positive
// rate. However, its construction and use is slower by a factor of ~4 than
// bloom filter, so in use cases that require fast testing and construction
// ribbon filter cannot be used.
//
// This filter is fast and low on CPU consumption on the one hand, but with a
// better memory footprint- FPR trade-off on the other hand.
//
class SpdbPairedBloomFilterPolicy : public FilterPolicy {
 public:
  // Max supported BPK. Filters using higher BPK-s will use the max
  static constexpr double kMaxBitsPerKey = 100.0;

 public:
  explicit SpdbPairedBloomFilterPolicy(double bits_per_key);

  FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext& context) const override;

  FilterBitsReader* GetFilterBitsReader(const Slice& contents) const override;

  // Plug-In Support
 public:
  static const char* kClassName();
  const char* Name() const override { return kClassName(); }
  static const char* kNickName();
  const char* NickName() const override { return kNickName(); }

  std::string GetId() const override;

  bool IsInstanceOf(const std::string& name) const override;

  // This filter is NOT compatible with RocksDB's built-in filter, only with
  // itself
  const char* CompatibilityName() const override {
    return kCompatibilityName();
  }
  static const char* kCompatibilityName() { return kClassName(); }

 private:
  // This filter supports fractional bits per key. For predictable behavior
  // of 0.001-precision values across floating point implementations, we
  // round to thousandths of a bit (on average) per key.
  int millibits_per_key_;

  // State for implementing optimize_filters_for_memory. Essentially, this
  // tracks a surplus or deficit in total FP rate of filters generated by
  // builders under this policy vs. what would have been generated without
  // optimize_filters_for_memory.
  //
  // To avoid floating point weirdness, the actual value is
  //  Sum over all generated filters f:
  //   (predicted_fp_rate(f) - predicted_fp_rate(f|o_f_f_m=false)) * 2^32
  mutable std::atomic<int64_t> aggregate_rounding_balance_;
};

// Plug-In Support
extern "C" {
int register_SpdbPairedBloomFilter(ROCKSDB_NAMESPACE::ObjectLibrary& library,
                                   const std::string&);
}  // extern "C"

}  // namespace ROCKSDB_NAMESPACE
