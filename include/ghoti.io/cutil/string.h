/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2023-2026 Corey Pennycuff
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
 * A collection of string-related functions.
 */

#ifndef GHOTI_IO_GCU_STRING_H
#define GHOTI_IO_GCU_STRING_H

#include <ghoti.io/cutil/macros.h>

#include <stddef.h>
#include <ghoti.io/cutil/type.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @section murmur3_contract What these hashes do and do not promise
 *
 * **Alignment is not a precondition.** `key` and `out` may sit at any
 * address.  The blocks are read and written a byte at a time into a local,
 * which the compiler folds back into a single load or store, so an odd
 * address costs nothing and is not undefined.  Hashing a substring, an offset
 * into a buffer, or a field inside a packed struct is fair use.
 *
 * **The result does not depend on the host's byte order.** Blocks are read
 * and `out` is written little-endian on every platform, so the same key gives
 * the same bytes and the same integer on a big-endian machine as on a little-
 * endian one, and every platform reproduces the verification values SMHasher
 * publishes.  Appleby's reference does not promise this -- it reads and
 * writes in host order, so its own output is little-endian only by virtue of
 * where it is usually run.  Verified on s390x, powerpc64, powerpc and sparc64
 * as well as x86_64, i686 and aarch64; see suite/tools/xarch in the workspace.
 *
 * The consequence for `out`: it holds a defined little-endian byte string,
 * not a host integer.  Read it with `gcu_string_hash_32()` /
 * `gcu_string_hash_64()`, or decode the bytes yourself.  Casting it to
 * `uint32_t *` and dereferencing gives the host's reading of those bytes,
 * which is the value you want on a little-endian machine and a byte-swapped
 * one elsewhere.
 *
 * **`gcu_string_hash_64()` is not stable across word sizes.**  It is
 * `x64_128` on a 64-bit platform and `x86_128` on a 32-bit one.  Those are
 * different algorithms, so its value differs between a 32- and a 64-bit build
 * of the same source.  **Do not persist or transmit one.**
 *
 * The split earns itself, which is measured rather than assumed -- wall
 * clock, five interleaved paired trials pinned to one core, 1-2% spread:
 *
 *                       x86_128    x64_128
 *       32-bit, short   581.8 ms   724.1 ms   x86_128 wins by 24%
 *       32-bit, bulk     93.5 ms   235.3 ms   x86_128 wins by 152%
 *       64-bit, short   395.1 ms   336.6 ms   x64_128 wins by 15%
 *       64-bit, bulk     87.9 ms    77.5 ms   x64_128 wins by 12%
 *
 * Each arm picks the variant that is faster for its word size, and on 32-bit
 * bulk input the wrong choice costs two and a half times.
 *
 * `gcu_string_hash_32()`, by contrast, **is** stable everywhere: there is only
 * one 32-bit variant, and since blocks are read little-endian it returns the
 * same value on all seven targets in the workspace's suite/tools/xarch.  So does any
 * direct call to the three `gcu_string_murmur3_*` functions.  A caller who
 * needs a hash that survives leaving the process already has one; what they do
 * not have is a 64-bit helper that promises it.
 */

/**
 * Helper function to wrap the hash function that produces a 32-bit number
 * representing the hash.
 *
 * @param str A pointer to the string (or data block).
 * @param len The length of the data in bytes.
 * @return A 32-bit number representing the value.
 */
GCU_API uint32_t gcu_string_hash_32(char const * str, size_t len);

/**
 * Helper function to wrap the hash function that produces a 64-bit number
 * representing the hash.
 *
 * @param str A pointer to the string (or data block).
 * @param len The length of the data in bytes.
 * @return A 64-bit number representing the value.
 */
GCU_API uint64_t gcu_string_hash_64(char const * str, size_t len);

/**
 * Get 32-bit hash using the MurmurHash3 by Appleby.
 *
 * MurmurHash3 hashing algorithm, was created and put into the public domain by
 * Austin Appleby, originally in C++.
 * https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
 *
 * @param key A pointer to the start of the source data.
 * @param len The size of the data in bytes.
 * @param seed A seed value for the initial hash.
 * @param out A pointer to a 32-bit (4-byte) buffer into which the hash may be
 *   written.  The caller must supply the buffer.
 */
GCU_API void gcu_string_murmur3_32(const void * key, size_t len, uint32_t seed, void * out);

/**
 * Get 128-bit hash using the MurmurHash3 for x86 architecture by Appleby.
 *
 * The x86 version does not produce the same hash as the x64 version, by design
 * by Appleby.
 *
 * MurmurHash3 hashing algorithm, was created and put into the public domain by
 * Austin Appleby, originally in C++.
 * https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
 *
 * @param key A pointer to the start of the source data.
 * @param len The size of the data in bytes.
 * @param seed A seed value for the initial hash.
 * @param out A pointer to a 128-bit (16-byte) buffer into which the hash may
 *   be written.  The caller must supply the buffer.
 */
GCU_API void gcu_string_murmur3_x86_128(const void * key, size_t len, uint32_t seed, void * out);

/**
 * Get 128-bit hash using the MurmurHash3 for x64 architecture by Appleby.
 *
 * The x86 version does not produce the same hash as the x64 version, by design
 * by Appleby.
 *
 * MurmurHash3 hashing algorithm, was created and put into the public domain by
 * Austin Appleby, originally in C++.
 * https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
 *
 * @param key A pointer to the start of the source data.
 * @param len The size of the data in bytes.
 * @param seed A seed value for the initial hash.
 * @param out A pointer to a 128-bit (16-byte) buffer into which the hash may
 *   be written.  The caller must supply the buffer.
 */
GCU_API void gcu_string_murmur3_x64_128(const void * key, size_t len, uint32_t seed, void * out);

#ifdef __cplusplus
}
#endif

#endif //GHOTI_IO_GCU_STRING_H

