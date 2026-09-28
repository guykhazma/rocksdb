//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

// Includes the Diva range filter (third-party/Diva) as a system header so that
// RocksDB's warning flags do not apply to third-party code. Diva is only
// available on x86-64 builds whose target supports the instructions it uses.

#pragma once

#if defined(__x86_64__) && defined(__SSE4_2__) && defined(__POPCNT__) && \
    defined(__BMI__) && defined(__BMI2__) && defined(__LZCNT__)
#define ROCKSDB_DIVA_SUPPORTED 1
#else
#define ROCKSDB_DIVA_SUPPORTED 0
#endif

#if ROCKSDB_DIVA_SUPPORTED

#if defined(__clang__)
#pragma clang system_header
#elif defined(__GNUC__)
#pragma GCC system_header
#endif

#include "rocksdb/rocksdb_namespace.h"
#include "third-party/Diva/include/diva.hpp"

namespace ROCKSDB_NAMESPACE {

using DivaRangeFilter =
    ::diva::Diva<::diva::DivaType::BinaryTrie, ::diva::PayloadType::None>;

}  // namespace ROCKSDB_NAMESPACE

#endif  // ROCKSDB_DIVA_SUPPORTED
