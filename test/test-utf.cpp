#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <ghoti.io/cutil/utf.h>

using namespace std;

namespace {

vector<GCU_Char16> toUtf16(const string & in, bool expectOk = true) {
  size_t units = gcu_utf8_to_utf16(in.c_str(), nullptr, 0);
  if (!expectOk) {
    return {};
  }
  EXPECT_GT(units, 0u);
  vector<GCU_Char16> out(units);
  EXPECT_EQ(units, gcu_utf8_to_utf16(in.c_str(), out.data(), out.size()));
  return out;
}

string toUtf8(const vector<GCU_Char16> & in) {
  size_t bytes = gcu_utf16_to_utf8(in.data(), nullptr, 0);
  EXPECT_GT(bytes, 0u);
  vector<char> out(bytes);
  EXPECT_EQ(bytes, gcu_utf16_to_utf8(in.data(), out.data(), out.size()));
  return string(out.data());
}

} // namespace

TEST(Utf, AsciiRoundTrips) {
  string in = "/home/user/file.txt";
  ASSERT_EQ(in, toUtf8(toUtf16(in)));
}

TEST(Utf, EveryEncodedLengthRoundTrips) {
  // One code point from each of the four UTF-8 lengths, including one above
  // the BMP so the surrogate pair path runs.
  string in = "Aé世\U0001F600";  // 1, 2, 3, 4 bytes
  auto wide = toUtf16(in);
  // 1 + 1 + 1 + 2 code units, plus the terminator.
  ASSERT_EQ(6u, wide.size());
  ASSERT_EQ(in, toUtf8(wide));
}

TEST(Utf, SurrogatePairIsBuiltCorrectly) {
  string in = "\U0001F600";              // U+1F600
  auto wide = toUtf16(in);
  ASSERT_EQ(3u, wide.size());
  ASSERT_EQ(0xD83Du, wide[0]);
  ASSERT_EQ(0xDE00u, wide[1]);
  ASSERT_EQ(0u, wide[2]);
}

TEST(Utf, MeasuringMatchesConverting) {
  string in = "café \U0001F600";
  size_t measured = gcu_utf8_to_utf16(in.c_str(), nullptr, 0);
  vector<GCU_Char16> out(measured);
  ASSERT_EQ(measured, gcu_utf8_to_utf16(in.c_str(), out.data(), out.size()));
}

TEST(Utf, TooSmallADestinationWritesNothing) {
  string in = "café";
  size_t needed = gcu_utf8_to_utf16(in.c_str(), nullptr, 0);
  ASSERT_GT(needed, 2u);

  vector<GCU_Char16> guard(needed, 0xAAAA);
  // Still reports the requirement, but must not have written a partial
  // string a caller could mistake for a whole one.
  ASSERT_EQ(needed, gcu_utf8_to_utf16(in.c_str(), guard.data(), needed - 1));
  for (size_t i = 0; i < guard.size(); ++i) {
    ASSERT_EQ(0xAAAAu, guard[i]) << "wrote a partial result at index " << i;
  }
}

TEST(Utf, EmptyStringIsValidAndCostsOnlyTheTerminator) {
  ASSERT_TRUE(gcu_utf8_is_valid(""));
  ASSERT_EQ(1u, gcu_utf8_to_utf16("", nullptr, 0));
  GCU_Char16 empty[1] = {0};
  ASSERT_EQ(1u, gcu_utf16_to_utf8(empty, nullptr, 0));
}

TEST(Utf, NullInputIsRejectedNotDereferenced) {
  ASSERT_FALSE(gcu_utf8_is_valid(nullptr));
  ASSERT_EQ(0u, gcu_utf8_to_utf16(nullptr, nullptr, 0));
  ASSERT_EQ(0u, gcu_utf16_to_utf8(nullptr, nullptr, 0));
}

TEST(Utf, OverlongEncodingsAreRejected) {
  // The security-relevant case. "\xC0\xAF" is an overlong '/'; a validator
  // that ran before conversion would not have seen a slash.
  const char * cases[] = {
    "\xC0\xAF",              // overlong '/'
    "\xC0\x80",              // overlong NUL
    "\xC1\xBF",              // overlong 0x7F
    "\xE0\x80\xAF",          // 3-byte overlong '/'
    "\xF0\x80\x80\xAF",      // 4-byte overlong '/'
  };
  for (const char * c : cases) {
    ASSERT_FALSE(gcu_utf8_is_valid(c)) << "accepted overlong: " << c;
    ASSERT_EQ(0u, gcu_utf8_to_utf16(c, nullptr, 0));
  }
}

TEST(Utf, SurrogatesEncodedInUtf8AreRejected) {
  // CESU-8 / WTF-8.  Accepting these produces a lone surrogate in the UTF-16
  // output, which is not a valid string for any Windows API to receive.
  const char * cases[] = {
    "\xED\xA0\x80",   // U+D800, high surrogate
    "\xED\xBF\xBF",   // U+DFFF, low surrogate
  };
  for (const char * c : cases) {
    ASSERT_FALSE(gcu_utf8_is_valid(c)) << "accepted an encoded surrogate";
    ASSERT_EQ(0u, gcu_utf8_to_utf16(c, nullptr, 0));
  }
}

TEST(Utf, AboveTheUnicodeRangeIsRejected) {
  ASSERT_FALSE(gcu_utf8_is_valid("\xF4\x90\x80\x80"));  // U+110000
  ASSERT_FALSE(gcu_utf8_is_valid("\xF5\x80\x80\x80"));  // far above
  ASSERT_FALSE(gcu_utf8_is_valid("\xFF"));              // never valid
  ASSERT_FALSE(gcu_utf8_is_valid("\xFE"));
}

TEST(Utf, TruncatedAndOrphanedSequencesAreRejected) {
  const char * cases[] = {
    "\xE4\xB8",        // 3-byte sequence, only 2 bytes before the NUL
    "\xF0\x9F\x98",    // 4-byte sequence, only 3
    "\x80",            // continuation byte with no lead
    "\xBF",
    "A\xE4\xB8",       // truncated at the end of a longer string
  };
  for (const char * c : cases) {
    ASSERT_FALSE(gcu_utf8_is_valid(c)) << "accepted a truncated sequence";
  }
}

TEST(Utf, TheLastValidCodePointIsAccepted) {
  // The boundary the range check sits on: U+10FFFF must pass, U+110000 must
  // not. A `>` written as `>=` fails exactly here and nowhere else.
  ASSERT_TRUE(gcu_utf8_is_valid("\xF4\x8F\xBF\xBF"));
  ASSERT_FALSE(gcu_utf8_is_valid("\xF4\x90\x80\x80"));
  ASSERT_EQ("\xF4\x8F\xBF\xBF", toUtf8(toUtf16("\xF4\x8F\xBF\xBF")));
}

TEST(Utf, UnpairedSurrogatesInUtf16AreRejected) {
  GCU_Char16 highAlone[] = { 0xD83D, 0 };
  GCU_Char16 lowAlone[]  = { 0xDE00, 0 };
  GCU_Char16 highThenAscii[] = { 0xD83D, 'A', 0 };
  GCU_Char16 twoHighs[] = { 0xD83D, 0xD83D, 0 };

  ASSERT_EQ(0u, gcu_utf16_to_utf8(highAlone, nullptr, 0));
  ASSERT_EQ(0u, gcu_utf16_to_utf8(lowAlone, nullptr, 0));
  ASSERT_EQ(0u, gcu_utf16_to_utf8(highThenAscii, nullptr, 0));
  ASSERT_EQ(0u, gcu_utf16_to_utf8(twoHighs, nullptr, 0));
}

TEST(Utf, EveryBmpCodePointRoundTrips) {
  // A generator rather than examples: the surrogate range is skipped, and
  // everything else must survive UTF-8 -> UTF-16 -> UTF-8 unchanged.
  int checked = 0;
  for (uint32_t cp = 1; cp < 0x10000u; ++cp) {
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
      continue;
    }
    char buf[5] = {0};
    size_t n = 0;
    if (cp < 0x80u) {
      buf[n++] = (char)cp;
    }
    else if (cp < 0x800u) {
      buf[n++] = (char)(0xC0u | (cp >> 6));
      buf[n++] = (char)(0x80u | (cp & 0x3Fu));
    }
    else {
      buf[n++] = (char)(0xE0u | (cp >> 12));
      buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
      buf[n++] = (char)(0x80u | (cp & 0x3Fu));
    }
    buf[n] = '\0';

    ASSERT_TRUE(gcu_utf8_is_valid(buf)) << "rejected U+" << hex << cp;
    ASSERT_EQ(string(buf), toUtf8(toUtf16(buf))) << "U+" << hex << cp;
    ++checked;
  }
  ASSERT_GT(checked, 63000) << "the sweep did not cover the BMP";
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
