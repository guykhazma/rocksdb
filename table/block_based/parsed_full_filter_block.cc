//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//

#include "table/block_based/parsed_full_filter_block.h"

#include "table/block_based/filter_policy_internal.h"

namespace ROCKSDB_NAMESPACE {

ParsedFullFilterBlock::ParsedFullFilterBlock(const FilterPolicy* filter_policy,
                                             BlockContents&& contents)
    : block_contents_(std::move(contents)),
      filter_bits_reader_(
          !block_contents_.data.empty()
              ? filter_policy->GetFilterBitsReader(block_contents_.data)
              : nullptr) {
  // A reader that copied what it needs (Diva's) leaves the block unused:
  // holding it would keep the filter in memory twice.
  if (filter_bits_reader_ != nullptr &&
      filter_bits_reader_->CanReleaseBackingFilterBlock()) {
    block_contents_ = BlockContents();
    block_released_ = true;
  }
}

ParsedFullFilterBlock::~ParsedFullFilterBlock() = default;

size_t ParsedFullFilterBlock::ApproximateMemoryUsage() const {
  return block_contents_.ApproximateMemoryUsage() +
         (filter_bits_reader_ != nullptr
              ? filter_bits_reader_->ApproximateMemoryUsage()
              : 0);
}

}  // namespace ROCKSDB_NAMESPACE
