# Random

## 1. What it is

One handle, `GCU_Random`, and one way to draw from it. The algorithm is
chosen when the handle is created. After that, an integer, a real in
`[0, 1)`, or an integer below a bound is the same call whichever algorithm
is behind it.

CUtil ships the seeded algorithms. It does not ship a kernel generator, and
it does not grow one: a generator whose output is supposed to be
unpredictable belongs in the library that owns the rest of the secret
material. That library plugs in by handing CUtil an engine. There is no
registry and no algorithm-id enum for a later library to extend.

## 2. The engine

An engine is three fields, the same shape as `GCU_Allocator`: a context
pointer and the functions that use it.

```c
typedef struct GCU_Random_Engine {
  void * ctx;
  int (* fill)(void * ctx, void * out, size_t n);
  void (* destroy)(void * ctx);
} GCU_Random_Engine;
```

`fill` is the only operation. It writes exactly `n` bytes to `out` and
returns 0, or it fails. On failure it wipes `out` before returning. A seeded
engine returns 0. A kernel engine can fail, which is why every draw returns
a status instead of a bare value.

`destroy` releases `ctx`. It may be `NULL` when there is nothing to free.
`gcu_random_free()` is the call that runs it, and it is always how a handle
is released, including when `destroy` is `NULL`. `gcu_random_free(NULL)`
does nothing.

There is no `create` field. Construction happens in the function that knows
the algorithm, before CUtil sees the struct. That function allocates
whatever state it wants, fills in the engine, and returns the handle from
`gcu_random_from_engine()`. CUtil copies the three fields into the handle.
A generator that lives for the whole process, and must not come from the
heap, uses `gcu_random_place()` on storage the caller owns;
`gcu_random_handle_size()` is how much, aligned for a pointer.
`gcu_random_free()` still runs `destroy`, and it does not free that storage.
The function pointers must outlive the handle; they are static functions,
not something allocated beside the state. The caller does not free `ctx`
itself after a successful `gcu_random_from_engine()`: the handle owns it,
and `destroy` is how it goes away. If `gcu_random_from_engine()` returns
`NULL` (the engine or its `fill` was `NULL`, or allocation failed), the
caller still owns `ctx`.

`fill` may be asked for any positive `n`, including a count that is not a
multiple of the algorithm's native word. Bytes are successive native words
stored little-endian, so the first four bytes from a 32-bit generator are
that generator's next word on any host. A request that stops in the middle
of a word leaves the unused bytes in that engine's own state. That leftover
is algorithm output, not a cache of kernel entropy, and CUtil does not keep
a second copy of it in the handle. Two handles never share state. Nothing
in the process or in a thread is a generator.

The handle is not safe to draw from on two threads at once. The caller that
wants that shares it under its own lock. Two threads holding two handles do
not interfere, even when both handles were built by the same constructor.

## 3. Draws

These are written once, on top of `fill`. They are not fields of the engine,
and an algorithm does not reimplement them.

| Call | Result |
| --- | --- |
| `gcu_random_bytes(r, out, n)` | `n` bytes. `n == 0` succeeds. |
| `gcu_random_u32(r, out)` | The next four bytes, little-endian. |
| `gcu_random_u64(r, out)` | The next eight bytes, little-endian. On a 32-bit generator this is two native words, low word first. |
| `gcu_random_f64(r, out)` | A `double` in `[0, 1)`. The top 53 bits of a `u64`, divided by 2^53. |
| `gcu_random_below(r, bound, out)` | An integer in `[0, bound)`. `bound == 0` is an error. The method is Lemire's nearly-divisionless map, so the result is unbiased. |

Each returns 0 or a negative value. A negative value from `fill` wipes the
output the draw was about to publish. An invalid argument (`r` or `out`
`NULL`, or a zero bound) leaves the output untouched.

`gcu_random_f64()` is not Python's `random.random()` and not Java's
`nextDouble()`. Those are a later, named function if a caller needs the
language's own assembly of bits. `gcu_random_u64()` on a 32-bit generator is
not Java's `nextLong()`, which is `(next(32) << 32) + next(32)`.

## 4. What CUtil constructs

Each seeded algorithm has its own constructor. It takes the seed that
algorithm defines and returns a `GCU_Random *`, or `NULL` if allocation
failed. The raw Mersenne Twister state and its `init` / `next` pair stay
available for a test that wants the native word with nothing around it.
They are the same recurrence the constructors use. A program that only
wants draws uses the handle.

| Constructor | Algorithm | Seed |
| --- | --- | --- |
| `gcu_random_mt32(uint32_t seed)` | MT19937 (Matsumoto and Nishimura, 1998), the C++ `std::mt19937` word | `state[0] = seed`. For `i` from 1 to 623, `state[i] = 1812433253 * (state[i - 1] xor (state[i - 1] >> 30)) + i`. That is `mersenne_twister_engine::seed`. |
| `gcu_random_mt64(uint64_t seed)` | MT19937-64 (Matsumoto and Nishimura), the C++ `std::mt19937_64` word | `state[0] = seed`. For `i` from 1 to 311, `state[i] = 6364136223846793005 * (state[i - 1] xor (state[i - 1] >> 62)) + i`. |
| `gcu_random_java(uint64_t seed)` | `java.util.Random`, the 48-bit linear congruential generator | Same as `setSeed`: `(seed ^ 0x5DEECE66D) & ((1 << 48) - 1)`. The native word is 32 bits, the top of the 48-bit state, which is `nextInt()`. |
| `gcu_random_splitmix64(uint64_t seed)` | SplitMix64, `java.util.SplittableRandom.nextLong` | The seed is stored as the state. The first draw adds the golden-ratio constant, then mixes. |
| `gcu_random_xoshiro256pp(uint64_t seed)` | xoshiro256++ (Blackman and Vigna) | One `uint64_t`, expanded to four state words with SplitMix64. The recommended default when a new stream has no sequence it must match. The all-zero state is not a valid xoshiro state; this seed function does not produce it. |
| `gcu_random_pcg64(uint64_t seed)` | PCG64 XSL-RR 128/64 (O'Neill), NumPy's `PCG64` output function | `srandom(seed, 0)` from the reference implementation: one stream, increment fixed from a sequence of 0. This is not NumPy's `SeedSequence` and not `numpy.random.default_rng`. |

Those two rows are the C++11 typedefs. `std::mt19937` has word size 32,
n = 624, m = 397, r = 31, a = `0x9908B0DF`, temper
(u, d, s, b, t, c, l) = (11, `0xFFFFFFFF`, 7, `0x9D2C5680`, 15,
`0xEFC60000`, 18), and initialisation multiplier f = 1812433253.
`std::mt19937_64` has word size 64, n = 312, m = 156, r = 31,
a = `0xB5026F5AA96619E9`, temper (29, `0x5555555555555555`, 17,
`0x71D67FFFEDA60000`, 37, `0xFFF7EEE000000000`, 43), and
f = 6364136223846793005. The native word is `operator()` after that seed.

A caller matching a language reads the row, not the shared `f64`. In
particular, .NET's seeded `Random(int)` is still the old Knuth generator,
and its unseeded `Random` is xoshiro256** rather than xoshiro256++. Go's
`math/rand/v2` PCG is not this PCG64. Python's `random.Random` and NumPy's
`MT19937` (the legacy `RandomState` and `Generator`) use the MT19937
recurrence and a different seed; the raw words here match C++ `std::mt19937`,
not those seedings. Python's float is its own assembly of bits, which
`gcu_random_f64()` is not.

## 5. A generator that lives in another library

Security ships the kernel constructor. It does not link into CUtil, and
CUtil does not call it.

```c
static int kernel_fill(void * ctx, void * out, size_t n) {
  (void)ctx;
  return gsec_random_bytes(out, n, NULL) == GSEC_OK ? 0 : -1;
}

GCU_Random * gsec_random_open(void) {
  GCU_Random_Engine engine = { NULL, kernel_fill, NULL };
  return gcu_random_from_engine(&engine);
}
```

`gsec_random_bytes()` already wipes its buffer when the kernel call fails,
which is the failure rule above. Bulk key material stays on
`gsec_random_bytes()` directly: that function takes the library's size
limit, and a draw through the handle is not a substitute for it. The handle
is how the rest of a program asks for the next integer or the next real
without learning which generator it was given.

`kernel_fill` must not keep unused kernel bytes for a later call. A `fork`
would duplicate them into the child.

## 6. What judges the sequences

`make test` does not run a container. The words in the unit test are a
record of a sequence, and a record can be copied from the implementation
that is supposed to be under test.

`make check-oracle` asks four references. `make oracle-build` builds the
images. Each image is built here, so the finished image has no digest to
pin. The base is Debian 13 slim at the same digest the rest of the suite
pins, and the package versions are pinned in the Containerfile.

| Generator | Judge | Image |
| --- | --- | --- |
| `gcu_random_java` | `java.util.Random.nextInt` | `localhost/ghoti-cutil-oracle-java:21.0.12`. The JDK is `openjdk-21-jdk-headless=21.0.12.1+1-1~deb13u1`. `java-version` must report `openjdk 21.0.12`. |
| `gcu_random_splitmix64` | `java.util.SplittableRandom.nextLong` | The same OpenJDK image. |
| `gcu_random_xoshiro256pp` | `rand::rngs::Xoshiro256PlusPlus::seed_from_u64`, then `next_u64` | `localhost/ghoti-cutil-oracle-prng:rand-0.10.3`. `rustc` is `1.85.1+dfsg1-1+deb13u1` and `rand` is `0.10.3`. `xoshiro-version` must report `rustc 1.85.1 rand 0.10.3`. |
| `gcu_random_pcg64` | `pcg64_srandom_r(rng, seed, 0)` then `pcg64_random_r` | The same image. pcg-c is commit `83252d9c23df9c82ecb42210afed61a7b42402d7`. `pcg-version` must report that commit. |
| `gcu_random_mt32` | `std::mt19937` | The same image. `g++` is `4:14.2.0-1` and `libstdc++-14-dev` is `14.2.0-19`. `mt-version` must report `g++ 14.2.0 libstdc++ 14.2.0-19`. The comparison is 1248 words, two trips through the 624-word state. |
| `gcu_random_mt64` | `std::mt19937_64` | The same image and the same word count, which is four trips through the 312-word state. |

`Random.nextLong` is not `gcu_random_u64` on the Java handle, and the
oracle does not compare those two.
