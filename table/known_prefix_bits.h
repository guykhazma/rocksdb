//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
//  Created 2026-10-03.

// Comparisons for approximate lower bounds given as a known bit prefix.
//
// A range filter can bound the first key it holds at or after a target
// without reading the table: it returns the first `known_bits` bits of that
// key, most significant bit first, as bytes whose remaining bits are zero.
// Only the known bits carry information; the zero padding must never be
// compared as if it were part of a key. Keys compare bytewise.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "rocksdb/slice.h"

namespace ROCKSDB_NAMESPACE {

// What a range filter knows about the keys a table (or a child iterator of a
// merge) holds at or after a target, without reading them.
struct KeyLowerBound {
  // No key >= target.
  bool empty = false;
  // Otherwise: every key >= target starts with, or sorts after, the first
  // `known_bits` bits of `prefix` (padded as by TruncateToKnownPrefixBits).
  // known_bits == 0 says nothing.
  std::string prefix;
  uint32_t known_bits = 0;
};

// The first `known_bits` bits of `key`, in ceil(known_bits / 8) bytes with
// the bits after them zeroed. A key shorter than that is padded with zeros.
inline std::string TruncateToKnownPrefixBits(const Slice& key,
                                             uint32_t known_bits) {
  if (known_bits == 0) {
    return std::string();
  }
  const size_t num_bytes = (known_bits + 7) / 8;
  std::string out(num_bytes, '\0');
  const size_t copied = std::min(key.size(), num_bytes);
  if (copied > 0) {
    memcpy(&out[0], key.data(), copied);
  }
  const uint32_t partial_bits = known_bits & 7;
  if (partial_bits != 0) {
    const uint8_t mask = static_cast<uint8_t>(0xff << (8 - partial_bits));
    out.back() = static_cast<char>(static_cast<uint8_t>(out.back()) & mask);
  }
  return out;
}

// Diva compares keys as if padded with zero bytes to a common length, so the
// bits it reports as known can run past the end of a shorter key, into its
// padding. Those can only be zeros. Dropping the trailing zero bits leaves
// bits that every key the bound covers really has. Returns the number of
// known bits left, up to and including the last one bit of the first
// `known_bits` bits of `prefix` (0 if they are all zero).
inline uint32_t KnownBitsWithoutTrailingZeros(const Slice& prefix,
                                              uint32_t known_bits) {
  while (known_bits > 0) {
    const uint32_t bit = known_bits - 1;
    if (bit / 8 < prefix.size() &&
        (static_cast<uint8_t>(prefix[bit / 8]) >> (7 - bit % 8)) & 1) {
      break;
    }
    --known_bits;
  }
  return known_bits;
}

// Compares the first `known_bits` bits of `prefix` with `key`: negative if
// every key starting with those bits sorts before `key`, positive if every
// such key sorts after it, and 0 if `key` itself starts with them (then keys
// with that prefix can sort on either side). `prefix` holds at least
// ceil(known_bits / 8) bytes.
inline int CompareKnownPrefixBits(const Slice& prefix, uint32_t known_bits,
                                  const Slice& key) {
  if (known_bits == 0) {
    return 0;
  }
  const size_t whole_bytes = known_bits / 8;
  const size_t common = std::min(whole_bytes, key.size());
  const int byte_cmp =
      common == 0 ? 0 : memcmp(prefix.data(), key.data(), common);
  if (byte_cmp != 0) {
    return byte_cmp < 0 ? -1 : +1;
  }
  if (key.size() < whole_bytes) {
    // `key` ends inside the known bytes and matches them so far: every key
    // with the prefix is longer, hence greater.
    return +1;
  }
  const uint32_t partial_bits = known_bits & 7;
  if (partial_bits != 0) {
    if (key.size() == whole_bytes) {
      return +1;  // the same, one byte further
    }
    const uint8_t mask = static_cast<uint8_t>(0xff << (8 - partial_bits));
    const uint8_t p = static_cast<uint8_t>(prefix[whole_bytes]) & mask;
    const uint8_t k = static_cast<uint8_t>(key[whole_bytes]) & mask;
    if (p != k) {
      return p < k ? -1 : +1;
    }
  }
  return 0;
}

// True if every key starting with the known bits is >= `upper_bound`, so a
// range ending before the exclusive `upper_bound` holds none of them.
inline bool KnownPrefixPastUpperBound(const Slice& prefix, uint32_t known_bits,
                                      const Slice& upper_bound) {
  const int cmp = CompareKnownPrefixBits(prefix, known_bits, upper_bound);
  if (cmp != 0) {
    return cmp > 0;
  }
  // `upper_bound` starts with the known bits. If it has no bits beyond them,
  // every key with the prefix is upper_bound itself or longer.
  return known_bits >= static_cast<uint32_t>(upper_bound.size()) * 8;
}

// True if every key starting with the known bits sorts after `key`. A key
// that itself starts with them is not "after": that is inconclusive.
inline bool KnownPrefixAfterKey(const Slice& prefix, uint32_t known_bits,
                                const Slice& key) {
  return CompareKnownPrefixBits(prefix, known_bits, key) > 0;
}

}  // namespace ROCKSDB_NAMESPACE
