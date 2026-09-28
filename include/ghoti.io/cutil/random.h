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
 *
 * One handle for every generator. Seeded algorithms are constructed here.
 * Another library supplies its own by filling in a GCU_Random_Engine and
 * passing it to gcu_random_from_engine(); there is no registry. The draws
 * are written once, on top of the engine's fill function. The long form is
 * documentation/random.md.
 */

#ifndef GHOTI_IO_GCU_RANDOM_H
#define GHOTI_IO_GCU_RANDOM_H

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <stdint.h>
#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/float.h>

/**
 * The number of elements in the state array for the 32-bit Mersenne Twister.
 */
#define GCU_RANDOM_MT_STATE_SIZE32 624

/**
 * The number of elements in the state array for the 64-bit Mersenne Twister.
 */
#define GCU_RANDOM_MT_STATE_SIZE64 312


/**
 * The state structure for the 32-bit Mersenne Twister.
 */
typedef struct GCU_Random_MT32_State {
  /**
   * The state array for the Mersenne Twister.
   */
  uint32_t state_array[GCU_RANDOM_MT_STATE_SIZE32];
  /**
   * The index into the state array, which is used to determine the next value
   * to be generated.
   *
   * This value is always in the range [0, n-1].
   */
  size_t state_index;
} GCU_Random_MT32_State;

/**
 * Initialize the 32-bit Mersenne Twister state with the given seed.
 *
 * @param state A pointer to the state structure to be initialized.
 * @param seed A seed value with which to initialize the state.
 */
GCU_API void gcu_random_mt32_init(GCU_Random_MT32_State * state, uint32_t seed);

/**
 * Generate the next random number from the 32-bit Mersenne Twister state.
 *
 * @param state A pointer to the state structure from which to generate the next
 * random number.
 * @return The next random number in the sequence.
 */
GCU_API uint32_t gcu_random_mt32_next(GCU_Random_MT32_State * state);

/**
 * The state structure for the 64-bit Mersenne Twister.
 */
typedef struct GCU_Random_MT64_State {
  /**
   * The state array for the Mersenne Twister.
   */
  uint64_t state_array[GCU_RANDOM_MT_STATE_SIZE64];
  /**
   * The index into the state array, which is used to determine the next value
   * to be generated.
   *
   * This value is always in the range [0, n-1].
   */
  size_t state_index;
} GCU_Random_MT64_State;

/**
 * Initialize the 64-bit Mersenne Twister state with the given seed.
 *
 * @param state A pointer to the state structure to be initialized.
 * @param seed A seed value with which to initialize the state.
 */
GCU_API void gcu_random_mt64_init(GCU_Random_MT64_State * state, uint64_t seed);

/**
 * Generate the next random number from the 64-bit Mersenne Twister state.
 *
 * @param state A pointer to the state structure from which to generate the next
 * random number.
 * @return The next random number in the sequence.
 */
GCU_API uint64_t gcu_random_mt64_next(GCU_Random_MT64_State * state);

/**
 * A generator handle. Draws go through this, whichever algorithm created it.
 *
 * Two handles never share state. Drawing from one handle on two threads at
 * once is not safe; the caller provides the lock if it wants to share one.
 * There is no process-wide or thread-wide generator.
 */
typedef struct GCU_Random GCU_Random;

/**
 * The plug a library implements when the generator does not live in CUtil.
 *
 * Same shape as GCU_Allocator: a context, and the functions that use it.
 * CUtil copies these three fields into the handle. The function pointers
 * must outlive the handle. Construction happens before this struct exists;
 * there is no create field.
 *
 * On success, fill writes exactly n bytes and returns 0. On failure it wipes
 * out and returns nonzero. A seeded engine does not fail. Bytes are
 * successive native words, little-endian. A request that ends in the middle
 * of a word leaves the spare bytes in the engine's own state, not in the
 * handle. destroy may be NULL when ctx needs nothing. After a successful
 * gcu_random_from_engine() the handle owns ctx and destroy is how it is
 * released. If that call returns NULL, the caller still owns ctx.
 */
typedef struct GCU_Random_Engine {
  /**
   * Algorithm state. NULL is a legal engine with no state.
   */
  void * ctx;
  /**
   * Write n bytes to out. n == 0 is success and writes nothing.
   */
  int (* fill)(void * ctx, void * out, size_t n);
  /**
   * Release ctx. NULL if there is nothing to free.
   */
  void (* destroy)(void * ctx);
} GCU_Random_Engine;

/**
 * Copy engine into a new handle.
 *
 * @param engine The engine to copy. The fill pointer is required.
 * @return The handle, or NULL if engine or its fill is NULL, or allocation
 * failed. The caller still owns engine->ctx when this returns NULL.
 */
GCU_API GCU_Random * gcu_random_from_engine(const GCU_Random_Engine * engine);

/**
 * Release a handle and, when the engine has one, call its destroy.
 *
 * @param r The handle. NULL does nothing.
 */
GCU_API void gcu_random_free(GCU_Random * r);

/**
 * Write n bytes from the generator.
 *
 * @param r The handle.
 * @param out The destination. May be NULL only when n is 0.
 * @param n The number of bytes.
 * @return 0 on success. On failure out is wiped. A NULL handle, or a NULL
 * out with n > 0, returns nonzero and does not write.
 */
GCU_API int gcu_random_bytes(GCU_Random * r, void * out, size_t n);

/**
 * The next four bytes, as a little-endian integer.
 *
 * On a 32-bit generator this is the next native word.
 *
 * @param r The handle.
 * @param out Receives the value. Wiped to 0 if the engine fails.
 * @return 0 on success, nonzero on a NULL argument or an engine failure.
 */
GCU_API int gcu_random_u32(GCU_Random * r, uint32_t * out);

/**
 * The next eight bytes, as a little-endian integer.
 *
 * On a 32-bit generator this is two native words, low word first. That is
 * not Java's nextLong(), which is (next(32) << 32) + next(32).
 *
 * @param r The handle.
 * @param out Receives the value. Wiped to 0 if the engine fails.
 * @return 0 on success, nonzero on a NULL argument or an engine failure.
 */
GCU_API int gcu_random_u64(GCU_Random * r, uint64_t * out);

/**
 * A double in [0, 1).
 *
 * The top 53 bits of a gcu_random_u64() result, divided by 2^53. This is
 * not Python's random.random() and not Java's nextDouble().
 *
 * @param r The handle.
 * @param out Receives the value. Set to 0 if the engine fails.
 * @return 0 on success, nonzero on a NULL argument or an engine failure.
 */
GCU_API int gcu_random_f64(GCU_Random * r, double * out);

/**
 * An integer in [0, bound), unbiased.
 *
 * Uses Lemire's nearly-divisionless map. bound == 0 is an error and leaves
 * out untouched, as does a NULL argument. An engine failure wipes out to 0.
 *
 * @param r The handle.
 * @param bound The exclusive upper bound. Must be nonzero.
 * @param out Receives the value.
 * @return 0 on success, nonzero on a bad argument or an engine failure.
 */
GCU_API int gcu_random_below(GCU_Random * r, uint64_t bound, uint64_t * out);

/**
 * A handle over MT19937, the std::mt19937 sequence.
 *
 * The native word is 32 bits. gcu_random_u32() on this handle matches
 * gcu_random_mt32_next() on a state initialised with the same seed.
 *
 * @param seed The standard 32-bit Mersenne Twister seed.
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_mt32(uint32_t seed);

/**
 * A handle over MT19937-64, the std::mt19937_64 sequence.
 *
 * @param seed The standard 64-bit Mersenne Twister seed.
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_mt64(uint64_t seed);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GHOTI_IO_GCU_RANDOM_H
