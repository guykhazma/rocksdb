//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

// Uses the Diva range filter as the SST filter. Point lookups use it like a
// Bloom filter; iterator seeks with an upper bound skip SST files whose filter
// rules out any key in [seek key, upper bound).

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"

using ROCKSDB_NAMESPACE::BlockBasedTableOptions;
using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::FlushOptions;
using ROCKSDB_NAMESPACE::Iterator;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Slice;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::WriteOptions;

#if defined(OS_WIN)
std::string kDBPath = "C:\\Windows\\TEMP\\rocksdb_diva_range_filter_example";
#else
std::string kDBPath = "/tmp/rocksdb_diva_range_filter_example";
#endif

int main() {
  Options options;
  options.create_if_missing = true;
  options.statistics = ROCKSDB_NAMESPACE::CreateDBStatistics();

  BlockBasedTableOptions table_options;
  // infix_bits_per_key is the filter's memory budget.
  table_options.filter_policy.reset(ROCKSDB_NAMESPACE::NewDivaFilterPolicy(
      /*rng_seed=*/1, /*infix_bits_per_key=*/10));
  options.table_factory.reset(
      ROCKSDB_NAMESPACE::NewBlockBasedTableFactory(table_options));

  ROCKSDB_NAMESPACE::DestroyDB(kDBPath, options);
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, kDBPath, &db);
  if (!s.ok()) {
    // For example NotSupported if this build does not include Diva.
    fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
    return 1;
  }

  // Two SST files: "user0000".."user0999" and "user5000".."user5999".
  char key[16];
  for (int file = 0; file < 2; ++file) {
    for (int i = 0; i < 1000; ++i) {
      snprintf(key, sizeof(key), "user%04d", file * 5000 + i);
      s = db->Put(WriteOptions(), key, "value");
      assert(s.ok());
    }
    s = db->Flush(FlushOptions());
    assert(s.ok());
  }

  std::string value;
  s = db->Get(ReadOptions(), "user0042", &value);
  printf("Get(user0042): %s\n", s.ToString().c_str());
  s = db->Get(ReadOptions(), "user3000", &value);
  printf("Get(user3000): %s\n", s.ToString().c_str());

  // A range query with an upper bound. No SST holds a key in
  // [user2000, user3000), so both files are skipped without reading them.
  const std::string upper_bound = "user3000";
  Slice upper_bound_slice(upper_bound);
  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound_slice;
  std::unique_ptr<Iterator> iter(db->NewIterator(read_options));
  int count = 0;
  for (iter->Seek("user2000"); iter->Valid(); iter->Next()) {
    ++count;
  }
  assert(iter->status().ok());
  printf("Keys in [user2000, user3000): %d\n", count);
  printf("SST files skipped by the range filter: %llu\n",
         static_cast<unsigned long long>(options.statistics->getTickerCount(
             ROCKSDB_NAMESPACE::RANGE_FILTER_USEFUL)));

  iter.reset();
  s = db->Close();
  assert(s.ok());
  return 0;
}
