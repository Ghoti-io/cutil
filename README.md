# Ghoti.io CUtil

The foundation the rest of the suite is built on: an allocator, containers,
checked arithmetic, threads, and the filesystem. It is C, and the headers
are usable from C++. The test suite is the proof of that.

## Before you call it

- `NULL` for an allocator argument is the default. `gcu_file_read()` and the other allocating calls say which function frees the result.
- `gcu_file_read()` reads in chunks, so a pipe works. The buffer has a NUL one byte past `length`, which is not counted in it. A file over the limit is `GCU_FILE_ERR_LIMIT` and nothing is allocated.
- `dir.h` creates, removes and walks directories. There is no recursive delete.
- `vector.h` and `hash.h` each carry a mutex the caller locks.
- `safemath.h` leaves the result untouched on overflow.

## Examples

```c
#include <ghoti.io/cutil/file.h>
#include <stdio.h>

int main(void) {
  void * data = NULL;
  size_t length = 0;

  if (gcu_file_read("notes.txt", 1u << 20, NULL, &data, &length)
      != GCU_FILE_OK) {
    return 1;
  }
  fwrite(data, 1, length, stdout);
  gcu_file_free(NULL, data);
  return 0;
}
```

The program writes the bytes of `notes.txt`.

## Compile and link

Once the library is installed, pkg-config carries the include path and the
library:

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-cutil-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-cutil-dev` instead. See
[Symbol versions](#symbol-versions).

## Building the library

CUtil has no library dependencies beyond libc. Google Test builds the tests.

```bash
make
make test
sudo make install
```

`make test` is the suite. `make help` lists the rest.

| Target | What it does |
| --- | --- |
| `make test-asan` | Rebuild with ASan and UBSan and run the suite |
| `make test-tsan` | The concurrency tests under ThreadSanitizer |
| `make docs` | The Doxygen manual, into `./docs` |

## The API

Everything is prefixed `gcu_` / `GCU_`, under `<ghoti.io/cutil/...>`.

**Memory.** `allocator.h` is a vtable (`malloc`, `calloc`, `realloc`,
`free`) that the rest of the suite accepts under its own name, so one
allocator serves every library. `memory.h` is the default behind it, with
optional allocation tracing. `safemath.h` is overflow-checked addition and
multiplication for `size_t`, `uint32_t` and `uint64_t`.

**Containers.** `array.h` is a growable array of caller-sized elements, the
one to use for structs. `vector.h` and `hash.h` hold 8, 16, 32 and 64-bit
values in a `GCU_TypeN_Union`. `string.h` is MurmurHash3 over a byte range,
which is what the hash tables expect as a key hash.

**Threads.** `thread.h`, `mutex.h`, `rwlock.h`, `semaphore.h`, `cond.h`,
`barrier.h`, `once.h`, `tls.h` and `atomic.h` are the primitives. `pool.h`
is a fixed set of worker threads. `sequencer.h` is a reorder buffer: work
finishes in any order and comes back out in the order it was submitted. The
pool decides when work runs; the sequencer decides the order results are
seen in.

**Files.** `file.h` reads a whole file, replaces one atomically, and offers a
thin handle for files too large to hold. `dir.h` creates, removes and walks
directories. `path.h` is lexical path
manipulation that takes an explicit Windows or POSIX flavour, so the Windows
rules are testable on Linux, plus the questions that have to ask the
operating system (current directory, home, temp). `mmap.h` maps a file
instead of copying it. `filelock.h` locks a whole file between processes.
`env.h` reads and writes environment variables as UTF-8 on both platforms.

**The rest.** `utf.h` converts between UTF-8 and UTF-16. `error.h` is the
last operating-system error, without caring which operating system.
`subprocess.h` runs a program and collects its output. `library.h` loads a
shared library at run time. `random.h` is a caller-owned Mersenne Twister.
`type.h` is the sized unions and the fixed-width floats.

## Documentation

The long arguments live next to the modules they belong to:

| Page | What it settles |
| --- | --- |
| [documentation/memory.md](documentation/memory.md) | The allocator and the traced heap |
| [documentation/file.md](documentation/file.md) | Whole-file read and atomic replace |
| [documentation/path.md](documentation/path.md) | Lexical paths and the environment |
| [documentation/thread-pool.md](documentation/thread-pool.md) | The worker pool |
| [documentation/sequencer.md](documentation/sequencer.md) | The reorder buffer |
| [documentation/random.md](documentation/random.md) | The random engine and the seeded algorithms |

`make docs` builds the manual from the headers.

## Symbol versions

Every public symbol is wrapped in `GHOTIIO_CUTIL()`, which prefixes a token
taken from the Makefile's `BRANCH`. The same token names the shared library,
the `.pc` file and the install directory, so two copies of this library can
sit in one process. `make BRANCH=-dev` is a build with its own identity
throughout.

`libver.h` exposes `GCU_VERSION_MAJOR`, `_MINOR`, `_PATCH`, and
`GCU_VERSION_NUMBER` for a compile-time comparison.

## Status

The Windows build is the MinGW toolchain; it is not what the
day-to-day suite runs.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
