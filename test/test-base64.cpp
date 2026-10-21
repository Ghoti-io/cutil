#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <ghoti.io/cutil/base64.h>

using namespace std;

namespace {

GCU_Base64_Decode_Options strict() {
  GCU_Base64_Decode_Options o;
  memset(&o, 0, sizeof o);
  return o;
}

string encode(const string & in, GCU_Base64_Alphabet a = GCU_BASE64_ALPHABET_STANDARD,
    GCU_Base64_Encode_Padding p = GCU_BASE64_ENCODE_PADDING_ALWAYS) {
  size_t need = 0;
  EXPECT_TRUE(gcu_base64_encoded_size(in.size(), p, &need));
  string buffer(need + 8, '\x7f');  // the guard bytes after the result must stay untouched
  size_t written = 0;
  EXPECT_EQ(gcu_base64_encode(in.data(), in.size(), buffer.data(), need, a, p, &written), GCU_BASE64_OK);
  EXPECT_EQ(written, need);
  EXPECT_EQ(buffer.substr(need), string(8, '\x7f')) << "wrote past the size it reported";
  return buffer.substr(0, need);
}

struct Decoded {
  GCU_Base64_Status status;
  string bytes;
  size_t offset;
};

Decoded decode(const string & in, const GCU_Base64_Decode_Options & o) {
  Decoded d;
  d.offset = 12345;
  string out(gcu_base64_decoded_size_max(in.size()) + 1, '\x7f');
  size_t written = 0;
  d.status = gcu_base64_decode(in.data(), in.size(), out.data(), out.size() - 1, &o, &written, &d.offset);
  if (d.status == GCU_BASE64_OK) {
    d.bytes = out.substr(0, written);
    EXPECT_EQ(out.back(), '\x7f') << "wrote past the end of the capacity";
  } else {
    for (size_t i = 0; i < out.size(); i++) {
      EXPECT_EQ(out[i], '\x7f') << "wrote on an error, at " << i;
    }
  }
  return d;
}

// An independent encoder: one bit at a time, sharing nothing with the codec's grouping.
string reference_encode(const string & in, const char * table, bool pad) {
  string out;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : in) {
    for (int b = 7; b >= 0; b--) {
      acc = (acc << 1) | ((c >> b) & 1);
      if (++bits == 6) {
        out += table[acc & 63];
        acc = 0;
        bits = 0;
      }
    }
  }
  if (bits) {
    out += table[(acc << (6 - bits)) & 63];
  }
  if (pad) {
    while (out.size() % 4) {
      out += '=';
    }
  }
  return out;
}

const char * kStd = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char * kUrl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace

TEST(Base64, Rfc4648Section10Vectors) {
  const char * plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
  const char * coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
  for (int i = 0; i < 7; i++) {
    EXPECT_EQ(encode(plain[i]), coded[i]) << plain[i];
    Decoded d = decode(coded[i], strict());
    EXPECT_EQ(d.status, GCU_BASE64_OK) << coded[i];
    EXPECT_EQ(d.bytes, plain[i]);
  }
}

TEST(Base64, NoPaddingEncoding) {
  EXPECT_EQ(encode("f", GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_NONE), "Zg");
  EXPECT_EQ(encode("fo", GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_NONE), "Zm8");
  EXPECT_EQ(encode("foo", GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_NONE), "Zm9v");
}

TEST(Base64, UrlAlphabet) {
  string bytes("\xfb\xef\xbe\xff\xff\xff", 6);
  EXPECT_EQ(encode(bytes), "++++////");
  EXPECT_EQ(encode(bytes, GCU_BASE64_ALPHABET_URL), "----____");
  GCU_Base64_Decode_Options url = strict();
  url.alphabet = GCU_BASE64_ALPHABET_URL;
  EXPECT_EQ(decode("----____", url).bytes, bytes);
  Decoded d = decode("++++////", url);  // the other alphabet's characters
  EXPECT_EQ(d.status, GCU_BASE64_ERR_CHARACTER);
  EXPECT_EQ(d.offset, 0u);
  d = decode("----____", strict());
  EXPECT_EQ(d.status, GCU_BASE64_ERR_CHARACTER);
  EXPECT_EQ(d.offset, 0u);
}

struct Row {
  const char * input;
  GCU_Base64_Padding padding;
  GCU_Base64_Whitespace whitespace;
  GCU_Base64_Trailing_Bits trailing;
  GCU_Base64_Status status;
  size_t offset;
  const char * bytes;
};

TEST(Base64, TheMatrix) {
  const auto R = GCU_BASE64_PADDING_REQUIRED, O = GCU_BASE64_PADDING_OPTIONAL, F = GCU_BASE64_PADDING_FORBIDDEN;
  const auto REJ = GCU_BASE64_WHITESPACE_REJECT, SKIP = GCU_BASE64_WHITESPACE_SKIP_ASCII;
  const auto ZERO = GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO, IGN = GCU_BASE64_TRAILING_BITS_IGNORE;
  const Row rows[] = {
      // padding
      {"Zg==", R, REJ, ZERO, GCU_BASE64_OK, 0, "f"},
      {"Zg", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 2, ""},
      {"Zg", O, REJ, ZERO, GCU_BASE64_OK, 0, "f"},
      {"Zg", F, REJ, ZERO, GCU_BASE64_OK, 0, "f"},
      {"Zg==", F, REJ, ZERO, GCU_BASE64_ERR_PADDING, 2, ""},
      {"Zg=", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 3, ""},
      {"Zg=", O, REJ, ZERO, GCU_BASE64_ERR_PADDING, 3, ""},
      {"Zg===", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 4, ""},
      {"Zm9v=", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 4, ""},
      {"=Zg=", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 0, ""},
      {"Z=", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 1, ""},
      {"Zg==Zm9v", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 4, ""},
      {"Zg==Zg==", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 4, ""},
      {"Zm8=", R, REJ, ZERO, GCU_BASE64_OK, 0, "fo"},
      {"Zm8", O, REJ, ZERO, GCU_BASE64_OK, 0, "fo"},
      {"Zm8", R, REJ, ZERO, GCU_BASE64_ERR_PADDING, 3, ""},
      // length
      {"Z", O, REJ, ZERO, GCU_BASE64_ERR_LENGTH, 1, ""},
      {"Zm9vY", O, REJ, ZERO, GCU_BASE64_ERR_LENGTH, 5, ""},
      {"Zm9vY", R, REJ, ZERO, GCU_BASE64_ERR_LENGTH, 5, ""},
      // trailing bits
      {"Zh==", R, REJ, ZERO, GCU_BASE64_ERR_TRAILING_BITS, 1, ""},
      {"Zh==", R, REJ, IGN, GCU_BASE64_OK, 0, "f"},
      {"Zm9=", R, REJ, ZERO, GCU_BASE64_ERR_TRAILING_BITS, 2, ""},
      {"Zm9=", R, REJ, IGN, GCU_BASE64_OK, 0, "fo"},
      {"Zh", O, REJ, ZERO, GCU_BASE64_ERR_TRAILING_BITS, 1, ""},
      {"Zh", O, REJ, IGN, GCU_BASE64_OK, 0, "f"},
      // whitespace
      {"Zm9v\nYmFy", R, SKIP, ZERO, GCU_BASE64_OK, 0, "foobar"},
      {"Zm9v\nYmFy", R, REJ, ZERO, GCU_BASE64_ERR_CHARACTER, 4, ""},
      {" Z m 9 v ", R, SKIP, ZERO, GCU_BASE64_OK, 0, "foo"},
      {"Zg==\n", R, SKIP, ZERO, GCU_BASE64_OK, 0, "f"},
      {"Zg= =", R, SKIP, ZERO, GCU_BASE64_ERR_PADDING, 3, ""},
      {"Zg=\n=", R, SKIP, ZERO, GCU_BASE64_ERR_PADDING, 3, ""},
      {"Z\vg==", R, SKIP, ZERO, GCU_BASE64_ERR_CHARACTER, 1, ""},  // VT is not skipped
      // characters
      {"Zm9v\xff", R, REJ, ZERO, GCU_BASE64_ERR_CHARACTER, 4, ""},
      {"Zm.v", R, REJ, ZERO, GCU_BASE64_ERR_CHARACTER, 2, ""},
      {"", R, REJ, ZERO, GCU_BASE64_OK, 0, ""},
      {"\n", R, SKIP, ZERO, GCU_BASE64_OK, 0, ""},
  };
  for (const Row & r : rows) {
    GCU_Base64_Decode_Options o = strict();
    o.padding = r.padding;
    o.whitespace = r.whitespace;
    o.trailing_bits = r.trailing;
    string in(r.input);
    Decoded d = decode(in, o);
    EXPECT_EQ(d.status, r.status) << "input '" << in << "'";
    if (r.status == GCU_BASE64_OK) {
      EXPECT_EQ(d.bytes, r.bytes) << "input '" << in << "'";
    } else {
      EXPECT_EQ(d.offset, r.offset) << "input '" << in << "'";
    }
  }
}

TEST(Base64, NulInTheMiddleIsACharacterError) {
  string in("Zm9v\0Zm9v", 9);
  Decoded d = decode(in, strict());
  EXPECT_EQ(d.status, GCU_BASE64_ERR_CHARACTER);
  EXPECT_EQ(d.offset, 4u);
}

TEST(Base64, CapacityAndMeasuring) {
  char out[8];
  size_t written = 0, offset = 0;
  EXPECT_EQ(gcu_base64_decode("Zm9vYmFy", 8, out, 5, NULL, &written, &offset), GCU_BASE64_ERR_CAPACITY);
  EXPECT_EQ(written, 6u);
  EXPECT_EQ(gcu_base64_decode("Zm9vYmFy", 8, NULL, 0, NULL, &written, &offset), GCU_BASE64_OK);
  EXPECT_EQ(written, 6u);
  EXPECT_EQ(gcu_base64_decode("Zm9vYmFy", 8, out, 6, NULL, &written, &offset), GCU_BASE64_OK);
  EXPECT_EQ(string(out, 6), "foobar");
  // An invalid input reports its error before its size.
  EXPECT_EQ(gcu_base64_decode("Zm9v!mFy", 8, out, 0, NULL, &written, &offset), GCU_BASE64_ERR_CHARACTER);
  char enc[8];
  EXPECT_EQ(gcu_base64_encode("foobar", 6, enc, 7, GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_ALWAYS, &written),
      GCU_BASE64_ERR_CAPACITY);
  EXPECT_EQ(written, 8u);
  EXPECT_EQ(gcu_base64_encode("", 0, NULL, 0, GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_ALWAYS, &written),
      GCU_BASE64_OK);
  EXPECT_EQ(written, 0u);
}

TEST(Base64, SizesAtTheOverflowBoundary) {
  size_t out = 7;
  EXPECT_FALSE(gcu_base64_encoded_size(SIZE_MAX, GCU_BASE64_ENCODE_PADDING_ALWAYS, &out));
  EXPECT_EQ(out, 7u) << "untouched on overflow";
  EXPECT_FALSE(gcu_base64_encoded_size((SIZE_MAX / 4) * 3 + 3, GCU_BASE64_ENCODE_PADDING_ALWAYS, &out));
  size_t just_fits = (SIZE_MAX / 4) * 3;
  EXPECT_TRUE(gcu_base64_encoded_size(just_fits, GCU_BASE64_ENCODE_PADDING_ALWAYS, &out));
  EXPECT_EQ(out, (SIZE_MAX / 4) * 4);
  size_t written = 0;
  EXPECT_EQ(gcu_base64_encode("x", SIZE_MAX, NULL, 0, GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_ALWAYS, &written),
      GCU_BASE64_ERR_OVERFLOW);
  EXPECT_GE(gcu_base64_decoded_size_max(SIZE_MAX), SIZE_MAX / 4 * 3);
  EXPECT_EQ(gcu_base64_decoded_size_max(0), 3u);
  EXPECT_EQ(gcu_base64_decoded_size_max(4), 6u);
}

TEST(Base64, InvalidArguments) {
  char out[8];
  size_t written;
  EXPECT_EQ(gcu_base64_encode(NULL, 3, out, 8, GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_ENCODE_PADDING_ALWAYS, &written),
      GCU_BASE64_ERR_INVALID);
  EXPECT_EQ(gcu_base64_encode("abc", 3, out, 8, (GCU_Base64_Alphabet) 7, GCU_BASE64_ENCODE_PADDING_ALWAYS, &written),
      GCU_BASE64_ERR_INVALID);
  EXPECT_EQ(gcu_base64_decode(NULL, 4, out, 8, NULL, &written, NULL), GCU_BASE64_ERR_INVALID);
  GCU_Base64_Decode_Options o = strict();
  o.padding = (GCU_Base64_Padding) 9;
  EXPECT_EQ(gcu_base64_decode("Zg==", 4, out, 8, &o, &written, NULL), GCU_BASE64_ERR_INVALID);
  EXPECT_STREQ(gcu_base64_status_string((GCU_Base64_Status) 99), "unknown");
  for (int s = 0; s < GCU_BASE64_STATUS_COUNT; s++) {
    EXPECT_STRNE(gcu_base64_status_string((GCU_Base64_Status) s), "unknown") << s;
  }
}

TEST(Base64, DecodeMayBeInPlace) {
  char buffer[16];
  memcpy(buffer, "Zm9vYmFy", 8);
  size_t written = 0;
  ASSERT_EQ(gcu_base64_decode(buffer, 8, buffer, sizeof buffer, NULL, &written, NULL), GCU_BASE64_OK);
  EXPECT_EQ(string(buffer, written), "foobar");
  memcpy(buffer, "Zm8=", 4);
  ASSERT_EQ(gcu_base64_decode(buffer, 4, buffer, sizeof buffer, NULL, &written, NULL), GCU_BASE64_OK);
  EXPECT_EQ(string(buffer, written), "fo");
}

TEST(Base64, RandomBuffersAgainstAnIndependentEncoder) {
  uint64_t state = 0x9e3779b97f4a7c15ull;
  auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (int round = 0; round < 20000; round++) {
    size_t n = next() % 200;
    string in(n, '\0');
    for (char & c : in) {
      c = (char) next();
    }
    for (int a = 0; a < 2; a++) {
      for (int p = 0; p < 2; p++) {
        auto alphabet = a ? GCU_BASE64_ALPHABET_URL : GCU_BASE64_ALPHABET_STANDARD;
        auto padding = p ? GCU_BASE64_ENCODE_PADDING_NONE : GCU_BASE64_ENCODE_PADDING_ALWAYS;
        string got = encode(in, alphabet, padding);
        ASSERT_EQ(got, reference_encode(in, a ? kUrl : kStd, !p)) << round;
        GCU_Base64_Decode_Options o = strict();
        o.alphabet = alphabet;
        o.padding = p ? GCU_BASE64_PADDING_FORBIDDEN : GCU_BASE64_PADDING_REQUIRED;
        Decoded d = decode(got, o);
        ASSERT_EQ(d.status, GCU_BASE64_OK) << round;
        ASSERT_EQ(d.bytes, in) << round;
      }
    }
  }
}

TEST(Base64, PythonVectors) {
  ifstream f("test/data/base64-vectors.txt");
  ASSERT_TRUE(f.good()) << "run from the library directory";
  string line;
  size_t rows = 0;
  while (getline(f, line)) {
    istringstream ls(line);
    string alphabet, padding, hex, enc;
    ls >> alphabet >> padding >> hex >> enc;
    string in;
    if (hex != "-") {
      for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        in += (char) stoi(hex.substr(i, 2), nullptr, 16);
      }
    }
    if (enc == "-") {
      enc.clear();
    }
    auto a = alphabet == "url" ? GCU_BASE64_ALPHABET_URL : GCU_BASE64_ALPHABET_STANDARD;
    bool pad = padding == "pad";
    EXPECT_EQ(encode(in, a, pad ? GCU_BASE64_ENCODE_PADDING_ALWAYS : GCU_BASE64_ENCODE_PADDING_NONE), enc) << line;
    GCU_Base64_Decode_Options o = strict();
    o.alphabet = a;
    o.padding = pad ? GCU_BASE64_PADDING_REQUIRED : GCU_BASE64_PADDING_FORBIDDEN;
    Decoded d = decode(enc, o);
    EXPECT_EQ(d.status, GCU_BASE64_OK) << line;
    EXPECT_EQ(d.bytes, in) << line;
    rows++;
  }
  EXPECT_GT(rows, 200u);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
