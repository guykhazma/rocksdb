## Diva as a range filter for SST files

This branch is RocksDB v11.1.2 plus an experimental filter policy backed by
[Diva](https://github.com/n3slami/Diva), a range filter, included as the
`third-party/Diva` git submodule. It adds the following:

1. **Diva as the SST filter.** `NewDivaFilterPolicy()` replaces a Bloom or
   Ribbon filter: point lookups (`Get`, `MultiGet`) use it the same way.
2. **Range filtering for bounded seeks.** When an iterator has
   `ReadOptions::iterate_upper_bound` set, each SST file's table iterator
   first asks its Diva filter, on a forward `Seek(target)`, whether any key
   can exist in `[target, upper bound)`. If not, the seek ends without reading
   the index or data blocks. Nothing else changes: iteration without an upper
   bound, `SeekForPrev`, `SeekToFirst` and compaction behave as with any
   filter.

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

Slice upper_bound("user3000");
ReadOptions read_options;
read_options.iterate_upper_bound = &upper_bound;  // enables range filtering
std::unique_ptr<Iterator> it(db->NewIterator(read_options));
for (it->Seek("user2000"); it->Valid(); it->Next()) { /* ... */ }
```

See `examples/diva_range_filter_example.cc` (`cd examples && make
diva_range_filter_example`). Per table `Seek`, the statistics tickers (and
the matching `PerfContextByLevel` counters) count:

- `RANGE_FILTER_USEFUL` (`rocksdb.range.filter.useful`): the filter ruled
  the table out;
- `RANGE_FILTER_FULL_POSITIVE` (`rocksdb.range.filter.full.positive`): it
  could not;
- `RANGE_FILTER_FULL_TRUE_POSITIVE`
  (`rocksdb.range.filter.full.true.positive`): it could not, and the `Seek`
  found a key below the upper bound.

Requirements, enforced when the DB is opened: bytewise comparator, no
user-defined timestamps, `whole_key_filtering = true`, no prefix extractor,
and no partitioned filters.

- Filtering happens per SST file, at `Seek` time. Within a file, data blocks
  are read as usual. Range deletions are not a supported use case.

Tests: `make db_diva_filter_test && ./db_diva_filter_test`.
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
