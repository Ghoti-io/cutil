#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/cutil/random.h>

TEST(Random, EngineMatchesTheNativeWord) {
  uint32_t seed32 = 0x12345678;
  GCU_Random_MT32_State raw32;
  gcu_random_mt32_init(&raw32, seed32);
  GCU_Random * r32 = gcu_random_mt32(seed32);
  ASSERT_NE(r32, nullptr);
  for (int i = 0; i < GCU_RANDOM_MT_STATE_SIZE32 * 2; ++i) {
    uint32_t word = 0;
    ASSERT_EQ(0, gcu_random_u32(r32, &word));
    EXPECT_EQ(gcu_random_mt32_next(&raw32), word);
  }
  gcu_random_free(r32);

  uint64_t seed64 = 0x1234567890ABCDEF;
  GCU_Random_MT64_State raw64;
  gcu_random_mt64_init(&raw64, seed64);
  GCU_Random * r64 = gcu_random_mt64(seed64);
  ASSERT_NE(r64, nullptr);
  for (int i = 0; i < GCU_RANDOM_MT_STATE_SIZE64 * 2; ++i) {
    uint64_t word = 0;
    ASSERT_EQ(0, gcu_random_u64(r64, &word));
    EXPECT_EQ(gcu_random_mt64_next(&raw64), word);
  }
  gcu_random_free(r64);
}

TEST(Random, Mt32U64IsTwoWordsLowFirst) {
  uint32_t seed = 0x12345678;
  GCU_Random * wide = gcu_random_mt32(seed);
  GCU_Random * narrow = gcu_random_mt32(seed);
  ASSERT_NE(wide, nullptr);
  ASSERT_NE(narrow, nullptr);
  uint64_t word = 0;
  uint32_t low = 0;
  uint32_t high = 0;
  ASSERT_EQ(0, gcu_random_u64(wide, &word));
  ASSERT_EQ(0, gcu_random_u32(narrow, &low));
  ASSERT_EQ(0, gcu_random_u32(narrow, &high));
  EXPECT_EQ(low, (uint32_t)word);
  EXPECT_EQ(high, (uint32_t)(word >> 32));
  gcu_random_free(wide);
  gcu_random_free(narrow);
}

TEST(Random, PartialBytesStayInTheEngine) {
  uint32_t seed = 1;
  GCU_Random * whole = gcu_random_mt32(seed);
  GCU_Random * split = gcu_random_mt32(seed);
  ASSERT_NE(whole, nullptr);
  ASSERT_NE(split, nullptr);
  unsigned char one[4];
  unsigned char rest[3];
  unsigned char four[4];
  ASSERT_EQ(0, gcu_random_bytes(whole, four, 4));
  ASSERT_EQ(0, gcu_random_bytes(split, one, 1));
  ASSERT_EQ(0, gcu_random_bytes(split, rest, 3));
  EXPECT_EQ(four[0], one[0]);
  EXPECT_EQ(0, memcmp(four + 1, rest, 3));
  gcu_random_free(whole);
  gcu_random_free(split);
}

TEST(Random, FloatIsTheTop53Bits) {
  GCU_Random * r = gcu_random_mt64(1);
  ASSERT_NE(r, nullptr);
  uint64_t word = 0;
  double value = -1.0;
  ASSERT_EQ(0, gcu_random_u64(r, &word));
  gcu_random_free(r);
  r = gcu_random_mt64(1);
  ASSERT_EQ(0, gcu_random_f64(r, &value));
  EXPECT_GE(value, 0.0);
  EXPECT_LT(value, 1.0);
  EXPECT_DOUBLE_EQ((double)(word >> 11) * (1.0 / 9007199254740992.0), value);
  gcu_random_free(r);
}

struct Seq {
  const uint64_t * words;
  size_t count;
  size_t index;
};

static int seq_fill(void * ctx, void * out, size_t n) {
  Seq * seq = static_cast<Seq *>(ctx);
  auto * p = static_cast<unsigned char *>(out);
  while (n != 0) {
    if (seq->index >= seq->count || n < 8) {
      memset(out, 0, n);
      return -1;
    }
    uint64_t w = seq->words[seq->index++];
    for (int b = 0; b < 8; ++b) {
      p[b] = static_cast<unsigned char>(w >> (8 * b));
    }
    p += 8;
    n -= 8;
  }
  return 0;
}

TEST(Random, BelowRejectsAndIsUnbiased) {
  const uint64_t words[] = { 0, 5 };
  Seq seq = { words, 2, 0 };
  GCU_Random_Engine engine = { &seq, seq_fill, NULL };
  GCU_Random * r = gcu_random_from_engine(&engine);
  ASSERT_NE(r, nullptr);
  uint64_t out = 99;
  EXPECT_NE(0, gcu_random_below(r, 0, &out));
  EXPECT_EQ(99u, out);
  out = 99;
  ASSERT_EQ(0, gcu_random_below(r, ~uint64_t{0}, &out));
  EXPECT_EQ(4u, out);
  EXPECT_EQ(2u, seq.index);
  gcu_random_free(r);
}

static int fail_fill(void * ctx, void * out, size_t n) {
  (void)ctx;
  memset(out, 0xa5, n);
  return -1;
}

static int destroyed = 0;

static void mark_destroyed(void * ctx) {
  (void)ctx;
  destroyed = 1;
}

TEST(Random, FailureWipesAndDestroyRuns) {
  GCU_Random_Engine engine = { NULL, fail_fill, mark_destroyed };
  GCU_Random * r = gcu_random_from_engine(&engine);
  ASSERT_NE(r, nullptr);
  uint64_t out = 0x1111;
  EXPECT_NE(0, gcu_random_u64(r, &out));
  EXPECT_EQ(0u, out);
  unsigned char buf[4] = { 1, 2, 3, 4 };
  EXPECT_NE(0, gcu_random_bytes(r, buf, 4));
  EXPECT_EQ(0, buf[0]);
  EXPECT_EQ(0, buf[3]);
  destroyed = 0;
  gcu_random_free(r);
  EXPECT_EQ(1, destroyed);
  gcu_random_free(NULL);
  EXPECT_EQ(NULL, gcu_random_from_engine(NULL));
  GCU_Random_Engine no_fill = { NULL, NULL, NULL };
  EXPECT_EQ(NULL, gcu_random_from_engine(&no_fill));
}

TEST(Random, PlaceDoesNotUseTheHeap) {
  alignas(void *) unsigned char storage[64];
  ASSERT_GE(sizeof storage, gcu_random_handle_size());
  size_t allocs = gcu_get_alloc_count();
  size_t frees = gcu_get_free_count();
  destroyed = 0;
  GCU_Random_Engine engine = { NULL, fail_fill, mark_destroyed };
  GCU_Random * r = gcu_random_place(storage, sizeof storage, &engine);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(allocs, gcu_get_alloc_count());
  gcu_random_free(r);
  EXPECT_EQ(1, destroyed);
  EXPECT_EQ(frees, gcu_get_free_count());
  EXPECT_EQ(NULL, gcu_random_place(storage, 1, &engine));
}

static void ExpectU32(GCU_Random * r, const uint32_t * words, int count) {
  for (int i = 0; i < count; ++i) {
    uint32_t word = 0;
    ASSERT_EQ(0, gcu_random_u32(r, &word));
    EXPECT_EQ(words[i], word) << i;
  }
}

static void ExpectU64(GCU_Random * r, const uint64_t * words, int count) {
  for (int i = 0; i < count; ++i) {
    uint64_t word = 0;
    ASSERT_EQ(0, gcu_random_u64(r, &word));
    EXPECT_EQ(words[i], word) << i;
  }
}

TEST(Random, JavaMatchesNextInt) {
  // java.util.Random(1).nextInt() is -1155869325, which is this bit pattern.
  const uint32_t words[] = {
    0xBB1AD573u, 0x19B89CD8u, 0x68FB0E6Fu, 0x684DF992u
  };
  GCU_Random * r = gcu_random_java(1);
  ASSERT_NE(r, nullptr);
  ExpectU32(r, words, 4);
  gcu_random_free(r);
}

TEST(Random, SplitMix64) {
  const uint64_t words[] = {
    0xE220A8397B1DCDAFULL, 0x6E789E6AA1B965F4ULL,
    0x06C45D188009454FULL, 0xF88BB8A8724C81ECULL
  };
  GCU_Random * r = gcu_random_splitmix64(0);
  ASSERT_NE(r, nullptr);
  ExpectU64(r, words, 4);
  gcu_random_free(r);
}

TEST(Random, Xoshiro256pp) {
  const uint64_t words[] = {
    0xCFC5D07F6F03C29BULL, 0xBF424132963FE08DULL,
    0x19A37D5757AAF520ULL, 0xBF08119F05CD56D6ULL
  };
  GCU_Random * r = gcu_random_xoshiro256pp(1);
  ASSERT_NE(r, nullptr);
  ExpectU64(r, words, 4);
  gcu_random_free(r);
}

TEST(Random, Pcg64) {
  /* pcg64_srandom_r(rng, 1, 0) then pcg64_random_r. The draw advances
   * before it outputs, so this is not the state srandom leaves behind. */
  const uint64_t words[] = {
    0x71564BA1920863F1ULL, 0x06F710DFF5126DAFULL,
    0xAF595B987D60EA49ULL, 0xA3D0BB4A02495B7FULL
  };
  GCU_Random * r = gcu_random_pcg64(1);
  ASSERT_NE(r, nullptr);
  ExpectU64(r, words, 4);
  gcu_random_free(r);
}

TEST(Random, TwoHandlesDoNotInterfere) {
  GCU_Random_MT64_State raw_a;
  GCU_Random_MT64_State raw_b;
  gcu_random_mt64_init(&raw_a, 1);
  gcu_random_mt64_init(&raw_b, 2);
  GCU_Random * a = gcu_random_mt64(1);
  GCU_Random * b = gcu_random_mt64(2);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  for (int i = 0; i < 8; ++i) {
    uint64_t wa = 0;
    uint64_t wb = 0;
    ASSERT_EQ(0, gcu_random_u64(b, &wb));
    ASSERT_EQ(0, gcu_random_u64(a, &wa));
    EXPECT_EQ(gcu_random_mt64_next(&raw_b), wb);
    EXPECT_EQ(gcu_random_mt64_next(&raw_a), wa);
  }
  gcu_random_free(a);
  gcu_random_free(b);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

