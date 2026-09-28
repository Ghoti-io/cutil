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
 * The 624-word state of MT19937, the C++ std::mt19937 engine.
 *
 * state_index is the next word to twist, always in [0, 623].
 */
typedef struct GCU_Random_MT32_State {
  /**
   * The 624 state words.
   */
  uint32_t state_array[GCU_RANDOM_MT_STATE_SIZE32];
  /**
   * The next word to twist. Always in [0, 623].
   */
  size_t state_index;
} GCU_Random_MT32_State;

/**
 * Seed an MT19937 state the way std::mt19937::seed does.
 *
 * state[0] = seed. For i from 1 to 623,
 * state[i] = 1812433253 * (state[i - 1] xor (state[i - 1] >> 30)) + i.
 * The next call of gcu_random_mt32_next() is then std::mt19937::operator().
 *
 * @param state The state to initialise.
 * @param seed The single word std::mt19937::seed accepts. This is not
 * Python's random.Random seed and not NumPy's MT19937 SeedSequence.
 */
GCU_API void gcu_random_mt32_init(GCU_Random_MT32_State * state, uint32_t seed);

/**
 * The next tempered MT19937 word.
 *
 * The twist and the temper are the std::mt19937 parameters: n = 624,
 * m = 397, r = 31, a = 0x9908B0DF, and (u, d, s, b, t, c, l) =
 * (11, 0xFFFFFFFF, 7, 0x9D2C5680, 15, 0xEFC60000, 18).
 *
 * @param state The state to advance.
 * @return The tempered 32-bit word.
 */
GCU_API uint32_t gcu_random_mt32_next(GCU_Random_MT32_State * state);

/**
 * The 312-word state of MT19937-64, the C++ std::mt19937_64 engine.
 *
 * state_index is the next word to twist, always in [0, 311].
 */
typedef struct GCU_Random_MT64_State {
  /**
   * The 312 state words.
   */
  uint64_t state_array[GCU_RANDOM_MT_STATE_SIZE64];
  /**
   * The next word to twist. Always in [0, 311].
   */
  size_t state_index;
} GCU_Random_MT64_State;

/**
 * Seed an MT19937-64 state the way std::mt19937_64::seed does.
 *
 * state[0] = seed. For i from 1 to 311,
 * state[i] = 6364136223846793005 * (state[i - 1] xor (state[i - 1] >> 62)) + i.
 * The next call of gcu_random_mt64_next() is then std::mt19937_64::operator().
 *
 * @param state The state to initialise.
 * @param seed The single word std::mt19937_64::seed accepts. This is not
 * Python's random.Random seed and not NumPy's MT19937 SeedSequence.
 */
GCU_API void gcu_random_mt64_init(GCU_Random_MT64_State * state, uint64_t seed);

/**
 * The next tempered MT19937-64 word.
 *
 * The twist and the temper are the std::mt19937_64 parameters: n = 312,
 * m = 156, r = 31, a = 0xB5026F5AA96619E9, and (u, d, s, b, t, c, l) =
 * (29, 0x5555555555555555, 17, 0x71D67FFFEDA60000, 37, 0xFFF7EEE000000000, 43).
 *
 * @param state The state to advance.
 * @return The tempered 64-bit word.
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
 * How many bytes gcu_random_place() needs.
 *
 * The bytes must be aligned for a pointer.
 */
GCU_API size_t gcu_random_handle_size(void);

/**
 * Build a handle in storage the caller owns.
 *
 * Use this for a generator that lives for the process and must not come from
 * the heap. gcu_random_free() still runs destroy, and it does not free the
 * storage. storage_size must be at least gcu_random_handle_size(), and
 * storage must be aligned for a pointer.
 *
 * @param storage Caller-owned bytes.
 * @param storage_size The number of bytes at storage.
 * @param engine The engine to copy. The fill pointer is required.
 * @return The handle, or NULL if the arguments cannot hold one. The caller
 * still owns engine->ctx when this returns NULL.
 */
GCU_API GCU_Random * gcu_random_place(void * storage, size_t storage_size, const GCU_Random_Engine * engine);

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
 * A handle over MT19937 (Matsumoto and Nishimura, 1998).
 *
 * The sequence is C++ std::mt19937: the parameters and the seed named on
 * gcu_random_mt32_init() and gcu_random_mt32_next(). The native word is 32
 * bits, so gcu_random_u32() matches gcu_random_mt32_next() for the same seed.
 *
 * @param seed Passed to gcu_random_mt32_init().
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_mt32(uint32_t seed);

/**
 * A handle over MT19937-64 (Matsumoto and Nishimura).
 *
 * The sequence is C++ std::mt19937_64: the parameters and the seed named on
 * gcu_random_mt64_init() and gcu_random_mt64_next().
 *
 * @param seed Passed to gcu_random_mt64_init().
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_mt64(uint64_t seed);

/**
 * java.util.Random, the 48-bit linear congruential generator.
 *
 * The seed is mixed the way setSeed does:
 * (seed ^ 0x5DEECE66D) & ((1 << 48) - 1). The native word is 32 bits, the
 * top of the 48-bit state, which is nextInt(). gcu_random_u64() on this
 * handle is two of those words, low word first, which is not nextLong().
 *
 * @param seed The same seed java.util.Random's constructor accepts.
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_java(uint64_t seed);

/**
 * SplitMix64, the generator behind java.util.SplittableRandom.nextLong.
 *
 * The seed is stored as the state. The first draw adds the golden-ratio
 * constant, then mixes. It is also the function that expands one word into
 * the four words of gcu_random_xoshiro256pp().
 *
 * @param seed The starting state, stored as itself.
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_splitmix64(uint64_t seed);

/**
 * xoshiro256++ (Blackman and Vigna).
 *
 * The recommended generator when a new stream has no sequence it must match.
 * The seed is one word, expanded to four state words with SplitMix64. The
 * all-zero state is not a valid xoshiro state; this expansion does not
 * produce it. This is not xoshiro256**, which is what .NET's unseeded
 * Random uses.
 *
 * @param seed Expanded with SplitMix64 into the four state words.
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_xoshiro256pp(uint64_t seed);

/**
 * PCG64 XSL-RR 128/64 (O'Neill), the output function of NumPy's PCG64.
 *
 * Seeded as the reference srandom(seed, 0): one stream, the increment fixed
 * from a sequence of 0. This is not NumPy's SeedSequence and not
 * numpy.random.default_rng.
 *
 * @param seed The 64-bit initstate. The stream selector is 0.
 * @return The handle, or NULL if allocation failed.
 */
GCU_API GCU_Random * gcu_random_pcg64(uint64_t seed);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GHOTI_IO_GCU_RANDOM_H
