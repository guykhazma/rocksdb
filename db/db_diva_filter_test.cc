//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/utilities/options_util.h"
#include "table/block_based/filter_policy_internal.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

namespace {

constexpr uint32_t kSeed = 7;
constexpr uint32_t kInfixBitsPerKey = 10;

std::shared_ptr<const FilterPolicy> NewDiva(uint32_t infix_bits_per_key) {
  return std::shared_ptr<const FilterPolicy>(
      NewDivaFilterPolicy(kSeed, infix_bits_per_key));
}

}  // namespace

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
                                const std::string& upper_bound) {
    ReadOptions read_options;
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

  void OpenWithDiva() {
    Options options = DivaOptions();
    options_statistics_ = options.statistics;
    Reopen(options);
  }

  std::shared_ptr<Statistics> options_statistics_;
};

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
// file's key range. Counters are per table Seek.
TEST_F(DBDivaFilterTest, BoundedSeekSkipsFiles) {
  OpenWithDiva();
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

  Pop(RANGE_FILTER_USEFUL);
  ASSERT_TRUE(Scan("a" + Key(120), "a" + Key(180)).empty());
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

// A range tombstone in a newer level must still hide older keys when the
// newer file's range filter rules out point keys in the seek range.
TEST_F(DBDivaFilterTest, RangeDeletionInNewerLevel) {
  OpenWithDiva();
  for (int i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "old"));
  }
  ASSERT_OK(Flush());
  MoveFilesToLevel(2);

  // The newer file has one point key, outside the range it deletes.
  ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(), Key(0),
                             Key(100)));
  ASSERT_OK(Put("zzz", "v"));
  ASSERT_OK(Flush());
  MoveFilesToLevel(1);
  ASSERT_EQ(NumTableFilesAtLevel(1), 1);
  ASSERT_EQ(NumTableFilesAtLevel(2), 1);

  Pop(RANGE_FILTER_USEFUL);
  ASSERT_TRUE(Scan(Key(10), Key(20)).empty());
  ASSERT_GE(Pop(RANGE_FILTER_USEFUL), 1);
  ASSERT_TRUE(Scan(Key(0), Key(100)).empty());
  ASSERT_EQ(Get(Key(10)), "NOT_FOUND");
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
    Options options = DivaOptions();
    BlockBasedTableOptions table_options;
    table_options.filter_policy = NewDiva(kInfixBitsPerKey);
    table_options.whole_key_filtering = false;
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    check_rejected(options);
  }
  {
    Options options = DivaOptions();
    BlockBasedTableOptions table_options;
    table_options.filter_policy = NewDiva(kInfixBitsPerKey);
    table_options.partition_filters = true;
    table_options.index_type = BlockBasedTableOptions::kTwoLevelIndexSearch;
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    check_rejected(options);
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

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
