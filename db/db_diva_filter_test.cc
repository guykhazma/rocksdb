//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/column_family.h"
#include "db/db_test_util.h"
#include "db/table_cache.h"
#include "memory/memory_allocator_impl.h"
#include "port/stack_trace.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/perf_level.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/utilities/options_util.h"
#include "table/block_based/filter_policy_internal.h"
#include "table/block_based/parsed_full_filter_block.h"
#include "table/format.h"
#include "table/known_prefix_bits.h"
#include "table/table_reader.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

namespace {

constexpr uint32_t kSeed = 7;
constexpr uint32_t kInfixBitsPerKey = 10;

std::shared_ptr<const FilterPolicy> NewDiva(uint32_t infix_bits_per_key) {
  return std::shared_ptr<const FilterPolicy>(
      NewDivaFilterPolicy(kSeed, infix_bits_per_key));
}

// Distinct keys of 1 to 20 random bytes, sorted. Bytes 0x00 and 0xff are
// common, so keys share prefixes and some are prefixes of others.
std::vector<std::string> RandomKeys(Random* rnd, int count) {
  std::set<std::string> keys;
  while (static_cast<int>(keys.size()) < count) {
    std::string key(1 + rnd->Uniform(20), '\0');
    for (char& c : key) {
      const uint32_t r = rnd->Uniform(8);
      c = static_cast<char>(r == 0 ? 0x00 : r == 1 ? 0xff : rnd->Uniform(256));
    }
    keys.insert(key);
  }
  return std::vector<std::string>(keys.begin(), keys.end());
}

// Seek targets around `keys`: the keys themselves, keys cut short or
// extended, random keys, and keys below and above all of them.
std::vector<std::string> SeekTargets(Random* rnd,
                                     const std::vector<std::string>& keys) {
  std::vector<std::string> targets = {std::string(), std::string(1, '\0'),
                                      std::string(30, '\xff')};
  for (int i = 0; i < 2000; ++i) {
    const std::string& key = keys[rnd->Uniform(static_cast<int>(keys.size()))];
    switch (rnd->Uniform(4)) {
      case 0:
        targets.push_back(key);
        break;
      case 1:
        targets.push_back(
            key.substr(0, rnd->Uniform(static_cast<int>(key.size()))));
        break;
      case 2:
        targets.push_back(key +
                          std::string(1, static_cast<char>(rnd->Uniform(256))));
        break;
      default:
        targets.push_back(RandomKeys(rnd, 1)[0]);
        break;
    }
  }
  return targets;
}

bool IsAllZeros(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == '\0'; });
}

// Checks one lower bound against the sorted keys it bounds: "empty" only when
// no key is >= target; otherwise the first such key, and hence every later
// one, starts with or sorts after the known bits, and the prefix is padded.
void CheckLowerBound(const std::vector<std::string>& keys,
                     const std::string& target, const KeyLowerBound& bound) {
  SCOPED_TRACE("target=" + Slice(target).ToString(/*hex=*/true));
  const auto first = std::lower_bound(keys.begin(), keys.end(), target);
  if (first == keys.end()) {
    return;  // nothing to find; either answer is correct
  }
  ASSERT_FALSE(bound.empty);
  ASSERT_EQ(bound.prefix.size(), (bound.known_bits + 7) / 8);
  ASSERT_EQ(bound.prefix,
            TruncateToKnownPrefixBits(bound.prefix, bound.known_bits));
  ASSERT_LE(CompareKnownPrefixBits(bound.prefix, bound.known_bits, *first), 0)
      << "first key " << Slice(*first).ToString(/*hex=*/true) << " bound "
      << Slice(bound.prefix).ToString(/*hex=*/true) << "/" << bound.known_bits;
}

}  // namespace

TEST(KnownPrefixBitsTest, Comparisons) {
  // 0x61 0x60 with 12 known bits: keys starting with "a" then 0110 in the
  // high bits of the second byte.
  const std::string prefix = TruncateToKnownPrefixBits("a\x6f", 12);
  ASSERT_EQ(prefix, std::string("a\x60"));
  ASSERT_EQ(CompareKnownPrefixBits(prefix, 12, "a\x65"), 0);  // starts with
  ASSERT_EQ(CompareKnownPrefixBits(prefix, 12, "a\x6fzz"), 0);
  ASSERT_LT(CompareKnownPrefixBits(prefix, 12, "a\x70"), 0);  // before it
  ASSERT_LT(CompareKnownPrefixBits(prefix, 12, "b"), 0);
  ASSERT_GT(CompareKnownPrefixBits(prefix, 12, "a\x50"), 0);  // after it
  ASSERT_GT(CompareKnownPrefixBits(prefix, 12, "a"), 0);      // shorter, same
  ASSERT_GT(CompareKnownPrefixBits(prefix, 12, ""), 0);
  ASSERT_EQ(CompareKnownPrefixBits(prefix, 0, "anything"), 0);  // no bits

  ASSERT_TRUE(KnownPrefixAfterKey(prefix, 12, "a\x5f\xff"));
  ASSERT_FALSE(KnownPrefixAfterKey(prefix, 12, "a\x60"));  // inconclusive
  ASSERT_FALSE(KnownPrefixAfterKey(prefix, 0, ""));

  // Trailing zero bits are dropped; a prefix of zeros keeps nothing.
  ASSERT_EQ(KnownBitsWithoutTrailingZeros("a\x60", 16), 11);
  ASSERT_EQ(KnownBitsWithoutTrailingZeros("a\x60", 9), 8);
  ASSERT_EQ(KnownBitsWithoutTrailingZeros(std::string(3, '\0'), 20), 0);
  ASSERT_EQ(KnownBitsWithoutTrailingZeros("\x80", 1), 1);

  // Exclusive upper bounds.
  ASSERT_TRUE(KnownPrefixPastUpperBound(prefix, 12, "a\x5f"));
  ASSERT_FALSE(KnownPrefixPastUpperBound(prefix, 12, "a\x61"));
  // Exactly 16 known bits equal to the bound: every key is the bound or
  // longer, so none is below it.
  ASSERT_TRUE(KnownPrefixPastUpperBound("ab", 16, "ab"));
  // The bound continues past the known bits: keys may sort below it.
  ASSERT_FALSE(KnownPrefixPastUpperBound("ab", 16, "abc"));
}

// A merge keeps postponed children ordered by bound and stops at the first one
// whose bound is after the smallest key, taking all the others to be after it
// too. That holds for trimmed bounds: if bound a <= bound b bytewise and a is
// after a key, so is b.
TEST(KnownPrefixBitsTest, TrimmedBoundOrderAgreesWithAfterKey) {
  Random rnd(303);
  const std::vector<std::string> keys = RandomKeys(&rnd, 400);
  std::vector<std::pair<std::string, uint32_t>> bounds;
  for (int i = 0; i < 400; ++i) {
    const std::string& key = keys[rnd.Uniform(static_cast<int>(keys.size()))];
    // Up to a byte past the key's end, as Diva reports for short keys.
    uint32_t bits = rnd.Uniform(static_cast<int>(key.size()) * 8 + 9);
    bits = KnownBitsWithoutTrailingZeros(TruncateToKnownPrefixBits(key, bits),
                                         bits);
    bounds.emplace_back(TruncateToKnownPrefixBits(key, bits), bits);
  }
  std::sort(bounds.begin(), bounds.end());
  for (const std::string& key : SeekTargets(&rnd, keys)) {
    bool after = false;
    for (const auto& [prefix, bits] : bounds) {
      const bool this_after = KnownPrefixAfterKey(prefix, bits, key);
      ASSERT_TRUE(this_after || !after)
          << "key " << Slice(key).ToString(/*hex=*/true) << " bound "
          << Slice(prefix).ToString(/*hex=*/true) << "/" << bits;
      after = this_after;
    }
  }
}

class DivaFilterPolicyTest : public testing::Test {
 protected:
  void SetUp() override {
    if (!DivaFilterPolicy::IsSupported()) {
      ROCKSDB_GTEST_SKIP("Diva filter is not available in this build");
    }
  }

  // Builds a filter from `keys` (sorted) and returns its reader.
  std::unique_ptr<FilterBitsReader> Build(
      const FilterPolicy& policy, const std::vector<std::string>& keys) {
    BlockBasedTableOptions table_options;
    std::unique_ptr<FilterBitsBuilder> builder(
        policy.GetBuilderWithContext(FilterBuildingContext(table_options)));
    EXPECT_NE(builder, nullptr);
    for (const auto& key : keys) {
      builder->AddKey(key);
    }
    filter_ = builder->Finish(&buf_);
    return std::unique_ptr<FilterBitsReader>(
        policy.GetFilterBitsReader(filter_));
  }

  std::unique_ptr<const char[]> buf_;
  Slice filter_;
};

// The bound Funkey's Diva filter returns is sound for every target, and
// "empty" whenever no key is >= target (above the largest key).
TEST_F(DivaFilterPolicyTest, ApproximateLowerBound) {
  Random rnd(301);
  const auto policy = NewDiva(kInfixBitsPerKey);
  for (int round = 0; round < 4; ++round) {
    const std::vector<std::string> keys = RandomKeys(&rnd, 3000);
    auto reader = Build(*policy, keys);
    for (const std::string& target : SeekTargets(&rnd, keys)) {
      KeyLowerBound bound;
      if (!reader->GetApproximateLowerBound(target, &bound)) {
        ASSERT_TRUE(IsAllZeros(target));
        continue;
      }
      CheckLowerBound(keys, target, bound);
    }
    KeyLowerBound above;
    ASSERT_TRUE(reader->GetApproximateLowerBound(keys.back() + "\xff", &above));
    ASSERT_TRUE(above.empty);
  }
}

// Filters that cannot bound keys say so.
TEST_F(DivaFilterPolicyTest, ApproximateLowerBoundUnsupported) {
  const auto policy = NewDiva(kInfixBitsPerKey);
  auto reader = Build(*policy, {"a", "b"});
  KeyLowerBound bound;
  // Targets of only zero bytes, which an all-zero key could follow.
  ASSERT_FALSE(reader->GetApproximateLowerBound("", &bound));
  ASSERT_FALSE(reader->GetApproximateLowerBound(std::string(3, '\0'), &bound));
  // Longer than the filter's iterator can hold.
  ASSERT_FALSE(
      reader->GetApproximateLowerBound(std::string(2000, 'a'), &bound));
  std::unique_ptr<FilterBitsReader> garbage(
      policy->GetFilterBitsReader(std::string(64, 'x')));
  ASSERT_FALSE(garbage->GetApproximateLowerBound("a", &bound));
}

TEST_F(DivaFilterPolicyTest, PointAndRangeQueries) {
  const auto policy = NewDiva(kInfixBitsPerKey);
  // Adjacent repeats (several versions of one user key) collapse to one key.
  const std::vector<std::string> keys = {"apple",  "apple",  "banana",
                                         "cherry", "cherry", "grape"};
  auto reader = Build(*policy, keys);
  ASSERT_TRUE(reader->supportsRange());
  for (const auto& key : keys) {
    ASSERT_TRUE(reader->MayMatch(key));
    ASSERT_TRUE(reader->RangeMayMatch(key, key));
  }
  ASSERT_TRUE(reader->RangeMayMatch("b", "c"));
  ASSERT_TRUE(reader->RangeMayMatch("a", "zzz"));
  // An empty interval (end < start) is empty regardless of the filter.
  ASSERT_FALSE(reader->RangeMayMatch("grape", "apple"));
  // No end: the range is unbounded.
  ASSERT_TRUE(reader->RangeMayMatch("zzz", Slice()));
}

// A table reader parses a filter block it owns. The Diva reader copies what it
// needs, so the parsed block frees the filter block and counts the reader's
// memory in its place, and still answers queries; a Bloom reader reads the
// block in place, so the block stays.
TEST_F(DivaFilterPolicyTest, ParsedBlockReleasesFilterBlock) {
  Random rnd(17);
  std::set<std::string> distinct;
  while (distinct.size() < 3000) {
    distinct.insert(rnd.RandomString(1 + static_cast<int>(rnd.Uniform(20))));
  }
  const std::vector<std::string> keys(distinct.begin(), distinct.end());
  const auto parse = [&](const FilterPolicy& policy) {
    Build(policy, keys);
    CacheAllocationPtr owned = AllocateBlock(filter_.size(), nullptr);
    memcpy(owned.get(), filter_.data(), filter_.size());
    return std::make_unique<ParsedFullFilterBlock>(
        &policy, BlockContents(std::move(owned), filter_.size()));
  };

  const auto diva = NewDiva(kInfixBitsPerKey);
  auto parsed = parse(*diva);
  ASSERT_TRUE(parsed->own_bytes());
  // The block's bytes are gone (an empty BlockContents still counts its own
  // size); in their place is the reader's estimate, the size of the block it
  // replaced.
  ASSERT_EQ(parsed->ApproximateMemoryUsage(),
            BlockContents().ApproximateMemoryUsage() + filter_.size());
  for (const auto& key : keys) {
    ASSERT_TRUE(parsed->filter_bits_reader()->MayMatch(key));
  }
  ASSERT_TRUE(parsed->filter_bits_reader()->RangeMayMatch(keys.front(),
                                                         keys.back()));

  const std::shared_ptr<const FilterPolicy> bloom(NewBloomFilterPolicy(10));
  auto bloom_parsed = parse(*bloom);
  ASSERT_TRUE(bloom_parsed->own_bytes());
  ASSERT_GE(bloom_parsed->ApproximateMemoryUsage(), filter_.size());
  ASSERT_EQ(bloom_parsed->ContentSlice().size(), filter_.size());
  for (const auto& key : keys) {
    ASSERT_TRUE(bloom_parsed->filter_bits_reader()->MayMatch(key));
  }
}

TEST_F(DivaFilterPolicyTest, UnusableContentsMatchEverything) {
  const auto policy = NewDiva(kInfixBitsPerKey);
  const std::string garbage(64, 'x');
  std::unique_ptr<FilterBitsReader> reader(
      policy->GetFilterBitsReader(garbage));
  ASSERT_FALSE(reader->supportsRange());
  ASSERT_TRUE(reader->MayMatch("anything"));
  ASSERT_TRUE(reader->RangeMayMatch("a", "b"));
}

TEST_F(DivaFilterPolicyTest, ZeroBitsPerKeyBuildsNoFilter) {
  const auto policy = NewDiva(0);
  BlockBasedTableOptions table_options;
  std::unique_ptr<FilterBitsBuilder> builder(
      policy->GetBuilderWithContext(FilterBuildingContext(table_options)));
  ASSERT_EQ(builder, nullptr);
}

// The id an OPTIONS file records must rebuild an equivalent policy.
TEST_F(DivaFilterPolicyTest, IdRoundTrip) {
  std::shared_ptr<const FilterPolicy> policy(
      NewDivaFilterPolicy(kSeed, kInfixBitsPerKey, 0.9 /* load_factor */));
  ConfigOptions config_options;
  config_options.ignore_unsupported_options = false;
  std::shared_ptr<const FilterPolicy> rebuilt;
  ASSERT_OK(FilterPolicy::CreateFromString(config_options, policy->GetId(),
                                           &rebuilt));
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_EQ(rebuilt->GetId(), policy->GetId());
  EXPECT_STREQ(rebuilt->Name(), policy->Name());
}

// The short form names only the infix bits; the rest take their defaults.
TEST_F(DivaFilterPolicyTest, ShortConfigString) {
  ConfigOptions config_options;
  config_options.ignore_unsupported_options = false;
  std::shared_ptr<const FilterPolicy> policy;
  ASSERT_OK(
      FilterPolicy::CreateFromString(config_options, "divafilter:9", &policy));
  ASSERT_NE(policy, nullptr);
  // The same policy NewDivaFilterPolicy() builds when only the infix bits are
  // given, so the short form and the API cannot drift apart.
  std::shared_ptr<const FilterPolicy> defaulted(
      NewDivaFilterPolicy(DivaFilterPolicy::kDefaultRngSeed, 9));
  EXPECT_EQ(policy->GetId(), defaulted->GetId());
}

class DBDivaFilterTest : public DBTestBase {
 public:
  DBDivaFilterTest()
      : DBTestBase("db_diva_filter_test", /*env_do_fsync=*/false) {}

 protected:
  void SetUp() override {
    if (!DivaFilterPolicy::IsSupported()) {
      ROCKSDB_GTEST_SKIP("Diva filter is not available in this build");
    }
  }

  Options DivaOptions() {
    Options options = CurrentOptions();
    options.create_if_missing = true;
    options.disable_auto_compactions = true;
    options.num_levels = 4;
    options.statistics = CreateDBStatistics();
    BlockBasedTableOptions table_options;
    table_options.filter_policy = NewDiva(kInfixBitsPerKey);
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    return options;
  }

  // Keys in [lo, upper_bound) returned by a forward scan from Seek(lo).
  std::vector<std::string> Scan(const std::string& lo,
                                const std::string& upper_bound,
                                ReadOptions read_options = ReadOptions()) {
    Slice upper_bound_slice(upper_bound);
    read_options.iterate_upper_bound = &upper_bound_slice;
    std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
    std::vector<std::string> keys;
    for (iter->Seek(lo); iter->Valid(); iter->Next()) {
      keys.push_back(iter->key().ToString());
    }
    EXPECT_OK(iter->status());
    return keys;
  }

  static std::vector<std::string> ModelScan(
      const std::map<std::string, std::string>& model, const std::string& lo,
      const std::string& upper_bound) {
    std::vector<std::string> keys;
    for (auto it = model.lower_bound(lo);
         it != model.end() && it->first < upper_bound; ++it) {
      keys.push_back(it->first);
    }
    return keys;
  }

  uint64_t Pop(Tickers ticker) {
    return options_statistics_->getAndResetTickerCount(ticker);
  }

  using Entries = std::vector<std::pair<std::string, std::string>>;

  // One setting of the deferred seek switches.
  static ReadOptions Reads(bool deferred_seeks, bool lsm_range_filter) {
    ReadOptions read_options;
    read_options.deferred_seeks = deferred_seeks;
    read_options.lsm_range_filter = lsm_range_filter;
    return read_options;
  }

  // Every setting of the deferred seek switches.
  static std::vector<ReadOptions> AllReads() {
    return {Reads(true, true), Reads(true, false), Reads(false, true),
            Reads(false, false)};
  }

  static std::string Describe(const ReadOptions& read_options) {
    return std::string("deferred_seeks=") +
           (read_options.deferred_seeks ? "1" : "0") +
           " lsm_range_filter=" + (read_options.lsm_range_filter ? "1" : "0");
  }

  // Up to `limit` entries of a forward scan from Seek(lo), below
  // `upper_bound` unless it is null.
  Entries ScanEntries(ReadOptions read_options, const std::string& lo,
                      const std::string* upper_bound, size_t limit) {
    Slice upper_bound_slice;
    if (upper_bound != nullptr) {
      upper_bound_slice = *upper_bound;
      read_options.iterate_upper_bound = &upper_bound_slice;
    }
    std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
    Entries entries;
    for (iter->Seek(lo); iter->Valid() && entries.size() < limit;
         iter->Next()) {
      entries.emplace_back(iter->key().ToString(), iter->value().ToString());
    }
    EXPECT_OK(iter->status());
    return entries;
  }

  static Entries ModelEntries(const std::map<std::string, std::string>& model,
                              const std::string& lo,
                              const std::string* upper_bound, size_t limit) {
    Entries entries;
    for (auto it = model.lower_bound(lo);
         it != model.end() && entries.size() < limit &&
         (upper_bound == nullptr || it->first < *upper_bound);
         ++it) {
      entries.emplace_back(it->first, it->second);
    }
    return entries;
  }

  // Scans from every target in `targets`, below every bound in
  // `upper_bounds` and unbounded, whole and cut short, in every setting, and
  // compares them with `model`.
  void CheckScans(const std::map<std::string, std::string>& model,
                  const std::vector<std::string>& targets,
                  const std::vector<std::string>& upper_bounds) {
    for (const ReadOptions& read_options : AllReads()) {
      SCOPED_TRACE(Describe(read_options));
      for (const std::string& lo : targets) {
        for (size_t b = 0; b <= upper_bounds.size(); ++b) {
          const std::string* upper_bound =
              b < upper_bounds.size() ? &upper_bounds[b] : nullptr;
          for (size_t limit : {size_t{1}, size_t{3}, SIZE_MAX}) {
            ASSERT_EQ(ScanEntries(read_options, lo, upper_bound, limit),
                      ModelEntries(model, lo, upper_bound, limit))
                << "lo=" << Slice(lo).ToString(/*hex=*/true) << " upper_bound="
                << (upper_bound ? Slice(*upper_bound).ToString(/*hex=*/true)
                                : "none")
                << " limit=" << limit;
          }
        }
      }
    }
  }

  struct DeferredSeekCounts {
    uint64_t bounds = 0;
    uint64_t empty = 0;
    uint64_t past_upper_bound = 0;
    uint64_t postponed = 0;
    uint64_t immediate = 0;
    uint64_t activated = 0;
    uint64_t drained = 0;
    uint64_t fallback = 0;
  };

  // The deferred seek tickers since the last call. Each child that gave a
  // bound is counted once as empty, past the upper bound, postponed or
  // sought right away. (A postponed child is sought at most once, but maybe
  // after the next call.)
  DeferredSeekCounts PopDeferredSeeks() {
    DeferredSeekCounts c;
    c.bounds = Pop(DEFERRED_SEEK_BOUNDS);
    c.empty = Pop(DEFERRED_SEEK_EMPTY);
    c.past_upper_bound = Pop(DEFERRED_SEEK_PAST_UPPER_BOUND);
    c.postponed = Pop(DEFERRED_SEEK_POSTPONED);
    c.immediate = Pop(DEFERRED_SEEK_IMMEDIATE);
    c.activated = Pop(DEFERRED_SEEK_ACTIVATED);
    c.drained = Pop(DEFERRED_SEEK_DRAINED);
    c.fallback = Pop(DEFERRED_SEEK_FALLBACK);
    EXPECT_EQ(c.bounds,
              c.empty + c.past_upper_bound + c.postponed + c.immediate);
    return c;
  }

  // Three files of 100 keys with disjoint key ranges: "a", "c" and "e"
  // followed by Key(i). In L0, or one per level with "a" in L3 and "e" in L1.
  void WriteDisjointFiles(bool l0, std::map<std::string, std::string>* model) {
    int level = 3;
    for (const char* prefix : {"a", "c", "e"}) {
      for (int i = 0; i < 100; ++i) {
        const std::string key = prefix + Key(i);
        const std::string value = std::string(prefix) + std::to_string(i);
        ASSERT_OK(Put(key, value));
        (*model)[key] = value;
      }
      ASSERT_OK(Flush());
      if (!l0) {
        MoveFilesToLevel(level--);
      }
    }
  }

  void OpenWithDiva() {
    Options options = DivaOptions();
    options_statistics_ = options.statistics;
    Reopen(options);
  }

  void ReopenEmptyWithDiva() {
    Options options = DivaOptions();
    options_statistics_ = options.statistics;
    DestroyAndReopen(options);
  }

  std::shared_ptr<Statistics> options_statistics_;
};

// Every SST file's table reader bounds the keys of that file, through the
// filter block it holds or reads.
TEST_F(DBDivaFilterTest, TableApproximateLowerBound) {
  OpenWithDiva();
  Random rnd(302);
  // Three overlapping files in L0, then the same pushed down a level.
  for (int file = 0; file < 3; ++file) {
    for (const std::string& key : RandomKeys(&rnd, 500)) {
      ASSERT_OK(Put(key, "v"));
    }
    ASSERT_OK(Flush());
  }
  // Iterators keep a reference to their read options.
  const ReadOptions read_options;
  const auto check_files = [&](int level) {
    ColumnFamilyData* cfd =
        static_cast<ColumnFamilyHandleImpl*>(db_->DefaultColumnFamily())->cfd();
    const auto& files = cfd->current()->storage_info()->LevelFiles(level);
    ASSERT_FALSE(files.empty());
    for (FileMetaData* meta : files) {
      TableCache::TypedHandle* handle = nullptr;
      TableReader* table = nullptr;
      ASSERT_OK(cfd->table_cache()->FindTable(
          read_options, FileOptions(), cfd->internal_comparator(), *meta,
          &handle, cfd->GetLatestMutableCFOptions(), &table));
      // The file's user keys, read back.
      std::vector<std::string> keys;
      std::unique_ptr<InternalIterator> iter(table->NewIterator(
          read_options, /*prefix_extractor=*/nullptr, /*arena=*/nullptr,
          /*skip_filters=*/false, TableReaderCaller::kUncategorized));
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        keys.push_back(ExtractUserKey(iter->key()).ToString());
      }
      ASSERT_OK(iter->status());
      for (const std::string& target : SeekTargets(&rnd, keys)) {
        KeyLowerBound bound;
        if (!table->GetApproximateLowerBound(read_options, target, &bound)) {
          ASSERT_TRUE(IsAllZeros(target));
          continue;
        }
        CheckLowerBound(keys, target, bound);
      }
      if (handle != nullptr) {
        cfd->table_cache()->get_cache().Release(handle);
      }
    }
  };
  check_files(0);
  MoveFilesToLevel(1);
  check_files(1);
}

TEST_F(DBDivaFilterTest, PointLookups) {
  OpenWithDiva();
  for (int i = 0; i < 2000; i += 2) {
    ASSERT_OK(Put(Key(i), "v" + std::to_string(i)));
  }
  ASSERT_OK(Flush());
  for (int i = 0; i < 2000; i += 2) {
    ASSERT_EQ(Get(Key(i)), "v" + std::to_string(i));
  }
  Pop(BLOOM_FILTER_USEFUL);
  for (int i = 1; i < 2000; i += 2) {
    ASSERT_EQ(Get(Key(i)), "NOT_FOUND");
  }
  ASSERT_GT(Pop(BLOOM_FILTER_USEFUL), 0);
}

// A bounded seek into a key range without keys skips SST files, both for L0
// files and for files of L1 and below, including when the range lies inside a
// file's key range. Counters are per table Seek. With deferred seeks the merge
// would not seek those files at all, so this checks the table's own range
// check with them off.
TEST_F(DBDivaFilterTest, BoundedSeekSkipsFiles) {
  OpenWithDiva();
  ReadOptions no_deferral;
  no_deferral.deferred_seeks = false;
  const auto Scan = [&](const std::string& lo, const std::string& upper_bound) {
    return this->Scan(lo, upper_bound, no_deferral);
  };
  // File 1 has a gap: "a" + Key(100) .. "a" + Key(199) are missing.
  for (int i = 0; i < 300; ++i) {
    if (i < 100 || i >= 200) {
      ASSERT_OK(Put("a" + Key(i), "va"));
    }
  }
  ASSERT_OK(Flush());
  for (int i = 0; i < 100; ++i) {
    ASSERT_OK(Put("c" + Key(i), "vc"));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);

  const auto check = [&](bool l0) {
    Pop(RANGE_FILTER_USEFUL);
    Pop(RANGE_FILTER_FULL_POSITIVE);
    Pop(RANGE_FILTER_FULL_TRUE_POSITIVE);
    // Between the files: every L0 file is sought; in a level only the file
    // after the seek key is.
    ASSERT_TRUE(Scan("b", "b~").empty());
    ASSERT_EQ(Pop(RANGE_FILTER_USEFUL), l0 ? 2 : 1);
    ASSERT_EQ(Pop(RANGE_FILTER_FULL_POSITIVE), 0);
    // Inside file 1's key range.
    ASSERT_TRUE(Scan("a" + Key(120), "a" + Key(180)).empty());
    ASSERT_GE(Pop(RANGE_FILTER_USEFUL), 1);
    ASSERT_EQ(Pop(RANGE_FILTER_FULL_TRUE_POSITIVE), 0);
    // Ranges with keys: the file holding them is a true positive.
    ASSERT_EQ(Scan("a", "b").size(), 200);
    ASSERT_EQ(Pop(RANGE_FILTER_FULL_TRUE_POSITIVE), 1);
    ASSERT_EQ(Scan("c", "d").size(), 100);
    ASSERT_EQ(Pop(RANGE_FILTER_FULL_TRUE_POSITIVE), 1);
    ASSERT_GE(Pop(RANGE_FILTER_FULL_POSITIVE), 2);
  };
  check(/*l0=*/true);
  MoveFilesToLevel(1);
  ASSERT_EQ(NumTableFilesAtLevel(1), 2);
  check(/*l0=*/false);
}

// A filter written into an SST file still filters after the DB is closed and
// reopened, with nothing left in memory from building it. This is asserted on
// the counters rather than on the keys returned, because
// GetFilterBitsReader() answers a block it does not recognize with a filter
// that matches everything: a serialization bug would leave reads correct but
// unfiltered, which only the counters can see.
TEST_F(DBDivaFilterTest, FilterSurvivesReopen) {
  OpenWithDiva();
  // One file with a gap: "a" + Key(100) .. "a" + Key(199) are missing.
  for (int i = 0; i < 300; ++i) {
    if (i < 100 || i >= 200) {
      ASSERT_OK(Put("a" + Key(i), "va"));
    }
  }
  ASSERT_OK(Flush());

  OpenWithDiva();  // closes and reopens; the table cache starts cold

  // The table's own range check, which deferred seeks would bypass.
  ReadOptions no_deferral;
  no_deferral.deferred_seeks = false;
  Pop(RANGE_FILTER_USEFUL);
  ASSERT_TRUE(Scan("a" + Key(120), "a" + Key(180), no_deferral).empty());
  EXPECT_GE(Pop(RANGE_FILTER_USEFUL), 1);
  ASSERT_EQ(Scan("a", "b").size(), 200);
  ASSERT_EQ(Get("a" + Key(50)), "va");
}

// Keys made only of 0x00 bytes, and lookups and seeks from them. Each file
// holds one such key.
TEST_F(DBDivaFilterTest, AllZeroKeys) {
  OpenWithDiva();
  const std::string zero2(2, '\0');
  ASSERT_OK(Put(zero2, "z2"));
  for (int i = 0; i < 100; ++i) {
    ASSERT_OK(Put("a" + Key(i), "va"));
  }
  ASSERT_OK(Flush());
  ASSERT_OK(Put("", "empty"));
  ASSERT_OK(Put("b", "vb"));
  ASSERT_OK(Flush());
  ASSERT_EQ(NumTableFilesAtLevel(0), 2);

  ASSERT_EQ(Get(zero2), "z2");
  ASSERT_EQ(Get(""), "empty");
  ASSERT_EQ(Get(std::string(1, '\0')), "NOT_FOUND");
  ASSERT_EQ(Scan("", zero2 + '\x01'), (std::vector<std::string>{"", zero2}));
  ASSERT_EQ(Scan(std::string(1, '\0'), "a"), (std::vector<std::string>{zero2}));
  ASSERT_EQ(Scan(std::string(8, '\0'), "a" + Key(1)).size(), 1);
}

// The seek path shares one filter_checked flag between the prefix filter and
// the range filter. A prefix-filter check on a table with no range filter
// must not be counted as a range-filter true positive.
TEST_F(DBDivaFilterTest, PrefixFilterSeekIsNotARangeFilterPositive) {
  Options options = CurrentOptions();
  options.create_if_missing = true;
  options.disable_auto_compactions = true;
  options.statistics = CreateDBStatistics();
  options.prefix_extractor.reset(NewFixedPrefixTransform(1));
  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(NewBloomFilterPolicy(10));
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  options_statistics_ = options.statistics;
  Reopen(options);
  for (int i = 0; i < 100; ++i) {
    ASSERT_OK(Put("a" + Key(i), "v"));
  }
  ASSERT_OK(Flush());
  Pop(RANGE_FILTER_FULL_TRUE_POSITIVE);

  const std::string upper = "a" + Key(50);
  const Slice upper_bound(upper);
  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound;
  std::unique_ptr<Iterator> iter(db_->NewIterator(read_options));
  iter->Seek("a" + Key(10));
  ASSERT_TRUE(iter->Valid());
  ASSERT_OK(iter->status());
  ASSERT_EQ(Pop(RANGE_FILTER_FULL_TRUE_POSITIVE), 0);
}

// Without iterate_upper_bound the range filter is not consulted.
TEST_F(DBDivaFilterTest, UnboundedSeekDoesNotUseFilter) {
  OpenWithDiva();
  for (int i = 0; i < 100; ++i) {
    ASSERT_OK(Put("a" + Key(i), "v"));
  }
  ASSERT_OK(Flush());
  Pop(RANGE_FILTER_USEFUL);
  Pop(RANGE_FILTER_FULL_POSITIVE);
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  iter->Seek("b");
  ASSERT_FALSE(iter->Valid());
  ASSERT_OK(iter->status());
  ASSERT_EQ(Pop(RANGE_FILTER_USEFUL), 0);
  ASSERT_EQ(Pop(RANGE_FILTER_FULL_POSITIVE), 0);
}

// Reads of tables with a range filter skip range tombstones, so DeleteRange()
// is rejected, alone or in a batch.
TEST_F(DBDivaFilterTest, DeleteRangeIsRejected) {
  OpenWithDiva();
  ASSERT_OK(Put(Key(1), "v"));
  Status s = db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(),
                              Key(0), Key(100));
  ASSERT_TRUE(s.IsNotSupported()) << s.ToString();
  WriteBatch batch;
  ASSERT_OK(batch.Put(Key(2), "v"));
  ASSERT_OK(batch.DeleteRange(Key(0), Key(100)));
  s = db_->Write(WriteOptions(), &batch);
  ASSERT_TRUE(s.IsNotSupported()) << s.ToString();
  ASSERT_EQ(Get(Key(1)), "v");
  ASSERT_EQ(Get(Key(2)), "NOT_FOUND");
}

// Tools that reopen a DB from its OPTIONS file (LoadLatestOptions, ldb,
// sst_dump) must rebuild the filter policy rather than lose it.
TEST_F(DBDivaFilterTest, OptionsFileRoundTrip) {
  OpenWithDiva();
  ASSERT_OK(Put(Key(1), "v"));
  ASSERT_OK(Flush());
  const std::string expected =
      DivaOptions()
          .table_factory->GetOptions<BlockBasedTableOptions>()
          ->filter_policy->GetId();

  ConfigOptions config_options;
  config_options.env = env_;
  config_options.ignore_unsupported_options = false;
  DBOptions db_options;
  std::vector<ColumnFamilyDescriptor> cfs;
  ASSERT_OK(LoadLatestOptions(config_options, dbname_, &db_options, &cfs));
  ASSERT_EQ(cfs.size(), 1);
  const auto* table_options =
      cfs[0].options.table_factory->GetOptions<BlockBasedTableOptions>();
  ASSERT_NE(table_options, nullptr);
  ASSERT_NE(table_options->filter_policy, nullptr);
  EXPECT_EQ(table_options->filter_policy->GetId(), expected);
}

TEST_F(DBDivaFilterTest, UnsupportedOptionsAreRejected) {
  const auto check_rejected = [&](Options options) {
    options.create_if_missing = true;
    Status s = TryReopen(options);
    ASSERT_TRUE(s.IsNotSupported()) << s.ToString();
  };
  // Diva with the given table options.
  const auto with_table_options = [&](BlockBasedTableOptions table_options) {
    table_options.filter_policy = NewDiva(kInfixBitsPerKey);
    Options options = DivaOptions();
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    return options;
  };
  {
    Options options = DivaOptions();
    options.prefix_extractor.reset(NewFixedPrefixTransform(3));
    check_rejected(options);
  }
  {
    Options options = DivaOptions();
    options.comparator = ReverseBytewiseComparator();
    check_rejected(options);
  }
  {
    BlockBasedTableOptions table_options;
    table_options.whole_key_filtering = false;
    check_rejected(with_table_options(table_options));
  }
  {
    BlockBasedTableOptions table_options;
    table_options.partition_filters = true;
    table_options.index_type = BlockBasedTableOptions::kTwoLevelIndexSearch;
    check_rejected(with_table_options(table_options));
  }
  {
    LRUCacheOptions cache_options;
    cache_options.capacity = 1 << 20;
    cache_options.secondary_cache =
        NewCompressedSecondaryCache(/*capacity=*/1 << 20);
    BlockBasedTableOptions table_options;
    table_options.block_cache = cache_options.MakeSharedCache();
    check_rejected(with_table_options(table_options));
  }
  {
    // A block cache without a secondary cache is fine.
    BlockBasedTableOptions table_options;
    table_options.block_cache = NewLRUCache(1 << 20);
    Options options = with_table_options(table_options);
    options.create_if_missing = true;
    ASSERT_OK(TryReopen(options));
  }
}

// Random overwrites and deletes spread over several levels; every point lookup
// and bounded scan must match a model of the expected contents.
TEST_F(DBDivaFilterTest, RandomizedMatchesModel) {
  const uint32_t seed = static_cast<uint32_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random rnd(seed);
  OpenWithDiva();

  // Variable-length keys with shared prefixes. No 0x00 bytes.
  const auto random_key = [&]() {
    std::string key = "k" + std::to_string(rnd.Uniform(4));
    const int len = 1 + static_cast<int>(rnd.Uniform(12));
    for (int i = 0; i < len; ++i) {
      key.push_back(static_cast<char>('a' + rnd.Uniform(26)));
    }
    return key;
  };

  std::map<std::string, std::string> model;
  std::vector<std::string> written;
  for (int round = 0; round < 7; ++round) {
    for (int i = 0; i < 400; ++i) {
      if (!written.empty() && rnd.OneIn(5)) {
        const std::string& key =
            written[rnd.Uniform(static_cast<int>(written.size()))];
        ASSERT_OK(Delete(key));
        model.erase(key);
      } else {
        const std::string key = random_key();
        const std::string value = "v" + std::to_string(rnd.Next());
        ASSERT_OK(Put(key, value));
        model[key] = value;
        written.push_back(key);
      }
    }
    ASSERT_OK(Flush());
    if (round % 2 == 1) {
      MoveFilesToLevel(3 - round / 2);
    }
  }

  for (const auto& key : written) {
    const auto it = model.find(key);
    ASSERT_EQ(Get(key), it == model.end() ? "NOT_FOUND" : it->second);
  }
  for (int i = 0; i < 2000; ++i) {
    std::string lo = random_key();
    std::string upper_bound = random_key();
    if (upper_bound < lo) {
      std::swap(lo, upper_bound);
    }
    ASSERT_EQ(Scan(lo, upper_bound), ModelScan(model, lo, upper_bound))
        << "lo=" << lo << " upper_bound=" << upper_bound;
  }
}

// A Seek() postpones the files whose keys all come after the first key found,
// and seeks them only once the scan reaches them. Their keys are returned in
// order, and none is returned twice.
TEST_F(DBDivaFilterTest, DeferredSeeksPostponeLaterFiles) {
  for (bool l0 : {true, false}) {
    SCOPED_TRACE(l0 ? "L0 files" : "one file per level");
    ReopenEmptyWithDiva();
    std::map<std::string, std::string> model;
    WriteDisjointFiles(l0, &model);
    PopDeferredSeeks();

    SetPerfLevel(PerfLevel::kEnableCount);
    get_perf_context()->Reset();
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->Seek("a");
    // The memtable and the "a" file.
    EXPECT_EQ(get_perf_context()->seek_child_seek_count, 2);
    DeferredSeekCounts c = PopDeferredSeeks();
    EXPECT_EQ(c.bounds, 3);
    EXPECT_EQ(c.immediate, 1);
    EXPECT_EQ(c.postponed, 2);

    auto expected = model.begin();
    for (int i = 0; i < 10; ++i, ++expected, iter->Next()) {
      ASSERT_TRUE(iter->Valid());
      ASSERT_EQ(iter->key().ToString(), expected->first);
    }
    c = PopDeferredSeeks();
    EXPECT_EQ(c.activated + c.drained, 0);

    for (; iter->Valid(); iter->Next(), ++expected) {
      ASSERT_NE(expected, model.end());
      ASSERT_EQ(iter->key().ToString(), expected->first);
      ASSERT_EQ(iter->value().ToString(), expected->second);
    }
    ASSERT_OK(iter->status());
    ASSERT_EQ(expected, model.end());
    // Nothing is left when the "a" and then the "c" file run out: both
    // postponed files are sought then, once each.
    c = PopDeferredSeeks();
    EXPECT_EQ(c.drained, 2);
    EXPECT_EQ(c.activated, 0);
    EXPECT_EQ(c.postponed, 0);

    // Past the "a" and "c" files: they are empty from "d" on.
    iter->Seek("d");
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().ToString(), "e" + Key(0));
    c = PopDeferredSeeks();
    EXPECT_EQ(c.empty, 2);
    EXPECT_EQ(c.immediate, 1);

    // The "c" and "e" files start past the upper bound.
    const std::string upper_bound = "b";
    EXPECT_EQ(ScanEntries(ReadOptions(), "a" + Key(50), &upper_bound, SIZE_MAX),
              ModelEntries(model, "a" + Key(50), &upper_bound, SIZE_MAX));
    c = PopDeferredSeeks();
    EXPECT_EQ(c.past_upper_bound, 2);
    EXPECT_EQ(c.immediate, 1);

    // Without deferral every child is sought.
    get_perf_context()->Reset();
    std::unique_ptr<Iterator> plain(
        db_->NewIterator(Reads(/*deferred_seeks=*/false, true)));
    plain->Seek("a");
    EXPECT_EQ(get_perf_context()->seek_child_seek_count, 4);
    c = PopDeferredSeeks();
    EXPECT_EQ(c.bounds, 0);
    EXPECT_EQ(c.fallback, 0);
    SetPerfLevel(PerfLevel::kDisable);
  }
}

// Files whose keys interleave: a postponed file is sought as soon as its
// bound is no longer after the smallest key.
TEST_F(DBDivaFilterTest, DeferredSeeksInterleavedFiles) {
  for (bool l0 : {true, false}) {
    SCOPED_TRACE(l0 ? "L0 files" : "one file per level");
    ReopenEmptyWithDiva();
    std::map<std::string, std::string> model;
    for (int file = 0; file < 3; ++file) {
      for (int i = file; i < 300; i += 3) {
        ASSERT_OK(Put("b" + Key(i), std::to_string(i)));
        model["b" + Key(i)] = std::to_string(i);
      }
      ASSERT_OK(Flush());
      if (!l0) {
        MoveFilesToLevel(3 - file);
      }
    }
    PopDeferredSeeks();
    CheckScans(model, {"", "b", "b" + Key(1), "b" + Key(150), "b" + Key(298)},
               {"b" + Key(5), "b" + Key(200), "c"});
    const DeferredSeekCounts c = PopDeferredSeeks();
    EXPECT_GT(c.bounds, 0);
    EXPECT_EQ(c.fallback, 0);
  }
}

// Versions of a key spread over several children: an older value in a lower
// level, a newer value or a delete above it. Postponing a child must never
// let an older version through.
TEST_F(DBDivaFilterTest, DeferredSeeksVersionsAcrossChildren) {
  OpenWithDiva();
  std::map<std::string, std::string> model;
  const auto put = [&](const std::string& key, const std::string& value) {
    ASSERT_OK(Put(key, value));
    model[key] = value;
  };
  const auto del = [&](const std::string& key) {
    ASSERT_OK(Delete(key));
    model.erase(key);
  };
  for (int i = 1; i <= 9; ++i) {
    put("b" + std::to_string(i), "old");
  }
  put("d", "old");
  ASSERT_OK(Flush());
  MoveFilesToLevel(3);
  put("a1", "l2");
  put("b5", "l2");
  ASSERT_OK(Flush());
  MoveFilesToLevel(2);
  del("b3");
  put("c1", "l1");
  ASSERT_OK(Flush());
  MoveFilesToLevel(1);
  put("b7", "l0");
  del("b9");
  ASSERT_OK(Flush());
  put("b2", "mem");
  del("d");

  CheckScans(model,
             {"", "a", "b", "b1", "b3", "b4", "b5", "b6", "b8", "b9", "c", "d"},
             {"b5", "b8", "c", "d", "e"});
  for (const char* key : {"b2", "b3", "b5", "b7", "b9", "d"}) {
    const auto it = model.find(key);
    ASSERT_EQ(Get(key), it == model.end() ? "NOT_FOUND" : it->second);
  }
  EXPECT_EQ(PopDeferredSeeks().fallback, 0);
}

// Upper bounds at, inside and past keys, keys that are prefixes of other keys
// and of the bound, and empty and all-zero seek targets.
TEST_F(DBDivaFilterTest, DeferredSeeksPrefixesAndBounds) {
  for (bool l0 : {true, false}) {
    SCOPED_TRACE(l0 ? "L0 files" : "one file per level");
    ReopenEmptyWithDiva();
    std::map<std::string, std::string> model;
    const std::vector<std::vector<std::string>> files = {
        {"ab"}, {"abc"}, {"abd", "b"}, {std::string("ab\x01", 3), "abcd"}};
    int level = 3;
    for (const auto& file : files) {
      for (const std::string& key : file) {
        ASSERT_OK(Put(key, "v" + key));
        model[key] = "v" + key;
      }
      ASSERT_OK(Flush());
      if (!l0 && level > 0) {
        MoveFilesToLevel(level--);
      }
    }
    const std::string zero1(1, '\0');
    const std::string zero2(2, '\0');
    const std::string ab0("ab\0", 3);
    const std::string abc0("abc\0", 4);
    CheckScans(model,
               {"", zero1, zero2, "a", "ab", ab0, "abb", "abc", abc0, "abcd",
                "abd", "abe", "b", std::string("b\0", 2), "c"},
               {"ab", ab0, "abc", abc0, "abcd", "abd", "b",
                std::string("b\0", 2), "c"});
    EXPECT_EQ(PopDeferredSeeks().fallback, 0);
  }
}

// Changing direction or seeking again after children were postponed.
TEST_F(DBDivaFilterTest, DeferredSeeksDirectionChangesAndReseeks) {
  for (bool l0 : {true, false}) {
    SCOPED_TRACE(l0 ? "L0 files" : "one file per level");
    ReopenEmptyWithDiva();
    std::map<std::string, std::string> model;
    WriteDisjointFiles(l0, &model);
    PopDeferredSeeks();

    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->Seek("a");
    ASSERT_GT(PopDeferredSeeks().postponed, 0);
    for (int i = 0; i < 5; ++i) {
      iter->Next();
    }
    ASSERT_EQ(iter->key().ToString(), "a" + Key(5));
    iter->Prev();
    ASSERT_EQ(iter->key().ToString(), "a" + Key(4));
    iter->Next();
    ASSERT_EQ(iter->key().ToString(), "a" + Key(5));

    // Back to the first key, then before it.
    iter->Seek("a");
    iter->Prev();
    ASSERT_FALSE(iter->Valid());
    ASSERT_OK(iter->status());

    // From the end of "a" into the postponed "c" file, backwards and on.
    iter->Seek("a" + Key(99));
    iter->Next();
    ASSERT_EQ(iter->key().ToString(), "c" + Key(0));
    iter->Prev();
    ASSERT_EQ(iter->key().ToString(), "a" + Key(99));
    iter->Next();
    ASSERT_EQ(iter->key().ToString(), "c" + Key(0));

    iter->SeekForPrev("d");
    ASSERT_EQ(iter->key().ToString(), "c" + Key(99));
    iter->Next();
    ASSERT_EQ(iter->key().ToString(), "e" + Key(0));

    iter->Seek("a");
    iter->SeekToLast();
    ASSERT_EQ(iter->key().ToString(), "e" + Key(99));

    // A later target, then an earlier one: nothing is left over.
    iter->Seek("e");
    ASSERT_EQ(iter->key().ToString(), "e" + Key(0));
    iter->Seek("a");
    auto expected = model.begin();
    for (; iter->Valid(); iter->Next(), ++expected) {
      ASSERT_NE(expected, model.end());
      ASSERT_EQ(iter->key().ToString(), expected->first);
    }
    ASSERT_OK(iter->status());
    ASSERT_EQ(expected, model.end());

    iter->SeekToFirst();
    ASSERT_EQ(iter->key().ToString(), "a" + Key(0));
    iter->Next();
    ASSERT_EQ(iter->key().ToString(), "a" + Key(1));
  }
}

// A snapshot read sees the versions it should, whichever children hold the
// newer ones.
TEST_F(DBDivaFilterTest, DeferredSeeksSnapshot) {
  OpenWithDiva();
  std::map<std::string, std::string> model;
  WriteDisjointFiles(/*l0=*/false, &model);
  const Snapshot* snapshot = db_->GetSnapshot();
  const std::map<std::string, std::string> old_model = model;
  for (int i = 0; i < 100; i += 7) {
    for (const char* prefix : {"a", "c", "e"}) {
      const std::string key = prefix + Key(i);
      if (i % 2 == 0) {
        ASSERT_OK(Put(key, "new"));
        model[key] = "new";
      } else {
        ASSERT_OK(Delete(key));
        model.erase(key);
      }
    }
  }
  ASSERT_OK(Put("b", "new"));
  model["b"] = "new";
  ASSERT_OK(Flush());

  for (ReadOptions read_options : AllReads()) {
    SCOPED_TRACE(Describe(read_options));
    for (const std::string& lo : {std::string("a"), "a" + Key(50),
                                  std::string("b"), std::string("d")}) {
      ASSERT_EQ(ScanEntries(read_options, lo, nullptr, SIZE_MAX),
                ModelEntries(model, lo, nullptr, SIZE_MAX));
      read_options.snapshot = snapshot;
      ASSERT_EQ(ScanEntries(read_options, lo, nullptr, SIZE_MAX),
                ModelEntries(old_model, lo, nullptr, SIZE_MAX));
      read_options.snapshot = nullptr;
    }
  }
  db_->ReleaseSnapshot(snapshot);
}

// With lsm_range_filter off, tables give no bounds and skip their range
// check; point lookups still use the filter. A level still says it is empty
// when no file holds keys past the target, from its file key ranges.
TEST_F(DBDivaFilterTest, LsmRangeFilterOff) {
  OpenWithDiva();
  std::map<std::string, std::string> model;
  WriteDisjointFiles(/*l0=*/false, &model);
  PopDeferredSeeks();
  Pop(RANGE_FILTER_USEFUL);
  Pop(RANGE_FILTER_FULL_POSITIVE);
  const std::string upper_bound = "b";
  for (bool deferred_seeks : {true, false}) {
    const ReadOptions read_options =
        Reads(deferred_seeks, /*lsm_range_filter=*/false);
    ASSERT_EQ(ScanEntries(read_options, "a", &upper_bound, SIZE_MAX),
              ModelEntries(model, "a", &upper_bound, SIZE_MAX));
    ASSERT_TRUE(ScanEntries(read_options, "b", &upper_bound, SIZE_MAX).empty());
    ASSERT_TRUE(
        ScanEntries(read_options, "a" + Key(100), &upper_bound, SIZE_MAX)
            .empty());
  }
  const DeferredSeekCounts c = PopDeferredSeeks();
  EXPECT_EQ(c.bounds, c.empty);
  EXPECT_EQ(Pop(RANGE_FILTER_USEFUL), 0);
  EXPECT_EQ(Pop(RANGE_FILTER_FULL_POSITIVE), 0);
  Pop(BLOOM_FILTER_USEFUL);
  ASSERT_EQ(Get("c" + Key(1) + "x"), "NOT_FOUND");
  ASSERT_EQ(Get("c" + Key(1)), "c1");
  EXPECT_GT(Pop(BLOOM_FILTER_USEFUL), 0);
}

// Without a range filter iterators keep the regular Seek(): they record no
// deferred seek stats, with or without range tombstones.
TEST_F(DBDivaFilterTest, DeferredSeeksNeedRangeFilter) {
  Options options = DivaOptions();
  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(NewBloomFilterPolicy(10));
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  options_statistics_ = options.statistics;
  Reopen(options);
  std::map<std::string, std::string> model;
  WriteDisjointFiles(/*l0=*/false, &model);
  ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(),
                             "c" + Key(10), "c" + Key(20)));
  for (int i = 10; i < 20; ++i) {
    model.erase("c" + Key(i));
  }
  PopDeferredSeeks();

  ASSERT_EQ(ScanEntries(ReadOptions(), "c", nullptr, SIZE_MAX),
            ModelEntries(model, "c", nullptr, SIZE_MAX));
  DeferredSeekCounts c = PopDeferredSeeks();
  EXPECT_EQ(c.fallback, 0);
  EXPECT_EQ(c.bounds, 0);

  // Ignoring range tombstones: scan below the deleted range.
  ReadOptions ignore_range_deletions;
  ignore_range_deletions.ignore_range_deletions = true;
  const std::string upper_bound = "c";
  ASSERT_EQ(ScanEntries(ignore_range_deletions, "a", &upper_bound, SIZE_MAX),
            ModelEntries(model, "a", &upper_bound, SIZE_MAX));
  c = PopDeferredSeeks();
  EXPECT_EQ(c.fallback, 0);
  EXPECT_EQ(c.bounds, 0);
}

// Random overwrites and deletes over three levels, L0 files and the memtable,
// with keys that share prefixes and gather in different files. Every scan, in
// every setting and with a snapshot, matches a model, and scans that switch
// direction match it too.
TEST_F(DBDivaFilterTest, RandomizedDeferredSeeksMatchModel) {
  const uint32_t seed = static_cast<uint32_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random rnd(seed);
  OpenWithDiva();

  // Mostly the round's own key group, so files cover different ranges.
  int round = 0;
  const auto random_key = [&]() {
    const int group = rnd.OneIn(4) ? static_cast<int>(rnd.Uniform(8)) : round;
    std::string key = "k" + std::to_string(group);
    const int len = static_cast<int>(rnd.Uniform(8));
    for (int i = 0; i < len; ++i) {
      key.push_back(static_cast<char>('a' + rnd.Uniform(4)));
    }
    return key;
  };

  std::map<std::string, std::string> model;
  std::map<std::string, std::string> snapshot_model;
  const Snapshot* snapshot = nullptr;
  std::vector<std::string> written;
  for (round = 0; round < 8; ++round) {
    for (int i = 0; i < 150; ++i) {
      if (!written.empty() && rnd.OneIn(5)) {
        const std::string& key =
            written[rnd.Uniform(static_cast<int>(written.size()))];
        ASSERT_OK(Delete(key));
        model.erase(key);
      } else {
        const std::string key = random_key();
        const std::string value = "v" + std::to_string(rnd.Next());
        ASSERT_OK(Put(key, value));
        model[key] = value;
        written.push_back(key);
      }
    }
    if (round == 4) {
      snapshot = db_->GetSnapshot();
      snapshot_model = model;
    }
    if (round < 7) {  // the last round stays in the memtable
      ASSERT_OK(Flush());
    }
    if (round < 3) {
      MoveFilesToLevel(3 - round);
    }
  }
  PopDeferredSeeks();

  for (int i = 0; i < 1500; ++i) {
    round = static_cast<int>(rnd.Uniform(8));
    const std::string lo = rnd.OneIn(10) ? std::string() : random_key();
    std::string upper_bound = random_key();
    const bool bounded = rnd.OneIn(2) && lo < upper_bound;
    const size_t limit = rnd.OneIn(3) ? SIZE_MAX : 1 + rnd.Uniform(20);
    const bool use_snapshot = rnd.OneIn(4);
    ReadOptions read_options = AllReads()[rnd.Uniform(4)];
    if (use_snapshot) {
      read_options.snapshot = snapshot;
    }
    const auto& expected_model = use_snapshot ? snapshot_model : model;
    const std::string* upper_bound_ptr = bounded ? &upper_bound : nullptr;
    ASSERT_EQ(ScanEntries(read_options, lo, upper_bound_ptr, limit),
              ModelEntries(expected_model, lo, upper_bound_ptr, limit))
        << Describe(read_options) << " lo=" << lo
        << " upper_bound=" << (bounded ? upper_bound : "none")
        << " limit=" << limit << " snapshot=" << use_snapshot;
  }

  // Forward, a step back, and forward again.
  for (int i = 0; i < 200; ++i) {
    round = static_cast<int>(rnd.Uniform(8));
    const std::string lo = random_key();
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->Seek(lo);
    auto expected = model.lower_bound(lo);
    const int steps = static_cast<int>(rnd.Uniform(10));
    for (int s = 0; s < steps && expected != model.end(); ++s) {
      ASSERT_TRUE(iter->Valid());
      ASSERT_EQ(iter->key().ToString(), expected->first);
      iter->Next();
      ++expected;
    }
    if (expected == model.end() || expected == model.begin()) {
      continue;
    }
    ASSERT_TRUE(iter->Valid());
    iter->Prev();
    --expected;
    ASSERT_TRUE(iter->Valid()) << "lo=" << lo;
    ASSERT_EQ(iter->key().ToString(), expected->first);
    for (int s = 0; s < 5 && expected != model.end(); ++s, ++expected) {
      ASSERT_TRUE(iter->Valid());
      ASSERT_EQ(iter->key().ToString(), expected->first);
      ASSERT_EQ(iter->value().ToString(), expected->second);
      iter->Next();
    }
    ASSERT_OK(iter->status());
  }

  const DeferredSeekCounts c = PopDeferredSeeks();
  EXPECT_GT(c.postponed, 0);
  EXPECT_GT(c.empty + c.past_upper_bound, 0);
  EXPECT_EQ(c.fallback, 0);
  db_->ReleaseSnapshot(snapshot);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
