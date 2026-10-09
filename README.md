## Diva as a range filter for SST files

This branch is RocksDB v11.1.2 plus an experimental filter policy backed by
[Diva](https://github.com/n3slami/Diva), a range filter, included as the
`third-party/Diva` git submodule and used in its default (`Standard`) mode.
It adds the following:

1. **Diva as the SST filter.** `NewDivaFilterPolicy()` replaces a Bloom or
   Ribbon filter: point lookups (`Get`, `MultiGet`) use it the same way.
2. **Range filtering for bounded seeks.** When an iterator has
   `ReadOptions::iterate_upper_bound` set, each SST file's table iterator
   first asks its Diva filter, on a forward `Seek(target)`, whether any key
   can exist in `[target, upper bound)`. If not, the seek ends without reading
   the index or data blocks.
3. **Deferred seeks** (`ReadOptions::deferred_seeks`, on by default). On a
   forward `Seek(target)`, the DB iterator asks each L0 file and each lower
   level for a lower bound of its first key `>= target`, taken from the Diva
   filter without reading data blocks. A file or level with no such key, or
   whose next key is past the upper bound, is not sought at all; one whose
   next key is provably after the smallest key found so far is sought only
   once `Next()` gets there. Results are unchanged; short scans read fewer
   blocks, with or without an upper bound.

`SeekForPrev`, `SeekToFirst`, backward iteration and compaction behave as
with any filter. `ReadOptions::lsm_range_filter = false` makes reads ignore
the range filter (2 and 3) while point lookups still use it, which is
handy for comparisons.

### Build

```bash
git submodule update --init third-party/Diva
make -j static_lib          # or CMake
```

### Use

```cpp
BlockBasedTableOptions table_options;
table_options.filter_policy.reset(NewDivaFilterPolicy(
    /*rng_seed=*/1, /*infix_bits_per_key=*/10));
options.table_factory.reset(NewBlockBasedTableFactory(table_options));

// Bounded scan: SST files with no key in [user2000, user3000) are skipped.
Slice upper_bound("user3000");
ReadOptions read_options;
read_options.iterate_upper_bound = &upper_bound;  // enables range filtering
std::unique_ptr<Iterator> it(db->NewIterator(read_options));
for (it->Seek("user2000"); it->Valid(); it->Next()) { /* ... */ }

// Short scan, bounded or not: deferred seeks (the default) leave SST files
// whose next key is after the keys found so far until the scan gets there.
ReadOptions short_scan;
short_scan.deferred_seeks = true;  // the default; false seeks every input
it.reset(db->NewIterator(short_scan));
it->Seek("user0100");
for (int i = 0; i < 10 && it->Valid(); ++i, it->Next()) { /* ... */ }

// Comparison baseline: the same SST files, the range filter ignored.
ReadOptions baseline = read_options;
baseline.lsm_range_filter = false;
```

`examples/diva_range_filter_example.cc` (`cd examples && make
diva_range_filter_example`) runs each case on two SST files and prints the
blocks read and the tickers below:

```text
10 keys from user0100, deferred seeks on:
  keys: 10, blocks read: 1
  inputs postponed: 1
  inputs sought later: 0
10 keys from user0100, deferred seeks off:
  keys: 10, blocks read: 2

Keys in [user2000, user3000):
  keys: 0, blocks read: 0
  inputs with no key at or after user2000: 1
  inputs past the upper bound: 1
Keys in [user2000, user3000), deferred seeks off:
  keys: 0, blocks read: 0
  SST files ruled out by the range filter: 2
Keys in [user2000, user3000), lsm_range_filter off:
  keys: 0, blocks read: 1
  SST files ruled out by the range filter: 0
```

Per table `Seek`, the statistics tickers (and the matching
`PerfContextByLevel` counters) count:

- `RANGE_FILTER_USEFUL` (`rocksdb.range.filter.useful`): the filter ruled
  the table out;
- `RANGE_FILTER_FULL_POSITIVE` (`rocksdb.range.filter.full.positive`): it
  could not;
- `RANGE_FILTER_FULL_TRUE_POSITIVE`
  (`rocksdb.range.filter.full.true.positive`): it could not, and the `Seek`
  found a key below the upper bound.

The `DEFERRED_SEEK_*` tickers (`rocksdb.deferred.seek.*`) count, per file or
level on a forward `Seek`, the bounds given and whether each input was ruled
out as empty or past the upper bound, postponed, or sought right away, and
later how many postponed inputs were activated.

Requirements, enforced when the DB is opened: bytewise comparator, no
user-defined timestamps, `whole_key_filtering = true`, no prefix extractor,
no partitioned filters, and no secondary cache under the block cache.
`DeleteRange()` is rejected on a column family with a Diva filter, whose
reads skip range tombstones.

- Filtering happens per SST file or level, at `Seek` time. Within a file,
  data blocks are read as usual.

Tests: `make db_diva_filter_test && ./db_diva_filter_test`.

Tools: `db_bench --use_diva_filter --bloom_bits=<infix bits per key>`, with
`--deferred_seeks` and `--lsm_range_filter`; bounded scans come from
`seekrandom --max_scan_distance=<n>`. `db_stress` takes the same three
flags, and `tools/db_crashtest.py` turns them on at random.
---

## RocksDB: A Persistent Key-Value Store for Flash and RAM Storage

[![CircleCI Status](https://circleci.com/gh/facebook/rocksdb.svg?style=svg)](https://circleci.com/gh/facebook/rocksdb)

RocksDB is developed and maintained by Facebook Database Engineering Team.
It is built on earlier work on [LevelDB](https://github.com/google/leveldb) by Sanjay Ghemawat (sanjay@google.com)
and Jeff Dean (jeff@google.com)

This code is a library that forms the core building block for a fast
key-value server, especially suited for storing data on flash drives.
It has a Log-Structured-Merge-Database (LSM) design with flexible tradeoffs
between Write-Amplification-Factor (WAF), Read-Amplification-Factor (RAF)
and Space-Amplification-Factor (SAF). It has multi-threaded compactions,
making it especially suitable for storing multiple terabytes of data in a
single database.

Start with example usage here: https://github.com/facebook/rocksdb/tree/main/examples

See the [github wiki](https://github.com/facebook/rocksdb/wiki) for more explanation.

The public interface is in `include/`.  Callers should not include or
rely on the details of any other header files in this package.  Those
internal APIs may be changed without warning.

Questions and discussions are welcome on the [RocksDB Developers Public](https://www.facebook.com/groups/rocksdb.dev/) Facebook group and [email list](https://groups.google.com/g/rocksdb) on Google Groups.

## License

RocksDB is dual-licensed under both the GPLv2 (found in the COPYING file in the root directory) and Apache 2.0 License (found in the LICENSE.Apache file in the root directory).  You may select, at your option, one of the above-listed licenses.
