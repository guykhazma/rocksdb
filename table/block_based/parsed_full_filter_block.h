//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cassert>
#include <memory>

#include "table/block_based/block_type.h"
#include "table/format.h"

namespace ROCKSDB_NAMESPACE {

class FilterBitsReader;
class FilterPolicy;

// The sharable/cachable part of the full filter.
class ParsedFullFilterBlock {
 public:
  ParsedFullFilterBlock(const FilterPolicy* filter_policy,
                        BlockContents&& contents);
  ~ParsedFullFilterBlock();

  FilterBitsReader* filter_bits_reader() const {
    return filter_bits_reader_.get();
  }

  // The block (unless released) and what the reader holds besides it.
  size_t ApproximateMemoryUsage() const;

  // A reader that released the block owns everything it needs.
  bool own_bytes() const {
    return block_released_ || block_contents_.own_bytes();
  }

  // For TypedCacheInterface. A released block has no contents to save, so a
  // filter whose reader releases it cannot go to a secondary cache.
  const Slice& ContentSlice() const {
    assert(!block_released_);
    return block_contents_.data;
  }
  static constexpr CacheEntryRole kCacheEntryRole =
      CacheEntryRole::kFilterBlock;
  static constexpr BlockType kBlockType = BlockType::kFilter;

 private:
  BlockContents block_contents_;
  std::unique_ptr<FilterBitsReader> filter_bits_reader_;
  // The reader kept nothing from the block, so the block was freed.
  bool block_released_ = false;
};

}  // namespace ROCKSDB_NAMESPACE
