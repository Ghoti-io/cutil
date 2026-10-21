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
 * Base64 (RFC 4648, sections 4 and 5), encoding and strict decoding.
 *
 * ## What is stated, not left to chance
 *
 * Two decoders can disagree about one string: one accepts missing padding,
 * another rejects it; one skips a newline, another stops at it; one ignores the
 * spare bits of the last character, another insists they are zero. A library
 * that picks silently makes its callers' data portable only to decoders that
 * happen to agree. So every one of those choices is a field of
 * ::GCU_Base64_Decode_Options, and the zero-filled options are the strictest
 * reading, RFC 4648 section 3.5's canonical form:
 *
 *   - the standard alphabet (`+` and `/`);
 *   - padding required, `=` only as the last one or two characters;
 *   - no whitespace;
 *   - the unused low bits of the last character are zero.
 *
 * A `data:` URI (RFC 2397) or MIME wants ::GCU_BASE64_WHITESPACE_SKIP_ASCII and,
 * usually, ::GCU_BASE64_PADDING_OPTIONAL, and says so at the call.
 *
 * ## No allocation, no state
 *
 * Both directions work in a buffer the caller owns and hold no state, so they
 * are reentrant, usable inside a parser's inner loop, and never fail for lack
 * of memory. Sizes are computed with overflow checks; no input length wraps.
 * Decoding writes nothing unless the whole input is valid and fits: a caller
 * cannot mistake a partial result for a whole one. Decoding may be done in
 * place (`out == in`); encoding may not (the buffers must not overlap).
 */

#ifndef GHOTI_IO_GCU_BASE64_H
#define GHOTI_IO_GCU_BASE64_H

#include <ghoti.io/cutil/macros.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The 64 characters. The two alphabets differ in the last two. */
typedef enum GCU_Base64_Alphabet {
  GCU_BASE64_ALPHABET_STANDARD = 0, ///< `A-Za-z0-9+/` (RFC 4648 section 4).
  GCU_BASE64_ALPHABET_URL,          ///< `A-Za-z0-9-_` (RFC 4648 section 5).
  GCU_BASE64_ALPHABET_COUNT         ///< Closes the enum.
} GCU_Base64_Alphabet;

/** Padding written by the encoder. */
typedef enum GCU_Base64_Encode_Padding {
  GCU_BASE64_ENCODE_PADDING_ALWAYS = 0, ///< `=` completes the last group of four.
  GCU_BASE64_ENCODE_PADDING_NONE,       ///< No `=`.
  GCU_BASE64_ENCODE_PADDING_COUNT       ///< Closes the enum.
} GCU_Base64_Encode_Padding;

/** Padding the decoder accepts. */
typedef enum GCU_Base64_Padding {
  GCU_BASE64_PADDING_REQUIRED = 0, ///< The input must end with the padding its length needs.
  GCU_BASE64_PADDING_OPTIONAL,     ///< Padding is accepted when complete, and may be left out.
  GCU_BASE64_PADDING_FORBIDDEN,    ///< `=` is an error.
  GCU_BASE64_PADDING_COUNT         ///< Closes the enum.
} GCU_Base64_Padding;

/** Whitespace the decoder skips. */
typedef enum GCU_Base64_Whitespace {
  GCU_BASE64_WHITESPACE_REJECT = 0, ///< Any whitespace is an error.
  /** Space, tab, CR, LF and FF are skipped between characters, but never inside an unfinished padding. */
  GCU_BASE64_WHITESPACE_SKIP_ASCII,
  GCU_BASE64_WHITESPACE_COUNT       ///< Closes the enum.
} GCU_Base64_Whitespace;

/** The spare bits of a last partial group. */
typedef enum GCU_Base64_Trailing_Bits {
  GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO = 0, ///< Non-zero spare bits are an error (the canonical form).
  GCU_BASE64_TRAILING_BITS_IGNORE,           ///< The spare bits are discarded.
  GCU_BASE64_TRAILING_BITS_COUNT             ///< Closes the enum.
} GCU_Base64_Trailing_Bits;

/** What the decoder accepts. Zero-filled is the strictest reading. */
typedef struct GCU_Base64_Decode_Options {
  GCU_Base64_Alphabet alphabet;       ///< Which alphabet; the other's two characters are errors.
  GCU_Base64_Padding padding;         ///< What to do with `=`.
  GCU_Base64_Whitespace whitespace;   ///< What to do with whitespace.
  GCU_Base64_Trailing_Bits trailing_bits; ///< What to do with the spare bits of the last character.
} GCU_Base64_Decode_Options;

/** The result of an encode or a decode. */
typedef enum GCU_Base64_Status {
  GCU_BASE64_OK = 0,
  GCU_BASE64_ERR_CHARACTER,     ///< A byte outside the alphabet (or whitespace where it is not skipped).
  GCU_BASE64_ERR_PADDING,       ///< `=` where it is not allowed, missing, incomplete or followed by data.
  GCU_BASE64_ERR_LENGTH,        ///< A final group of one character, which carries no whole byte.
  GCU_BASE64_ERR_TRAILING_BITS, ///< Non-zero spare bits under ::GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO.
  GCU_BASE64_ERR_CAPACITY,      ///< The output buffer is too small; the size needed is reported.
  GCU_BASE64_ERR_OVERFLOW,      ///< The size of the result does not fit in `size_t`.
  GCU_BASE64_ERR_INVALID,       ///< A NULL argument that must not be, or an option out of range.
  GCU_BASE64_STATUS_COUNT       ///< Closes the enum.
} GCU_Base64_Status;

/** A short name for a status, or "unknown". */
GCU_API const char * gcu_base64_status_string(GCU_Base64_Status status);

/**
 * The number of characters the encoding of @p n bytes takes.
 *
 * @return false (and @p out untouched) if the size does not fit in `size_t`.
 */
GCU_API bool gcu_base64_encoded_size(size_t n, GCU_Base64_Encode_Padding padding, size_t * out);

/** An upper bound on the bytes @p n characters can decode to; never overflows. */
GCU_API size_t gcu_base64_decoded_size_max(size_t n);

/**
 * Encode @p n bytes into @p out, which has room for @p cap characters.
 *
 * Writes exactly the size ::gcu_base64_encoded_size gives and no terminator.
 * Nothing is written when @p cap is short.
 *
 * @param written Receives the characters written, or the characters needed
 *   when the result is ::GCU_BASE64_ERR_CAPACITY; may be NULL.
 * @return ::GCU_BASE64_OK, ::GCU_BASE64_ERR_CAPACITY, ::GCU_BASE64_ERR_OVERFLOW
 *   or ::GCU_BASE64_ERR_INVALID (NULL `out` with a non-zero `cap`, NULL `in`
 *   with a non-zero `n`, or an alphabet or padding out of range).
 */
GCU_API GCU_Base64_Status gcu_base64_encode(const void * in, size_t n, char * out, size_t cap,
    GCU_Base64_Alphabet alphabet, GCU_Base64_Encode_Padding padding, size_t * written);

/**
 * Decode @p n characters into @p out, which has room for @p cap bytes.
 *
 * The whole input is checked first; nothing is written unless it is valid and
 * fits. Pass `out == NULL` to measure: @p written receives the size.
 *
 * @param options NULL for the zero-filled (strictest) options.
 * @param written Receives the bytes decoded, or the bytes needed on
 *   ::GCU_BASE64_ERR_CAPACITY; may be NULL.
 * @param error_offset Receives the offset of the first byte refused (or `n`
 *   for an error at the end of the input); may be NULL.
 * @return ::GCU_BASE64_OK or the first error found.
 */
GCU_API GCU_Base64_Status gcu_base64_decode(const char * in, size_t n, void * out, size_t cap,
    const GCU_Base64_Decode_Options * options, size_t * written, size_t * error_offset);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_BASE64_H
