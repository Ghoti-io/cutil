/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io CUtil.
 *
 * Ghoti.io CUtil is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io CUtil is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 * Base64 encoding and strict decoding. See base64.h for the contract.
 */

#include <ghoti.io/cutil/base64.h>

#include <ghoti.io/cutil/safemath.h>

#include <stdint.h>

static const char k_standard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char k_url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const char * gcu_base64_status_string(GCU_Base64_Status status) {
  switch (status) {
    case GCU_BASE64_OK: return "ok";
    case GCU_BASE64_ERR_CHARACTER: return "character outside the alphabet";
    case GCU_BASE64_ERR_PADDING: return "bad padding";
    case GCU_BASE64_ERR_LENGTH: return "impossible length";
    case GCU_BASE64_ERR_TRAILING_BITS: return "non-zero trailing bits";
    case GCU_BASE64_ERR_CAPACITY: return "output buffer too small";
    case GCU_BASE64_ERR_OVERFLOW: return "size overflow";
    case GCU_BASE64_ERR_INVALID: return "invalid argument";
    case GCU_BASE64_STATUS_COUNT: break;
  }
  return "unknown";
}

bool gcu_base64_encoded_size(size_t n, GCU_Base64_Encode_Padding padding, size_t * out) {
  size_t whole = n / 3, rest = n % 3, groups = whole, chars;
  if (padding == GCU_BASE64_ENCODE_PADDING_ALWAYS) {
    groups += rest ? 1 : 0;
    if (!gcu_safe_mul_size(groups, 4, &chars)) {
      return false;
    }
  } else {
    if (!gcu_safe_mul_size(whole, 4, &chars)) {
      return false;
    }
    if (rest && !gcu_safe_add_size(chars, rest + 1, &chars)) {
      return false;
    }
  }
  if (out) {
    *out = chars;
  }
  return true;
}

size_t gcu_base64_decoded_size_max(size_t n) {
  /* ceil(3n / 4) without forming 3n: n/4 groups of three bytes, and up to three more. */
  return (n / 4) * 3 + 3;
}

GCU_Base64_Status gcu_base64_encode(const void * in, size_t n, char * out, size_t cap,
    GCU_Base64_Alphabet alphabet, GCU_Base64_Encode_Padding padding, size_t * written) {
  if ((n && !in) || alphabet >= GCU_BASE64_ALPHABET_COUNT || padding >= GCU_BASE64_ENCODE_PADDING_COUNT) {
    return GCU_BASE64_ERR_INVALID;
  }
  size_t need;
  if (!gcu_base64_encoded_size(n, padding, &need)) {
    return GCU_BASE64_ERR_OVERFLOW;
  }
  if (written) {
    *written = need;
  }
  if (need > cap) {
    return GCU_BASE64_ERR_CAPACITY;
  }
  if (need && !out) {
    return GCU_BASE64_ERR_INVALID;
  }
  const char * table = alphabet == GCU_BASE64_ALPHABET_URL ? k_url : k_standard;
  const uint8_t * p = (const uint8_t *) in;
  size_t o = 0, i = 0;
  for (; n - i >= 3; i += 3) {
    uint32_t v = ((uint32_t) p[i] << 16) | ((uint32_t) p[i + 1] << 8) | p[i + 2];
    out[o++] = table[(v >> 18) & 63];
    out[o++] = table[(v >> 12) & 63];
    out[o++] = table[(v >> 6) & 63];
    out[o++] = table[v & 63];
  }
  size_t rest = n - i;
  if (rest == 1) {
    uint32_t v = (uint32_t) p[i] << 16;
    out[o++] = table[(v >> 18) & 63];
    out[o++] = table[(v >> 12) & 63];
    if (padding == GCU_BASE64_ENCODE_PADDING_ALWAYS) {
      out[o++] = '=';
      out[o++] = '=';
    }
  } else if (rest == 2) {
    uint32_t v = ((uint32_t) p[i] << 16) | ((uint32_t) p[i + 1] << 8);
    out[o++] = table[(v >> 18) & 63];
    out[o++] = table[(v >> 12) & 63];
    out[o++] = table[(v >> 6) & 63];
    if (padding == GCU_BASE64_ENCODE_PADDING_ALWAYS) {
      out[o++] = '=';
    }
  }
  return GCU_BASE64_OK;
}

/** The value of a character in an alphabet, or -1. */
static int value_of(unsigned char c, GCU_Base64_Alphabet alphabet) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (alphabet == GCU_BASE64_ALPHABET_STANDARD) {
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  }
  return c == '-' ? 62 : c == '_' ? 63 : -1;
}

static bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

/**
 * One pass over the input. With `out` NULL it only validates and counts; with `out` set it also writes, which
 * the caller does only after a validating pass has succeeded, so a write pass cannot fail.
 */
static GCU_Base64_Status scan(const char * in, size_t n, uint8_t * out, const GCU_Base64_Decode_Options * o,
    size_t * count, size_t * error_offset) {
  uint32_t acc = 0;
  unsigned have = 0;       /* symbols in the current group, 0 to 3 */
  unsigned pads = 0;       /* '=' seen in the current group */
  bool group_done_padded = false; /* a padded group has been completed: only whitespace may follow */
  size_t produced = 0;
  size_t last_data = 0;
#define FAIL(status, offset) \
  do { \
    if (error_offset) { \
      *error_offset = (offset); \
    } \
    return (status); \
  } while (0)
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char) in[i];
    if (c == '=') {
      if (o->padding == GCU_BASE64_PADDING_FORBIDDEN || group_done_padded || have < 2) {
        FAIL(GCU_BASE64_ERR_PADDING, i);
      }
      pads++;
      if (have + pads > 4) {
        FAIL(GCU_BASE64_ERR_PADDING, i);
      }
      if (have + pads == 4) {
        /* The group is complete: emit its bytes. */
        if (have == 2) {
          if (o->trailing_bits == GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO && (acc & 0x0f)) {
            FAIL(GCU_BASE64_ERR_TRAILING_BITS, last_data);
          }
          if (out) {
            out[produced] = (uint8_t) (acc >> 4);
          }
          produced += 1;
        } else {
          if (o->trailing_bits == GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO && (acc & 0x03)) {
            FAIL(GCU_BASE64_ERR_TRAILING_BITS, last_data);
          }
          if (out) {
            out[produced] = (uint8_t) (acc >> 10);
            out[produced + 1] = (uint8_t) (acc >> 2);
          }
          produced += 2;
        }
        group_done_padded = true;
        acc = 0;
        have = 0;
        pads = 0;
      }
      continue;
    }
    if (is_space(c) && o->whitespace == GCU_BASE64_WHITESPACE_SKIP_ASCII) {
      if (pads) {
        FAIL(GCU_BASE64_ERR_PADDING, i); /* whitespace inside an unfinished padding */
      }
      continue;
    }
    int v = value_of(c, o->alphabet);
    if (v < 0) {
      FAIL(GCU_BASE64_ERR_CHARACTER, i);
    }
    if (group_done_padded || pads) {
      FAIL(GCU_BASE64_ERR_PADDING, i); /* data after padding */
    }
    acc = (acc << 6) | (uint32_t) v;
    have++;
    last_data = i;
    if (have == 4) {
      if (out) {
        out[produced] = (uint8_t) (acc >> 16);
        out[produced + 1] = (uint8_t) (acc >> 8);
        out[produced + 2] = (uint8_t) acc;
      }
      produced += 3;
      acc = 0;
      have = 0;
    }
  }
  if (pads) {
    FAIL(GCU_BASE64_ERR_PADDING, n); /* an unfinished padding */
  }
  if (have == 1) {
    FAIL(GCU_BASE64_ERR_LENGTH, n);
  }
  if (have) {
    if (o->padding == GCU_BASE64_PADDING_REQUIRED) {
      FAIL(GCU_BASE64_ERR_PADDING, n);
    }
    uint32_t spare_mask = have == 2 ? 0x0f : 0x03;
    if (o->trailing_bits == GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO && (acc & spare_mask)) {
      FAIL(GCU_BASE64_ERR_TRAILING_BITS, last_data);
    }
    if (out) {
      if (have == 2) {
        out[produced] = (uint8_t) (acc >> 4);
      } else {
        out[produced] = (uint8_t) (acc >> 10);
        out[produced + 1] = (uint8_t) (acc >> 2);
      }
    }
    produced += have - 1;
  }
#undef FAIL
  *count = produced;
  return GCU_BASE64_OK;
}

GCU_Base64_Status gcu_base64_decode(const char * in, size_t n, void * out, size_t cap,
    const GCU_Base64_Decode_Options * options, size_t * written, size_t * error_offset) {
  static const GCU_Base64_Decode_Options strictest = {GCU_BASE64_ALPHABET_STANDARD, GCU_BASE64_PADDING_REQUIRED,
      GCU_BASE64_WHITESPACE_REJECT, GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO};
  const GCU_Base64_Decode_Options * o = options ? options : &strictest;
  if ((n && !in) || o->alphabet >= GCU_BASE64_ALPHABET_COUNT || o->padding >= GCU_BASE64_PADDING_COUNT ||
      o->whitespace >= GCU_BASE64_WHITESPACE_COUNT || o->trailing_bits >= GCU_BASE64_TRAILING_BITS_COUNT) {
    return GCU_BASE64_ERR_INVALID;
  }
  size_t count = 0;
  GCU_Base64_Status s = scan(in ? in : "", n, NULL, o, &count, error_offset);
  if (s != GCU_BASE64_OK) {
    return s;
  }
  if (written) {
    *written = count;
  }
  if (!out) {
    return GCU_BASE64_OK;
  }
  if (count > cap) {
    return GCU_BASE64_ERR_CAPACITY;
  }
  if (count) {
    /* Validated above, so this pass cannot fail. Writes never overtake reads (a byte is written only after the
     * characters that make it are read), so out == in works. */
    scan(in, n, (uint8_t *) out, o, &count, NULL);
  }
  return GCU_BASE64_OK;
}
