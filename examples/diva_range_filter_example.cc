//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

// Uses the Diva range filter as the SST filter:
// - point lookups use it like a Bloom filter;
// - seeks with an upper bound skip SST files whose filter rules out any key
//   in [seek key, upper bound);
// - with deferred seeks (on by default), a Seek() leaves SST files whose next
//   key the filter places after the keys found so far until the scan gets
//   there, so a short scan does not read them at all.

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"

using ROCKSDB_NAMESPACE::BlockBasedTableOptions;
using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::FlushOptions;
using ROCKSDB_NAMESPACE::Iterator;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Slice;
using ROCKSDB_NAMESPACE::Statistics;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::Tickers;
using ROCKSDB_NAMESPACE::WriteOptions;

#if defined(OS_WIN)
std::string kDBPath = "C:\\Windows\\TEMP\\rocksdb_diva_range_filter_example";
#else
std::string kDBPath = "/tmp/rocksdb_diva_range_filter_example";
#endif

namespace {

// Scans up to `limit` keys from Seek(start) and prints how many keys it found,
// how many blocks it read and the given tickers.
void Scan(DB* db, Statistics* stats, const char* label,
          ReadOptions read_options, const char* start, int limit,
          std::initializer_list<std::pair<const char*, Tickers>> tickers) {
  // Read blocks from the files every time, so each scan's reads show.
  read_options.fill_cache = false;
  stats->Reset().PermitUncheckedError();
  ROCKSDB_NAMESPACE::get_perf_context()->Reset();

  std::unique_ptr<Iterator> iter(db->NewIterator(read_options));
  int count = 0;
  for (iter->Seek(start); iter->Valid() && count < limit; iter->Next()) {
    ++count;
  }
  assert(iter->status().ok());

  printf("%s\n  keys: %d, blocks read: %llu\n", label, count,
         static_cast<unsigned long long>(
             ROCKSDB_NAMESPACE::get_perf_context()->block_read_count));
  for (const auto& [name, ticker] : tickers) {
    printf("  %s: %llu\n", name,
           static_cast<unsigned long long>(stats->getTickerCount(ticker)));
  }
}

}  // namespace

int main() {
  Options options;
  options.create_if_missing = true;
  options.statistics = ROCKSDB_NAMESPACE::CreateDBStatistics();

  BlockBasedTableOptions table_options;
  // infix_bits_per_key is the filter's memory budget.
  table_options.filter_policy.reset(ROCKSDB_NAMESPACE::NewDivaFilterPolicy(
      /*rng_seed=*/1, /*infix_bits_per_key=*/10));
  // Small blocks, so that a short scan reads few of them.
  table_options.block_size = 1024;
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

  // Two SST files in L0: "user0000".."user0999" and "user5000".."user5999".
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
  printf("Get(user3000): %s\n\n", s.ToString().c_str());

  ROCKSDB_NAMESPACE::SetPerfLevel(ROCKSDB_NAMESPACE::PerfLevel::kEnableCount);
  Statistics* stats = options.statistics.get();

  // A short scan without an upper bound. The second file's filter places its
  // first key at "user5..." or later, after "user0100", so with deferred seeks
  // that file is never sought: 10 keys come from the first file alone.
  ReadOptions deferred;
  deferred.deferred_seeks = true;  // the default
  Scan(db.get(), stats, "10 keys from user0100, deferred seeks on:", deferred,
       "user0100", 10,
       {{"inputs postponed", ROCKSDB_NAMESPACE::DEFERRED_SEEK_POSTPONED},
        {"inputs sought later", ROCKSDB_NAMESPACE::DEFERRED_SEEK_ACTIVATED}});
  ReadOptions not_deferred;
  not_deferred.deferred_seeks = false;
  Scan(db.get(), stats,
       "10 keys from user0100, deferred seeks off:", not_deferred, "user0100",
       10, {});

  // A range query with an upper bound. No SST holds a key in
  // [user2000, user3000), so no file is read. Deferred seeks rule both out
  // before seeking them: the first has no key at or after user2000, and the
  // second's next key is past the upper bound.
  const std::string upper_bound = "user3000";
  Slice upper_bound_slice(upper_bound);
  ReadOptions bounded;
  bounded.iterate_upper_bound = &upper_bound_slice;
  Scan(db.get(), stats, "\nKeys in [user2000, user3000):", bounded, "user2000",
       /*limit=*/1000000,
       {{"inputs with no key at or after user2000",
         ROCKSDB_NAMESPACE::DEFERRED_SEEK_EMPTY},
        {"inputs past the upper bound",
         ROCKSDB_NAMESPACE::DEFERRED_SEEK_PAST_UPPER_BOUND}});

  // Without deferred seeks, each file's Seek() asks its filter whether
  // [user2000, user3000) can hold a key, and stops there.
  ReadOptions bounded_not_deferred = bounded;
  bounded_not_deferred.deferred_seeks = false;
  Scan(db.get(), stats, "Keys in [user2000, user3000), deferred seeks off:",
       bounded_not_deferred, "user2000", /*limit=*/1000000,
       {{"SST files ruled out by the range filter",
         ROCKSDB_NAMESPACE::RANGE_FILTER_USEFUL}});

  // The same query with the range filter ignored, as with a point-only filter
  // such as Bloom: the second file reads a block to find that its next key is
  // past the upper bound.
  ReadOptions no_range_filter = bounded;
  no_range_filter.lsm_range_filter = false;
  Scan(db.get(), stats,
       "Keys in [user2000, user3000), lsm_range_filter off:", no_range_filter,
       "user2000", /*limit=*/1000000,
       {{"SST files ruled out by the range filter",
         ROCKSDB_NAMESPACE::RANGE_FILTER_USEFUL}});

  s = db->Close();
  assert(s.ok());
  return 0;
}
