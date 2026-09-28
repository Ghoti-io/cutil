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


#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/cutil/random.h>
#include <stdint.h>
#include <string.h>

// For reference, see the Mersenne Twister pseudocode from Wikipedia:
// https://en.wikipedia.org/wiki/Mersenne_Twister


void gcu_random_mt32_init(GCU_Random_MT32_State * state, uint32_t seed) {
  uint32_t* state_array = (uint32_t*)&state->state_array;
  state_array[0] = seed;
  for (int i = 1; i < GCU_RANDOM_MT_STATE_SIZE32; ++i) {
    uint32_t x = state_array[i - 1];
    state_array[i] = ((x ^ (x >> 30)) * 1812433253UL) + i; 
  }
  state->state_index = 0;
}


uint32_t gcu_random_mt32_next(GCU_Random_MT32_State * state) {
  // 32-bit Mersenne Twister by Matsumoto and Nishimura, 1998
  // Values taken from https://en.cppreference.com/w/cpp/numeric/random/mersenne_twister_engine
  const uint32_t upper_mask = (~(uint32_t)0) << 31; // 0x80000000
  const uint32_t lower_mask = ~upper_mask;          // 0x7FFFFFFF
  const uint32_t state_size = GCU_RANDOM_MT_STATE_SIZE32; // n
  const uint32_t middle_word = 397;          // m
  const uint32_t multiplier = 0x9908B0DFUL;  // a
  const uint32_t tempering_u = 11;           // u
  const uint32_t tempering_d = 0xFFFFFFFF;   // d
  const uint32_t tempering_s = 7;            // s
  const uint32_t tempering_b = 0x9D2C5680;   // b
  const uint32_t tempering_t = 15;           // t
  const uint32_t tempering_c = 0xEFC60000;   // c
  const uint32_t tempering_l = 18;           // l

  uint32_t* state_array = (uint32_t*)&state->state_array;
  size_t current_index = state->state_index;

  // Compute the index which is one element behind the current index, wrapped.
  size_t next_index = (current_index == state_size - 1)
    ? 0
    : current_index + 1;

  // Compute the index which is 397 elements behind the current index, wrapped.
  uint32_t middle_index = ((current_index + middle_word) < state_size)
    ? current_index + middle_word
    : current_index + middle_word - state_size;

  // Compute the new state value.
  uint32_t x = (state_array[current_index] & upper_mask) | (state_array[next_index] & lower_mask);
  x = state_array[middle_index] ^ (x >> 1) ^ (x & 1 ? multiplier : 0);
  state_array[current_index] = x;

  // Increment the state index (wrap-around included).
  state->state_index = next_index;

  // Temper the value.
  x ^= (x >> tempering_u) & tempering_d;
  x ^= (x << tempering_s) & tempering_b;
  x ^= (x << tempering_t) & tempering_c;
  x ^= x >> tempering_l;
  return x;
}


void gcu_random_mt64_init(GCU_Random_MT64_State * state, uint64_t seed) {
  uint64_t* state_array = (uint64_t*)&state->state_array;
  state_array[0] = seed;
  for (int i = 1; i < GCU_RANDOM_MT_STATE_SIZE64; ++i) {
    uint64_t x = state_array[i - 1];
    state_array[i] = ((x ^ (x >> 62)) * 6364136223846793005ULL) + i; 
  }
  state->state_index = 0;
}


uint64_t gcu_random_mt64_next(GCU_Random_MT64_State * state) {
  // 64-bit Mersenne Twister by Matsumoto and Nishimura, 2000
  // Values taken from https://en.cppreference.com/w/cpp/numeric/random/mersenne_twister_engine
  const uint64_t upper_mask = (~(uint64_t)0) << 31; // 0xFFFFFFFF80000000
  const uint64_t lower_mask = ~upper_mask;          // 0x000000007FFFFFFF
  const uint64_t state_size = GCU_RANDOM_MT_STATE_SIZE64; // n
  const uint64_t middle_word = 156;                       // m
  const uint64_t multiplier = 0xB5026F5AA96619E9ULL;  // a
  const uint64_t tempering_u = 29;                    // u
  const uint64_t tempering_d = 0x5555555555555555ULL; // d
  const uint64_t tempering_s = 17;                    // s
  const uint64_t tempering_b = 0x71D67FFFEDA60000ULL; // b
  const uint64_t tempering_t = 37;                    // t
  const uint64_t tempering_c = 0xFFF7EEE000000000ULL; // c
  const uint64_t tempering_l = 43;                    // l

  uint64_t* state_array = (uint64_t*)&state->state_array;
  size_t current_index = state->state_index;

  // Compute the index which is one element behind the current index, wrapped.
  size_t next_index = (current_index == state_size - 1)
    ? 0
    : current_index + 1;

  // Compute the index which is 156 elements behind the current index, wrapped.
  uint64_t middle_index = ((current_index + middle_word) < state_size)
    ? current_index + middle_word
    : current_index + middle_word - state_size;

  // Compute the new state value.
  uint64_t x = (state_array[current_index] & upper_mask) | (state_array[next_index] & lower_mask);
  x = state_array[middle_index] ^ (x >> 1) ^ (x & 1 ? multiplier : 0);
  state_array[current_index] = x;

  // Increment the state index (wrap-around included).
  state->state_index = next_index;

  // Temper the value.
  x ^= (x >> tempering_u) & tempering_d;
  x ^= (x << tempering_s) & tempering_b;
  x ^= (x << tempering_t) & tempering_c;
  x ^= x >> tempering_l;
  return x;
}


struct GCU_Random {
  GCU_Random_Engine engine;
  int owned;
};


struct BytePump {
  unsigned char pending[8];
  unsigned off;
  unsigned len;
  unsigned word_bytes;
  int (* produce)(void * inner, unsigned char * le);
  void * inner;
};


static void wipe(void * p, size_t n) {
  if (p != NULL && n != 0) {
    memset(p, 0, n);
  }
}


static void free_ctx(void * ctx) {
  gcu_free(ctx);
}


static void mul_wide(uint64_t a, uint64_t b, uint64_t * lo, uint64_t * hi) {
#if defined(__SIZEOF_INT128__)
  __uint128_t m = (__uint128_t)a * b;
  *lo = (uint64_t)m;
  *hi = (uint64_t)(m >> 64);
#else
  uint64_t a0 = (uint32_t)a;
  uint64_t a1 = a >> 32;
  uint64_t b0 = (uint32_t)b;
  uint64_t b1 = b >> 32;
  uint64_t p0 = a0 * b0;
  uint64_t p1 = a0 * b1;
  uint64_t p2 = a1 * b0;
  uint64_t p3 = a1 * b1;
  uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
  *lo = (p0 & 0xffffffffu) | (mid << 32);
  *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
#endif
}


static int byte_pump_fill(void * ctx, void * out, size_t n) {
  struct BytePump * pump = ctx;
  unsigned char * dst = out;
  size_t left = n;

  if (n == 0) {
    return 0;
  }
  while (left != 0) {
    size_t have;
    size_t take;
    if (pump->off == pump->len) {
      if (pump->produce(pump->inner, pump->pending) != 0) {
        wipe(out, n);
        return -1;
      }
      pump->off = 0;
      pump->len = pump->word_bytes;
    }
    have = (size_t)(pump->len - pump->off);
    take = left < have ? left : have;
    memcpy(dst, pump->pending + pump->off, take);
    pump->off += (unsigned)take;
    dst += take;
    left -= take;
  }
  return 0;
}


static void store_u32_le(uint32_t w, unsigned char * le) {
  le[0] = (unsigned char)w;
  le[1] = (unsigned char)(w >> 8);
  le[2] = (unsigned char)(w >> 16);
  le[3] = (unsigned char)(w >> 24);
}


static void store_u64_le(uint64_t w, unsigned char * le) {
  le[0] = (unsigned char)w;
  le[1] = (unsigned char)(w >> 8);
  le[2] = (unsigned char)(w >> 16);
  le[3] = (unsigned char)(w >> 24);
  le[4] = (unsigned char)(w >> 32);
  le[5] = (unsigned char)(w >> 40);
  le[6] = (unsigned char)(w >> 48);
  le[7] = (unsigned char)(w >> 56);
}


static uint32_t load_u32_le(const unsigned char * le) {
  return (uint32_t)le[0]
    | ((uint32_t)le[1] << 8)
    | ((uint32_t)le[2] << 16)
    | ((uint32_t)le[3] << 24);
}


static uint64_t load_u64_le(const unsigned char * le) {
  return (uint64_t)le[0]
    | ((uint64_t)le[1] << 8)
    | ((uint64_t)le[2] << 16)
    | ((uint64_t)le[3] << 24)
    | ((uint64_t)le[4] << 32)
    | ((uint64_t)le[5] << 40)
    | ((uint64_t)le[6] << 48)
    | ((uint64_t)le[7] << 56);
}


GCU_Random * gcu_random_from_engine(const GCU_Random_Engine * engine) {
  GCU_Random * r;
  if (engine == NULL || engine->fill == NULL) {
    return NULL;
  }
  r = gcu_malloc(sizeof *r);
  if (r == NULL) {
    return NULL;
  }
  r->engine = *engine;
  r->owned = 1;
  return r;
}


size_t gcu_random_handle_size(void) {
  return sizeof(struct GCU_Random);
}


GCU_Random * gcu_random_place(void * storage, size_t storage_size, const GCU_Random_Engine * engine) {
  GCU_Random * r;
  if (storage == NULL || engine == NULL || engine->fill == NULL) {
    return NULL;
  }
  if (storage_size < sizeof *r) {
    return NULL;
  }
  if (((uintptr_t)storage % _Alignof(GCU_Random)) != 0) {
    return NULL;
  }
  r = storage;
  r->engine = *engine;
  r->owned = 0;
  return r;
}


void gcu_random_free(GCU_Random * r) {
  if (r == NULL) {
    return;
  }
  if (r->engine.destroy != NULL) {
    r->engine.destroy(r->engine.ctx);
  }
  if (r->owned) {
    gcu_free(r);
  }
}


int gcu_random_bytes(GCU_Random * r, void * out, size_t n) {
  if (r == NULL || r->engine.fill == NULL || (out == NULL && n != 0)) {
    return -1;
  }
  if (n == 0) {
    return 0;
  }
  if (r->engine.fill(r->engine.ctx, out, n) != 0) {
    wipe(out, n);
    return -1;
  }
  return 0;
}


int gcu_random_u32(GCU_Random * r, uint32_t * out) {
  unsigned char b[4];
  if (out == NULL) {
    return -1;
  }
  if (gcu_random_bytes(r, b, sizeof b) != 0) {
    *out = 0;
    return -1;
  }
  *out = load_u32_le(b);
  return 0;
}


int gcu_random_u64(GCU_Random * r, uint64_t * out) {
  unsigned char b[8];
  if (out == NULL) {
    return -1;
  }
  if (gcu_random_bytes(r, b, sizeof b) != 0) {
    *out = 0;
    return -1;
  }
  *out = load_u64_le(b);
  return 0;
}


int gcu_random_f64(GCU_Random * r, double * out) {
  uint64_t u;
  if (out == NULL) {
    return -1;
  }
  if (gcu_random_u64(r, &u) != 0) {
    *out = 0.0;
    return -1;
  }
  *out = (double)(u >> 11) * (1.0 / 9007199254740992.0);
  return 0;
}


int gcu_random_below(GCU_Random * r, uint64_t bound, uint64_t * out) {
  uint64_t x;
  uint64_t lo;
  uint64_t hi;
  uint64_t threshold;
  if (r == NULL || out == NULL || bound == 0) {
    return -1;
  }
  if (gcu_random_u64(r, &x) != 0) {
    *out = 0;
    return -1;
  }
  mul_wide(x, bound, &lo, &hi);
  if (lo < bound) {
    threshold = (uint64_t)(0 - bound) % bound;
    while (lo < threshold) {
      if (gcu_random_u64(r, &x) != 0) {
        *out = 0;
        return -1;
      }
      mul_wide(x, bound, &lo, &hi);
    }
  }
  *out = hi;
  return 0;
}


typedef struct {
  struct BytePump pump;
  GCU_Random_MT32_State mt;
} Mt32Box;


typedef struct {
  struct BytePump pump;
  GCU_Random_MT64_State mt;
} Mt64Box;


static int mt32_produce(void * inner, unsigned char * le) {
  store_u32_le(gcu_random_mt32_next(inner), le);
  return 0;
}


static int mt64_produce(void * inner, unsigned char * le) {
  store_u64_le(gcu_random_mt64_next(inner), le);
  return 0;
}


GCU_Random * gcu_random_mt32(uint32_t seed) {
  Mt32Box * box;
  GCU_Random_Engine engine;
  GCU_Random * r;
  box = gcu_malloc(sizeof *box);
  if (box == NULL) {
    return NULL;
  }
  memset(box, 0, sizeof *box);
  gcu_random_mt32_init(&box->mt, seed);
  box->pump.word_bytes = 4;
  box->pump.produce = mt32_produce;
  box->pump.inner = &box->mt;
  engine.ctx = box;
  engine.fill = byte_pump_fill;
  engine.destroy = free_ctx;
  r = gcu_random_from_engine(&engine);
  if (r == NULL) {
    gcu_free(box);
  }
  return r;
}


GCU_Random * gcu_random_mt64(uint64_t seed) {
  Mt64Box * box;
  GCU_Random_Engine engine;
  GCU_Random * r;
  box = gcu_malloc(sizeof *box);
  if (box == NULL) {
    return NULL;
  }
  memset(box, 0, sizeof *box);
  gcu_random_mt64_init(&box->mt, seed);
  box->pump.word_bytes = 8;
  box->pump.produce = mt64_produce;
  box->pump.inner = &box->mt;
  engine.ctx = box;
  engine.fill = byte_pump_fill;
  engine.destroy = free_ctx;
  r = gcu_random_from_engine(&engine);
  if (r == NULL) {
    gcu_free(box);
  }
  return r;
}


static uint64_t rotl64(uint64_t x, unsigned k) {
  return (x << k) | (x >> (64u - k));
}


static uint64_t mix64(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}


typedef struct {
  uint64_t lo;
  uint64_t hi;
} U128;


static U128 u128_add(U128 a, U128 b) {
  U128 r;
  r.lo = a.lo + b.lo;
  r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u);
  return r;
}


static U128 u128_mul(U128 a, U128 b) {
  uint64_t lo;
  uint64_t hi;
  U128 r;
  mul_wide(a.lo, b.lo, &lo, &hi);
  r.lo = lo;
  r.hi = hi;
  mul_wide(a.lo, b.hi, &lo, &hi);
  r.hi += lo;
  mul_wide(a.hi, b.lo, &lo, &hi);
  r.hi += lo;
  return r;
}


static GCU_Random * adopt_pump(void * box, struct BytePump * pump, unsigned word_bytes, int (* produce)(void * inner, unsigned char * le), void * inner) {
  GCU_Random_Engine engine;
  GCU_Random * r;
  pump->off = 0;
  pump->len = 0;
  pump->word_bytes = word_bytes;
  pump->produce = produce;
  pump->inner = inner;
  engine.ctx = box;
  engine.fill = byte_pump_fill;
  engine.destroy = free_ctx;
  r = gcu_random_from_engine(&engine);
  if (r == NULL) {
    gcu_free(box);
  }
  return r;
}


typedef struct {
  struct BytePump pump;
  uint64_t seed;
} JavaBox;


static int java_produce(void * inner, unsigned char * le) {
  uint64_t * seed = inner;
  *seed = (*seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
  store_u32_le((uint32_t)(*seed >> 16), le);
  return 0;
}


GCU_Random * gcu_random_java(uint64_t seed) {
  JavaBox * box = gcu_malloc(sizeof *box);
  if (box == NULL) {
    return NULL;
  }
  memset(box, 0, sizeof *box);
  box->seed = (seed ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
  return adopt_pump(box, &box->pump, 4, java_produce, &box->seed);
}


typedef struct {
  struct BytePump pump;
  uint64_t state;
} SplitBox;


static int splitmix_produce(void * inner, unsigned char * le) {
  uint64_t * state = inner;
  *state += 0x9E3779B97F4A7C15ULL;
  store_u64_le(mix64(*state), le);
  return 0;
}


GCU_Random * gcu_random_splitmix64(uint64_t seed) {
  SplitBox * box = gcu_malloc(sizeof *box);
  if (box == NULL) {
    return NULL;
  }
  memset(box, 0, sizeof *box);
  box->state = seed;
  return adopt_pump(box, &box->pump, 8, splitmix_produce, &box->state);
}


typedef struct {
  struct BytePump pump;
  uint64_t s[4];
} XoshiroBox;


static int xoshiro_produce(void * inner, unsigned char * le) {
  uint64_t * s = inner;
  uint64_t result = rotl64(s[0] + s[3], 23) + s[0];
  uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = rotl64(s[3], 45);
  store_u64_le(result, le);
  return 0;
}


GCU_Random * gcu_random_xoshiro256pp(uint64_t seed) {
  XoshiroBox * box = gcu_malloc(sizeof *box);
  unsigned i;
  uint64_t sm;
  if (box == NULL) {
    return NULL;
  }
  memset(box, 0, sizeof *box);
  sm = seed;
  for (i = 0; i < 4; ++i) {
    sm += 0x9E3779B97F4A7C15ULL;
    box->s[i] = mix64(sm);
  }
  return adopt_pump(box, &box->pump, 8, xoshiro_produce, box->s);
}


typedef struct {
  struct BytePump pump;
  U128 state;
  U128 inc;
} PcgBox;


static const U128 PCG_MULT = { 4865540595714422341ULL, 2549297995355413924ULL };


static uint64_t pcg_output(U128 state) {
  uint64_t mixed = state.hi ^ state.lo;
  unsigned rot = (unsigned)(state.hi >> 58);
  if (rot == 0) {
    return mixed;
  }
  return (mixed >> rot) | (mixed << (64u - rot));
}


static uint64_t pcg_step(PcgBox * box) {
  U128 old = box->state;
  box->state = u128_add(u128_mul(old, PCG_MULT), box->inc);
  return pcg_output(old);
}


static int pcg_produce(void * inner, unsigned char * le) {
  store_u64_le(pcg_step(inner), le);
  return 0;
}


GCU_Random * gcu_random_pcg64(uint64_t seed) {
  PcgBox * box = gcu_malloc(sizeof *box);
  U128 initstate;
  if (box == NULL) {
    return NULL;
  }
  memset(box, 0, sizeof *box);
  box->inc.lo = 1;
  box->inc.hi = 0;
  (void)pcg_step(box);
  initstate.lo = seed;
  initstate.hi = 0;
  box->state = u128_add(box->state, initstate);
  (void)pcg_step(box);
  return adopt_pump(box, &box->pump, 8, pcg_produce, box);
}
