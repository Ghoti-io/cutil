# Fibers

**Status:** Implemented.  `include/ghoti.io/cutil/fiber.h`, `src/fiber.c`.

## 1. What it is

A fiber is a function running on a stack of its own that can give up the CPU
in the middle of a call and be resumed later exactly where it stopped.  Code
that waits for something can be written as ordinary top-to-bottom code on a
fiber: the wait is a `gcu_fiber_yield()` and whoever scheduled the fiber
resumes it when the thing it waits for has happened.

```c
GCU_Fiber * f;
gcu_fiber_create(&f, entry, arg, GCU_FIBER_DEFAULT_STACK_SIZE, NULL);
while (!gcu_fiber_is_finished(f)) {
  gcu_fiber_switch_to(f);      /* runs until it yields or returns */
}
gcu_fiber_destroy(f);
```

This is the mechanism and nothing else.  Which fiber runs next, for how long,
which tenant is owed a turn and what a fiber is waiting for are policy, and
belong to the scheduler in the framework that owns them.  Nothing here keeps a
run queue.

Resumers form a chain: a fiber may itself resume another, and each yield goes
back one step.  That is the whole shape.  There is no symmetric
"transfer to any fiber" call, because a chain cannot lose a fiber: every
running fiber has exactly one place to go back to.

## 2. The five decisions

### 2.1 The switch is hand-written, not `swapcontext`

`makecontext` and `swapcontext` were removed from POSIX in 2008, and
`swapcontext` makes a system call on every switch to save and restore the
signal mask.  A fiber switch that costs a system call is not a cheap switch,
and nothing here wants the signal mask to travel with a fiber.

So each supported architecture has a short routine that saves exactly what the
ABI makes callee-saved, on the outgoing stack, stores the stack pointer, loads
the incoming one and restores.  The routines are `__asm__` blocks in
`src/fiber.c`; there is no separate assembly file, so the build has nothing
new to know about.

| | saved |
| --- | --- |
| x86-64 System V | `rbx rbp r12-r15`, the MXCSR control bits, the x87 control word |
| arm64 | `x19-x30` (frame pointer and link register included), `d8-d15`, `sp`, `FPCR` |

The floating-point control state is the part that is easy to leave out and
costly to leave out.  Rounding mode and exception masks live in registers that
no general-purpose save touches.  A switch that saves only the integer
registers lets a fiber that calls `fesetround(FE_UPWARD)` change the rounding
of every fiber that runs after it, and of the thread that resumed it, and the
symptom is numeric results that differ between runs depending on what else was
scheduled.

x86-64 detail that matters: the MXCSR *status* flags (bits 0-5) are not
callee-saved and are not switched.  The restore keeps the flags as they are and
takes only the control bits (6-15) from the saved word.  The exception flags
therefore accumulate on the thread, as they do across any function call.  On
arm64 FPCR holds only control bits; FPSR, the flags, is not touched.

A new fiber starts in the default environment (round to nearest, all exceptions
masked), not in whatever mode its creator was in when it was made.

Not supported: x86-64 with a CET shadow stack enforced.  A `ret` into a stack
the shadow stack has not seen faults where that is enforced.  Linux user space
does not enforce it by default.

### 2.2 Windows uses its own Fiber API

The Windows x64 ABI also makes XMM6-XMM15 non-volatile, and the thread
information block carries stack limits that a hand-written switch would have to
maintain.  `CreateFiberEx`, `ConvertThreadToFiberEx`, `SwitchToFiber` and
`DeleteFiber` already do that, so they are used.

Floating-point state is the exception.  `FIBER_FLAG_FLOAT_SWITCH` asks the
Fiber API to switch it, and was the first thing tried; under wine the
two-fiber rounding test fails *with* the flag passed, on every probe, so
the flag could not be what the test was measuring.  On x86-64 the Windows arm
therefore saves and restores the same MXCSR control bits and x87 control word
the System V switch does, around each `SwitchToFiber`, and does not pass the
flag.  The test passes that way under wine, and fails when either register is
left out (`GCU_FIBER_PLANT_NO_MXCSR`, `GCU_FIBER_PLANT_NO_X87CW` compiled into
the Windows arm).  On Windows arm64, which has no such code and was not
built, the flag is passed and nothing has measured whether it is enough.

All of this was run under wine, which is not Windows.  The ledger entry is in
`notes/suite/WINDOWS-TODO.md`.

### 2.3 Guard pages

A fiber stack is its own anonymous mapping with one page below it that is
mapped inaccessible.  An overflow touches that page and the process takes
SIGSEGV where it would otherwise write into whatever lay below: another
fiber's stack, a heap chunk, the descriptor of the next fiber.  The stack is
unmapped, guard included, when the fiber is destroyed.

The mapping is made `PROT_NONE` whole and the stack opened up above the guard,
so the guard is never a window between two calls.  The size is per fiber,
rounded up to whole pages, with a floor (`GCU_FIBER_MIN_STACK_SIZE`, 16 KiB)
that is enough for the library's own trampoline and an entry that does
nothing.  A caller that calls anything needs far more, and the size is
per call because the framework that uses this picks it by what kind of call the
fiber will carry.

The stack is not zeroed on creation or destruction.  A caller that keeps
secrets on a fiber stack zeroes what it used.

`cutil` has a file-mapping module (`mmap.h`) and these stacks do not use it:
that one maps files, and a fiber stack is anonymous memory with a guard.  What
was reused is only the way an OS error is reported: a failed mapping returns
`GCU_FIBER_ERR_OOM` and leaves the OS error in `gcu_error_last()`.

### 2.4 A fiber never changes thread

A fiber belongs to the thread that created it.  Resuming it or destroying it
from another thread returns `GCU_FIBER_ERR_THREAD` and leaves the fiber
untouched.  This is not a limitation waiting to be lifted:

- a fiber's stack holds pointers into thread-local state, and a function that
  cached the address of a thread-local variable before a yield would use the
  wrong thread's after one;
- the sanitizer bookkeeping is per thread;
- the Windows Fiber API is bound to the thread that converted itself;
- the framework this exists for pins fibers to threads on purpose, because the
  runtime contexts that fibers hold are pinned too.

Because the check is the first thing the call does, a refused call touches
nothing.  The consequence worth knowing: a fiber whose thread has exited
cannot be destroyed by anyone, and its stack and descriptor are leaked.  Finish
or destroy a fiber before its thread ends; the header says so too.

### 2.5 Destroying a suspended fiber does not unwind it

Destroying a fiber that has started and not finished releases its stack as it
stands.  Destructors do not run and anything the fiber held is still held.
Unwinding would need the library to resume the fiber into a cancellation path
and trust every fiber to cooperate, which is a much larger promise than a
stack mapping.  The header says it plainly instead.

## 3. The sanitizers

A fiber library that the sanitizer gates cannot see through is a library those
gates stop testing, so telling them about each switch is part of the module.

**AddressSanitizer.**  `__sanitizer_start_switch_fiber` before every switch and
`__sanitizer_finish_switch_fiber` after it.  Without them ASan does not know
which stack is current.  The failure that follows is subtle and specific:
frames that an exception skipped leave their redzones poisoned, ASan clears
them in `__asan_handle_no_return` using the current stack's bounds, and when
those bounds are not the fiber's it prints "ASan is ignoring requested
`__asan_handle_no_return`" and leaves the poison.  The next function to land
on that part of the stack is then reported as overflowing a variable that is
not there.  The stack's shadow is also cleared when a fiber is created and when
one is destroyed, so a mapping that reuses an address does not inherit a dead
fiber's redzones.

**ThreadSanitizer.**  `__tsan_create_fiber` per fiber,
`__tsan_switch_to_fiber` before each switch, `__tsan_destroy_fiber` on
destroy.  Each fiber is then its own context to TSan, with its own shadow call
stack.

**Valgrind.**  `VALGRIND_STACK_REGISTER` per stack, so that a move of the stack
pointer into the mapping is not read as a stack overflow.  The header is
included only if it exists.  This registration is **unmeasured**: test-fiber
runs clean under Valgrind, but nothing was seen to go wrong with it removed.

These annotations are on the ELF arms only.  The Windows arm annotates
nothing; it has no sanitizer gate.

The shadow clear at create and destroy is belt and braces.  Measured against
gcc 14's libasan, its `mmap` and `munmap` interceptors already clear the shadow
(a probe saw it clean after destroy with the calls removed, and removing both
changed no test), so no planted defect can show them.  They stay for runtimes
that do not clear on `munmap` and for stacks a pool reuses without unmapping.
`ANewStackStartsCleanOfADestroyedFibersRedzones` pins the property itself.

## 4. The planted defects, and what each one showed

The fiber tests are only worth running if they are known to fail when the
switch is wrong.  `GCU_FIBER_PLANT_*` macros compile one defect into
`src/fiber.c`; no ordinary build defines them.  `make check-fiber-defects`
builds the library with each, runs the fiber tests against it, and requires
them to report.  It also runs the real library first and requires it to be
quiet, so a gate that fails for another reason cannot pass as having caught a
defect.

| Planted | Caught by | What it looks like |
| --- | --- | --- |
| `NO_MXCSR` | the rounding test | `double` and `MXCSR` probes wrong on every round, `fegetround` and `long double` right |
| `NO_X87CW` | the rounding test | `fegetround` and `long double` wrong on every round, `double` and `MXCSR` right |
| `NO_FPCR` (arm64) | `suite/tools/xarch/fiber.sh` under qemu-user | rounding checks fail |
| `NO_CALLEE_SAVED` | `CalleeSavedRegistersSurviveSwitchesInBothDirections` (x86-64: r12 is not saved; arm64: x28), and the same check in `fiber-check.c` | one register of the resumer or the fiber changes across a switch; on x86-64 the plain suite then crashes later, which the gate does not need |
| `NO_ASAN` | ASan, `detect_stack_use_after_return=0` | `stack-buffer-underflow` in a frame the exception had skipped |
| `NO_ASAN`, default options | ASan prints a warning, exit 0 | see below |
| `NO_TSAN` | a test that asks which TSan context is running | every fiber is the host thread's context; a deep-park test also crashes TSan |

The ASan defect has two arms for a reason.  In the default mode the compiler
puts each frame's locals on a heap-backed fake stack, so the real fiber stack
is never poisoned and the unannotated switch produces only the warning above.
The gate requires the warning there and the real report with
use-after-return detection off, and `make test-asan` runs the fiber tests in
both modes.

## 5. Known gaps

- The sanitizers do not run under qemu-user, so arm64 has a functional gate
  (`suite/tools/xarch/fiber.sh`) and no sanitizer gate.
- On Windows only wine has run this.  Windows x64 also makes XMM6-XMM15
non-volatile; the arm relies on `SwitchToFiber` to keep them and **no test
measures that**.
- Under the sanitizers one test is skipped, not passed: the neighbouring-stack
  check needs two stacks mapped next to each other, and the sanitizer runtimes
  allocate their own per-fiber state between them.  The test that reads the
  guard page out of `/proc/self/maps` does not depend on layout and runs in
  every build.
- macOS and the architectures with no switch (s390x, powerpc, sparc, i686)
  build, link, and return `GCU_FIBER_ERR_UNSUPPORTED`; the tests report an
  explicit skip there.
