/**
 * @file
 *
 * Tests for fibers.
 *
 * What each group is for, since several of these only fail against one
 * specific wrong implementation:
 *
 *   - Rounding isolation fails if the switch skips any part of the
 *     floating-point control state.  It reads the mode three ways (fegetround,
 *     double arithmetic, and on x86-64 long double arithmetic) because the
 *     three read different registers: fegetround() reads the x87 control word
 *     on x86-64, double arithmetic is SSE and reads MXCSR, long double is x87.
 *     A switch that saves only one of them passes the other two.
 *   - The overflow tests fork, because the expected outcome is a fault.  One
 *     lets the default SIGSEGV action kill the child; the other catches it on
 *     an alternate stack and checks that the fiber stack mapped just below was
 *     not touched, which is what a missing guard page would break.
 *   - The wrong-thread tests run the calls on a second thread and then prove
 *     the fiber still works, because "refused" and "refused and corrupted"
 *     look the same until the fiber is used again.
 *   - Under AddressSanitizer and ThreadSanitizer every test here is also a
 *     test of the switch annotations: a suspended fiber destroyed, stacks
 *     reused at one address, locals read across a yield.  `make
 *     check-fiber-defects` removes the annotations and requires those runs to
 *     report.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gtest/gtest.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/error.h>
#include <ghoti.io/cutil/fiber.h>

#include "hang-guard.h"

#include <atomic>
#include <cfenv>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

// Which sanitizer, if any, built this test; GCC and clang spell it differently.
#if defined(__SANITIZE_ADDRESS__)
#define GCU_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GCU_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define GCU_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GCU_TEST_TSAN 1
#endif
#endif
#ifdef GCU_TEST_ASAN
#include <sanitizer/asan_interface.h>
#endif
#ifdef GCU_TEST_TSAN
#include <sanitizer/tsan_interface.h>
#endif

using namespace std;

namespace {

// Explicit, never a pass: where there are no fibers the tests say so and the
// run records a skip.
#define REQUIRE_FIBERS() \
  do { \
    if (!GCU_FIBER_SUPPORTED) { \
      GTEST_SKIP() << "fibers are not supported on this platform"; \
    } \
  } while (0)

#ifdef _WIN32
#define REQUIRE_FORK() GTEST_SKIP() << "needs fork(); POSIX only"
#else
#define REQUIRE_FORK() \
  do { \
  } while (0)
#endif

class Fiber : public ghoti_test::HangGuarded<120> {};

//
// Floating-point probes.
//

enum Mode { kNearest = 0, kUp = 1, kDown = 2, kZero = 3, kInconsistent = -1 };

int feMode(int fe) {
  if (fe == FE_TONEAREST) return kNearest;
  if (fe == FE_UPWARD) return kUp;
  if (fe == FE_DOWNWARD) return kDown;
  if (fe == FE_TOWARDZERO) return kZero;
  return kInconsistent;
}

int feValue(int mode) {
  switch (mode) {
    case kUp: return FE_UPWARD;
    case kDown: return FE_DOWNWARD;
    case kZero: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
  }
}

/**
 * The rounding mode the arithmetic of one type actually uses, found by
 * adding numbers whose correct result depends on it.
 *
 * `tiny` is far below one half unit in the last place; `three_quarters` is
 * three quarters of one.  Round-to-nearest drops the first and rounds the
 * second up, upward rounding raises both, downward rounding raises neither
 * but moves a negative sum away from zero, and toward zero moves neither.
 * Volatile, so the additions happen at run time under the mode in force
 * rather than in the compiler's.
 */
template <class T>
int arithmeticMode(T tiny, T three_quarters) {
  volatile T one = 1;
  volatile T minus_one = -1;
  volatile T t = tiny;
  volatile T q = three_quarters;
  volatile T x = one + t;
  volatile T y = minus_one - t;
  volatile T z = one + q;
  bool up_x = x > 1;
  bool down_y = y < -1;
  bool up_z = z > 1;
  if (!up_x && !down_y && up_z) return kNearest;
  if (up_x && !down_y && up_z) return kUp;
  if (!up_x && down_y && !up_z) return kDown;
  if (!up_x && !down_y && !up_z) return kZero;
  return kInconsistent;
}

// noinline: the probe must run where it is called, not be hoisted across a
// switch the optimiser cannot see through.
__attribute__((noinline)) int doubleMode() {
  return arithmeticMode<double>(0x1p-60, 0x1.8p-53);
}

#if defined(__x86_64__)
__attribute__((noinline)) int longDoubleMode() {
  return arithmeticMode<long double>(0x1p-70L, 0x1.8p-64L);
}
__attribute__((noinline)) int mxcsrMode() {
  switch (_MM_GET_ROUNDING_MODE()) {
    case _MM_ROUND_NEAREST: return kNearest;
    case _MM_ROUND_UP: return kUp;
    case _MM_ROUND_DOWN: return kDown;
    case _MM_ROUND_TOWARD_ZERO: return kZero;
  }
  return kInconsistent;
}
#endif

/**
 * How often each reading of the rounding mode disagreed.  Kept apart because
 * they read different state: when a switch drops one register, only the
 * readings of that register go wrong, and the counts say which.
 */
struct ProbeTally {
  int fegetroundWrong = 0;   ///< x87 control word on x86-64, FPCR on arm64.
  int doubleWrong = 0;       ///< SSE arithmetic on x86-64: MXCSR.
  int longDoubleWrong = 0;   ///< x87 arithmetic (x86-64 only).
  int mxcsrWrong = 0;        ///< MXCSR read directly (x86-64 only).

  int total() const {
    return fegetroundWrong + doubleWrong + longDoubleWrong + mxcsrWrong;
  }
  string describe() const {
    return "fegetround " + to_string(fegetroundWrong) + ", double " +
      to_string(doubleWrong) + ", long double " +
      to_string(longDoubleWrong) + ", MXCSR " + to_string(mxcsrWrong);
  }
};

/**
 * Check every available reading of the rounding mode against @p expected,
 * counting each that disagrees into @p tally.
 */
void probeAll(int expected, ProbeTally * tally) {
  if (feMode(fegetround()) != expected) tally->fegetroundWrong++;
  if (doubleMode() != expected) tally->doubleWrong++;
#if defined(__x86_64__)
  if (longDoubleMode() != expected) tally->longDoubleWrong++;
  if (mxcsrMode() != expected) tally->mxcsrWrong++;
#endif
}

/**
 * Whether every available reading of the rounding mode says @p expected.
 */
bool everyProbeSays(int expected) {
  ProbeTally tally;
  probeAll(expected, &tally);
  return tally.total() == 0;
}

/// Restores round-to-nearest however the test leaves, so one failure does not
/// poison the rest of the process.
struct RoundingGuard {
  ~RoundingGuard() { fesetround(FE_TONEAREST); }
};

//
// The tests' fiber bodies.
//

struct Roundtrip {
  int step = 0;
  const char * where_a = nullptr;
  uintptr_t local_leg1 = 0;
  uintptr_t local_leg2 = 0;
  GCU_Fiber * seen_current = nullptr;
};

void roundtripEntry(void * arg) {
  Roundtrip * r = static_cast<Roundtrip *>(arg);
  volatile char local[64];
  local[0] = 1;
  r->seen_current = gcu_fiber_current();
  r->local_leg1 = reinterpret_cast<uintptr_t>(&local[0]);
  r->step = 1;
  gcu_fiber_yield();
  local[0] = 2;
  r->local_leg2 = reinterpret_cast<uintptr_t>(&local[0]);
  r->step = 2;
  gcu_fiber_yield();
  r->step = 3;
}

TEST_F(Fiber, RoundTripRunsEachLegOnceOnItsOwnStack) {
  REQUIRE_FIBERS();
  Roundtrip r;
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, roundtripEntry, &r,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_NE(nullptr, f);
  EXPECT_FALSE(gcu_fiber_is_finished(f));
  EXPECT_EQ(nullptr, gcu_fiber_current());
  EXPECT_EQ(0, r.step) << "creating a fiber must not run it";

  volatile char mine = 0;
  uintptr_t mainLocal = reinterpret_cast<uintptr_t>(&mine);

  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_EQ(1, r.step);
  EXPECT_FALSE(gcu_fiber_is_finished(f));
  EXPECT_EQ(f, r.seen_current);
  EXPECT_EQ(nullptr, gcu_fiber_current());

  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_EQ(2, r.step);
  EXPECT_FALSE(gcu_fiber_is_finished(f));

  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_EQ(3, r.step);
  EXPECT_TRUE(gcu_fiber_is_finished(f));

  // The same frame at both legs, and nowhere near the caller's stack.
  EXPECT_EQ(r.local_leg1, r.local_leg2);
  uintptr_t gap = r.local_leg1 > mainLocal ? r.local_leg1 - mainLocal
                                           : mainLocal - r.local_leg1;
  EXPECT_GT(gap, (uintptr_t)GCU_FIBER_DEFAULT_STACK_SIZE);

  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

struct Echo {
  void * seen = nullptr;
};
void echoEntry(void * arg) {
  Echo * e = static_cast<Echo *>(arg);
  e->seen = arg;
}

TEST_F(Fiber, EntryReceivesTheArgumentItWasCreatedWith) {
  REQUIRE_FIBERS();
  Echo e;
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, echoEntry, &e,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_EQ(&e, e.seen);
  EXPECT_TRUE(gcu_fiber_is_finished(f));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

//
// Callee-saved registers, loaded with known values by a small assembly
// routine rather than left to the compiler.  `register` and `volatile` do not
// put a value in a callee-saved register, so a check written with them can
// pass while the switch drops every one of them.  This routine puts a known
// value in each register the ABI says a call must preserve, calls through a
// function pointer, and reports what each held afterwards.
//
// x86-64: rbx r12 r13 r14 r15.  arm64: x19-x28 and d8-d15 (as bit patterns).
//

#if GCU_FIBER_SUPPORTED && !defined(_WIN32) && \
    (defined(__x86_64__) || defined(__aarch64__))
#define GCU_TEST_REGS 1
#endif

#ifdef GCU_TEST_REGS
#if defined(__x86_64__)
const int kRegCount = 5;
__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl gcu_test_regs_across\n"
  ".hidden gcu_test_regs_across\n"
  ".type gcu_test_regs_across, @function\n"
  "gcu_test_regs_across:\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n"
  "  pushq %r14\n  pushq %r15\n  pushq %rdx\n"
  "  movq %rdi, %rax\n"
  "  movq 0(%rsi), %rbx\n  movq 8(%rsi), %r12\n  movq 16(%rsi), %r13\n"
  "  movq 24(%rsi), %r14\n  movq 32(%rsi), %r15\n"
  "  call *%rax\n"
  "  movq (%rsp), %rdx\n"
  "  movq %rbx, 0(%rdx)\n  movq %r12, 8(%rdx)\n  movq %r13, 16(%rdx)\n"
  "  movq %r14, 24(%rdx)\n  movq %r15, 32(%rdx)\n"
  "  popq %rdx\n  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
  "  popq %rbx\n  popq %rbp\n  ret\n"
  ".size gcu_test_regs_across, .-gcu_test_regs_across\n"
);
#else
const int kRegCount = 18;
__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl gcu_test_regs_across\n"
  ".hidden gcu_test_regs_across\n"
  ".type gcu_test_regs_across, %function\n"
  "gcu_test_regs_across:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  stp x19, x20, [sp, #-16]!\n  stp x21, x22, [sp, #-16]!\n"
  "  stp x23, x24, [sp, #-16]!\n  stp x25, x26, [sp, #-16]!\n"
  "  stp x27, x28, [sp, #-16]!\n"
  "  stp d8, d9, [sp, #-16]!\n  stp d10, d11, [sp, #-16]!\n"
  "  stp d12, d13, [sp, #-16]!\n  stp d14, d15, [sp, #-16]!\n"
  "  str x2, [sp, #-16]!\n"
  "  mov x9, x0\n  mov x10, x1\n"
  "  ldp x19, x20, [x10, #0]\n  ldp x21, x22, [x10, #16]\n"
  "  ldp x23, x24, [x10, #32]\n  ldp x25, x26, [x10, #48]\n"
  "  ldp x27, x28, [x10, #64]\n"
  "  ldp d8, d9, [x10, #80]\n  ldp d10, d11, [x10, #96]\n"
  "  ldp d12, d13, [x10, #112]\n  ldp d14, d15, [x10, #128]\n"
  "  blr x9\n"
  "  ldr x2, [sp]\n"
  "  stp x19, x20, [x2, #0]\n  stp x21, x22, [x2, #16]\n"
  "  stp x23, x24, [x2, #32]\n  stp x25, x26, [x2, #48]\n"
  "  stp x27, x28, [x2, #64]\n"
  "  stp d8, d9, [x2, #80]\n  stp d10, d11, [x2, #96]\n"
  "  stp d12, d13, [x2, #112]\n  stp d14, d15, [x2, #128]\n"
  "  add sp, sp, #16\n"
  "  ldp d14, d15, [sp], #16\n  ldp d12, d13, [sp], #16\n"
  "  ldp d10, d11, [sp], #16\n  ldp d8, d9, [sp], #16\n"
  "  ldp x27, x28, [sp], #16\n  ldp x25, x26, [sp], #16\n"
  "  ldp x23, x24, [sp], #16\n  ldp x21, x22, [sp], #16\n"
  "  ldp x19, x20, [sp], #16\n  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  ".size gcu_test_regs_across, .-gcu_test_regs_across\n"
);
#endif

extern "C" void gcu_test_regs_across(void (*fn)(void), const uint64_t * in,
  uint64_t * out);

/// Distinct, non-zero, recognisable values for one side of the switch.
void regPattern(uint64_t seed, uint64_t * v) {
  for (int i = 0; i < kRegCount; i++) {
    v[i] = seed * 0x0101010101010101ull + (uint64_t)(i + 1) * 0x1000193ull;
  }
}

/// How many registers did not come back holding what they were given.
int regsChanged(const uint64_t * in, const uint64_t * out) {
  int bad = 0;
  for (int i = 0; i < kRegCount; i++) {
    if (in[i] != out[i]) bad++;
  }
  return bad;
}

GCU_Fiber * gRegsFiber = nullptr;
void switchThunk() { gcu_fiber_switch_to(gRegsFiber); }
void yieldThunk() { gcu_fiber_yield(); }

struct RegsJob {
  int rounds = 0;
  int changedInFiber = 0;
};

void regsEntry(void * arg) {
  RegsJob * job = static_cast<RegsJob *>(arg);
  for (int i = 0; i < job->rounds; i++) {
    uint64_t in[18], out[18];
    regPattern(0x70 + i, in);
    gcu_test_regs_across(yieldThunk, in, out);
    job->changedInFiber += regsChanged(in, out);
  }
}

TEST_F(Fiber, CalleeSavedRegistersSurviveSwitchesInBothDirections) {
  REQUIRE_FIBERS();
  RegsJob job;
  job.rounds = 200;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&gRegsFiber, regsEntry, &job,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  int changedInCaller = 0;
  for (int i = 0; i < job.rounds; i++) {
    uint64_t in[18], out[18];
    regPattern(0x20 + i, in);
    gcu_test_regs_across(switchThunk, in, out);
    changedInCaller += regsChanged(in, out);
  }
  EXPECT_EQ(0, changedInCaller)
    << "a callee-saved register of the resumer changed across a resume";
  gcu_fiber_switch_to(gRegsFiber); // let it finish
  EXPECT_EQ(0, job.changedInFiber)
    << "a callee-saved register of the fiber changed across a yield";
  EXPECT_TRUE(gcu_fiber_is_finished(gRegsFiber));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(gRegsFiber));
}
#endif // GCU_TEST_REGS

//
// Floating-point isolation.
//

struct RoundingJob {
  int mode;
  int rounds;
  ProbeTally tally;
  int ran = 0;
};

void roundingEntry(void * arg) {
  RoundingJob * job = static_cast<RoundingJob *>(arg);
  fesetround(feValue(job->mode));
  for (int i = 0; i < job->rounds; i++) {
    probeAll(job->mode, &job->tally);
    job->ran++;
    gcu_fiber_yield();
  }
  probeAll(job->mode, &job->tally);
}

TEST_F(Fiber, EachFiberKeepsItsOwnRoundingModeAcrossSwitches) {
  REQUIRE_FIBERS();
  RoundingGuard guard;
  const int kRounds = 300;
  RoundingJob a{kUp, kRounds, {}, 0};
  RoundingJob b{kDown, kRounds, {}, 0};
  GCU_Fiber * fa = nullptr;
  GCU_Fiber * fb = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fa, roundingEntry, &a,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fb, roundingEntry, &b,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));

  ASSERT_EQ(0, fesetround(FE_TOWARDZERO));
  ASSERT_TRUE(everyProbeSays(kZero)) << "the probe itself must see the mode";

  ProbeTally caller;
  for (int i = 0; i < kRounds; i++) {
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fa));
    probeAll(kZero, &caller);
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fb));
    probeAll(kZero, &caller);
  }
  // Let both run to the end.
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fa));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fb));

  EXPECT_TRUE(gcu_fiber_is_finished(fa));
  EXPECT_TRUE(gcu_fiber_is_finished(fb));
  EXPECT_EQ(0, a.tally.total())
    << "the upward fiber saw another mode: " << a.tally.describe();
  EXPECT_EQ(0, b.tally.total())
    << "the downward fiber saw another mode: " << b.tally.describe();
  EXPECT_EQ(kRounds, a.ran);
  EXPECT_EQ(kRounds, b.ran);
  EXPECT_EQ(0, caller.total())
    << "a fiber's mode leaked into the thread that resumed it: "
    << caller.describe();
  EXPECT_TRUE(everyProbeSays(kZero)) << "the thread's mode after both finish";

  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fa));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fb));
}

struct StartMode {
  bool everyProbeNearest = false;
};
void startModeEntry(void * arg) {
  static_cast<StartMode *>(arg)->everyProbeNearest = everyProbeSays(kNearest);
}

TEST_F(Fiber, ANewFiberStartsInTheDefaultEnvironmentNotItsCreators) {
#if defined(_WIN32) && !defined(__x86_64__)
  GTEST_SKIP() << "Windows on arm64 relies on the Fiber API, which does not "
                  "specify a new fiber's mode";
#endif
  REQUIRE_FIBERS();
  RoundingGuard guard;
  ASSERT_EQ(0, fesetround(FE_UPWARD));
  StartMode s;
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, startModeEntry, &s,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_TRUE(s.everyProbeNearest);
  EXPECT_TRUE(everyProbeSays(kUp)) << "the creator's mode after the fiber ran";
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

//
// Nesting and the states a call is refused in.
//

struct Nest {
  vector<string> log;
  GCU_Fiber * a = nullptr;
  GCU_Fiber * b = nullptr;
  GCU_Fiber_Result b_resumes_a = GCU_FIBER_OK;
  GCU_Fiber_Result b_resumes_b = GCU_FIBER_OK;
  GCU_Fiber_Result b_destroys_a = GCU_FIBER_OK;
  GCU_Fiber_Result b_destroys_b = GCU_FIBER_OK;
  GCU_Fiber * current_in_b = nullptr;
  GCU_Fiber * current_in_a_after = nullptr;
};

void nestB(void * arg) {
  Nest * n = static_cast<Nest *>(arg);
  n->log.push_back("b1");
  n->current_in_b = gcu_fiber_current();
  n->b_resumes_a = gcu_fiber_switch_to(n->a);
  n->b_resumes_b = gcu_fiber_switch_to(n->b);
  n->b_destroys_a = gcu_fiber_destroy(n->a);
  n->b_destroys_b = gcu_fiber_destroy(n->b);
  gcu_fiber_yield();
  n->log.push_back("b2");
}

void nestA(void * arg) {
  Nest * n = static_cast<Nest *>(arg);
  n->log.push_back("a1");
  gcu_fiber_switch_to(n->b);
  n->current_in_a_after = gcu_fiber_current();
  n->log.push_back("a2");
  gcu_fiber_yield();
  n->log.push_back("a3");
  gcu_fiber_switch_to(n->b);
  n->log.push_back("a4");
}

TEST_F(Fiber, AFiberMayResumeAnotherAndAYieldGoesOneStepBack) {
  REQUIRE_FIBERS();
  Nest n;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&n.a, nestA, &n,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&n.b, nestB, &n,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));

  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(n.a));
  // b yielded to a, not to the thread; a then yielded to the thread.
  EXPECT_EQ((vector<string>{"a1", "b1", "a2"}), n.log);
  EXPECT_EQ(n.b, n.current_in_b);
  EXPECT_EQ(n.a, n.current_in_a_after);
  EXPECT_EQ(nullptr, gcu_fiber_current());

  // While b was running a was on the chain, and b was running itself.
  EXPECT_EQ(GCU_FIBER_ERR_STATE, n.b_resumes_a);
  EXPECT_EQ(GCU_FIBER_ERR_STATE, n.b_resumes_b);
  EXPECT_EQ(GCU_FIBER_ERR_STATE, n.b_destroys_a);
  EXPECT_EQ(GCU_FIBER_ERR_STATE, n.b_destroys_b);

  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(n.a));
  EXPECT_EQ((vector<string>{"a1", "b1", "a2", "a3", "b2", "a4"}), n.log);
  EXPECT_TRUE(gcu_fiber_is_finished(n.a));
  EXPECT_TRUE(gcu_fiber_is_finished(n.b));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(n.a));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(n.b));
}

void nothingEntry(void *) {}

TEST_F(Fiber, CallsAreRefusedInTheStatesTheHeaderSaysTheyAre) {
  REQUIRE_FIBERS();
  EXPECT_EQ(GCU_FIBER_ERR_STATE, gcu_fiber_yield())
    << "yield from a thread that is not on a fiber";
  EXPECT_EQ(GCU_FIBER_ERR_INVALID, gcu_fiber_switch_to(nullptr));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(nullptr));
  EXPECT_TRUE(gcu_fiber_is_finished(nullptr));

  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, nothingEntry, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  ASSERT_TRUE(gcu_fiber_is_finished(f));
  EXPECT_EQ(GCU_FIBER_ERR_STATE, gcu_fiber_switch_to(f))
    << "a finished fiber cannot be resumed";
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

TEST_F(Fiber, CreateRefusesWhatItCannotUse) {
  REQUIRE_FIBERS();
  GCU_Fiber * f = reinterpret_cast<GCU_Fiber *>(0x1);
  EXPECT_EQ(GCU_FIBER_ERR_INVALID, gcu_fiber_create(nullptr, nothingEntry,
    nullptr, GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  EXPECT_EQ(GCU_FIBER_ERR_INVALID, gcu_fiber_create(&f, nullptr, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  EXPECT_EQ(GCU_FIBER_ERR_INVALID, gcu_fiber_create(&f, nothingEntry, nullptr,
    GCU_FIBER_MIN_STACK_SIZE - 1, nullptr));
  EXPECT_EQ(reinterpret_cast<GCU_Fiber *>(0x1), f)
    << "the output is written only on success";

  // The documented minimum is enough for an entry that does nothing.
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, nothingEntry, nullptr,
    GCU_FIBER_MIN_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_TRUE(gcu_fiber_is_finished(f));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

TEST_F(Fiber, EveryResultHasADistinctName) {
  vector<string> names;
  for (int i = 0; i < GCU_FIBER_RESULT_COUNT; i++) {
    const char * s = gcu_fiber_result_string(static_cast<GCU_Fiber_Result>(i));
    ASSERT_NE(nullptr, s);
    EXPECT_GT(strlen(s), 0u);
    for (const string & seen : names) {
      EXPECT_NE(seen, s);
    }
    names.push_back(s);
  }
  EXPECT_NE(nullptr, gcu_fiber_result_string(GCU_FIBER_RESULT_COUNT));
  EXPECT_NE(nullptr, gcu_fiber_result_string(static_cast<GCU_Fiber_Result>(-1)));
}

#if defined(__ELF__) && (defined(__x86_64__) || defined(__aarch64__)) || \
    defined(_WIN32)
TEST_F(Fiber, SupportedWhereASwitchExists) {
  // Guards the macro itself: if it silently became 0 every other test here
  // would skip and the run would look clean.
  EXPECT_EQ(1, GCU_FIBER_SUPPORTED);
}
#else
TEST_F(Fiber, UnsupportedBuildsLinkAndRefuse) {
  EXPECT_EQ(0, GCU_FIBER_SUPPORTED);
  GCU_Fiber * f = nullptr;
  EXPECT_EQ(GCU_FIBER_ERR_UNSUPPORTED, gcu_fiber_create(&f, nothingEntry,
    nullptr, GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  EXPECT_EQ(GCU_FIBER_ERR_UNSUPPORTED, gcu_fiber_yield());
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(nullptr))
    << "destroy(NULL) is ignored, as in the real implementations";
  EXPECT_EQ(nullptr, gcu_fiber_current());
}
#endif

//
// Many fibers, and the stack really being the size asked for.
//

struct Counter {
  int id;
  long sum = 0;
  int steps;
};
void counterEntry(void * arg) {
  Counter * c = static_cast<Counter *>(arg);
  volatile long mine = c->id * 1000;
  for (int i = 0; i < c->steps; i++) {
    mine = mine + i;
    c->sum = mine;
    gcu_fiber_yield();
  }
}

TEST_F(Fiber, ManyFibersInterleavedKeepTheirOwnState) {
  REQUIRE_FIBERS();
  const int kFibers = 64;
  const int kSteps = 20;
  vector<Counter> counters(kFibers);
  vector<GCU_Fiber *> fibers(kFibers, nullptr);
  for (int i = 0; i < kFibers; i++) {
    counters[i].id = i;
    counters[i].steps = kSteps;
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fibers[i], counterEntry,
      &counters[i], GCU_FIBER_MIN_STACK_SIZE * 4, nullptr));
  }
  for (int s = 0; s <= kSteps; s++) {
    for (int i = 0; i < kFibers; i++) {
      if (!gcu_fiber_is_finished(fibers[i])) {
        ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fibers[i]));
      }
    }
  }
  for (int i = 0; i < kFibers; i++) {
    long expect = i * 1000L + (kSteps - 1) * kSteps / 2;
    EXPECT_EQ(expect, counters[i].sum) << "fiber " << i;
    EXPECT_TRUE(gcu_fiber_is_finished(fibers[i]));
    EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fibers[i]));
  }
}

__attribute__((noinline)) long deepRecurse(int depth) {
  volatile char pad[1024];
  pad[0] = (char)depth;
  pad[1023] = (char)depth;
  if (depth == 0) {
    return pad[0] + pad[1023];
  }
  return deepRecurse(depth - 1) + pad[0] + pad[1023];
}

struct Deep {
  long result = -1;
};
void deepEntry(void * arg) {
  static_cast<Deep *>(arg)->result = deepRecurse(48);
}

TEST_F(Fiber, ARequestedStackIsUsableToItsFullDepth) {
  REQUIRE_FIBERS();
  Deep d;
  GCU_Fiber * f = nullptr;
  // 48 KiB of frames in a 256 KiB stack, with room for sanitizer frames.
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, deepEntry, &d,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_TRUE(gcu_fiber_is_finished(f));
  EXPECT_NE(-1, d.result);
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

//
// What only a sanitizer can see: the switch annotations.
//

/**
 * Frames with redzone-guarded locals, abandoned by an exception.  Unwinding
 * skips the epilogues that would clear the redzones, so AddressSanitizer
 * clears them itself when told which stack is current, in
 * __asan_handle_no_return().  If the fiber's stack was never announced it
 * declines to ("ASan is ignoring requested __asan_handle_no_return"), the
 * redzones stay poisoned, and the next calls to land on that part of the stack
 * are reported as overflowing a variable that is not there.
 */
volatile char gSink;

__attribute__((noinline)) void throwThroughFrames(int depth) {
  volatile char pad[96];
  pad[0] = (char)depth;
  if (depth == 0) {
    throw 7;
  }
  throwThroughFrames(depth - 1);
  pad[1] = pad[0];
  gSink = pad[1];
}

__attribute__((noinline)) int touchFrames(int depth) {
  volatile char pad[160];
  for (int i = 0; i < 160; i++) {
    pad[i] = (char)(depth + i);
  }
  if (depth == 0) {
    return pad[5];
  }
  return touchFrames(depth - 1) + pad[7];
}

struct Thrown {
  int caught = 0;
  long sum = 0;
  int stalePoison = 0;
};
void thrownEntry(void * arg) {
  Thrown * t = static_cast<Thrown *>(arg);
  for (int round = 0; round < 20; round++) {
    try {
      throwThroughFrames(12);
    } catch (int) {
      t->caught++;
    }
#ifdef GCU_TEST_ASAN
    // The frames the exception skipped lie just below this one and are dead.
    // Nothing may be left poisoned there.  Read before touchFrames() runs,
    // because that rewrites the same shadow as it goes and would hide it.
    {
      uintptr_t here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
      if (__asan_region_is_poisoned(reinterpret_cast<void *>(here - 4096),
            2560) != nullptr) {
        t->stalePoison++;
      }
    }
#endif
    t->sum += touchFrames(14);
    gcu_fiber_yield();
  }
}

TEST_F(Fiber, ExceptionsCaughtInsideAFiberLeaveNoStalePoisonOnItsStack) {
  REQUIRE_FIBERS();
  Thrown t;
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, thrownEntry, &t,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  for (int i = 0; i < 20; i++) {
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  }
  EXPECT_EQ(20, t.caught);
  EXPECT_EQ(0, t.stalePoison)
    << "poison left on the dead frames of a fiber's stack: AddressSanitizer "
       "was not told which stack it was on";
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_TRUE(gcu_fiber_is_finished(f));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

#ifdef GCU_TEST_TSAN
struct TsanSeen {
  void * outer = nullptr;
  void * inner = nullptr;
};
GCU_Fiber * gTsanInner = nullptr;
void tsanInnerEntry(void * arg) {
  static_cast<TsanSeen *>(arg)->inner = __tsan_get_current_fiber();
}
void tsanOuterEntry(void * arg) {
  static_cast<TsanSeen *>(arg)->outer = __tsan_get_current_fiber();
  gcu_fiber_switch_to(gTsanInner);
}

TEST_F(Fiber, EachFiberRunsAsItsOwnContextToThreadSanitizer) {
  // Only meaningful, and only built, under ThreadSanitizer.  Without the
  // announcement every fiber is the host thread to it: it then keeps one
  // shadow call stack for all of them and reads their accesses as one
  // thread's, which no race report can say is wrong, so nothing but this
  // test would ever notice.
  REQUIRE_FIBERS();
  TsanSeen seen;
  GCU_Fiber * outer = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&gTsanInner, tsanInnerEntry, &seen,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&outer, tsanOuterEntry, &seen,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  void * host = __tsan_get_current_fiber();
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(outer));
  EXPECT_NE(host, seen.outer);
  EXPECT_NE(host, seen.inner);
  EXPECT_NE(seen.outer, seen.inner);
  EXPECT_EQ(host, __tsan_get_current_fiber())
    << "control must return to the host thread's context";
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(outer));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(gTsanInner));
}
#endif

__attribute__((noinline)) void parkDeep(int depth) {
  volatile char pad[16];
  pad[0] = (char)depth;
  if (depth == 0) {
    gcu_fiber_yield();
    return;
  }
  parkDeep(depth - 1);
  pad[1] = pad[0];
}

void parkDeepEntry(void *) {
  parkDeep(400);
}

TEST_F(Fiber, ManyFibersParkedDeepInTheirCallsThenAllFinish) {
  // Each parked fiber is a few hundred calls deep.  A thread-wide record of
  // the call stack, which is what ThreadSanitizer keeps unless it is told
  // about each fiber, would hold every fiber's frames at once.
  REQUIRE_FIBERS();
  const int kFibers = 300;
  vector<GCU_Fiber *> fibers(kFibers, nullptr);
  for (int i = 0; i < kFibers; i++) {
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fibers[i], parkDeepEntry,
      nullptr, GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fibers[i]));
  }
  for (int i = 0; i < kFibers; i++) {
    ASSERT_FALSE(gcu_fiber_is_finished(fibers[i]));
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fibers[i]));
    EXPECT_TRUE(gcu_fiber_is_finished(fibers[i]));
    EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fibers[i]));
  }
}

#ifdef GCU_TEST_ASAN
uintptr_t gRedzoneFrame = 0;
void redzoneEntry(void *) {
  gRedzoneFrame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  // A large local with redzones around it, live while the fiber is parked.
  volatile char big[512];
  for (int i = 0; i < 512; i++) big[i] = (char)i;
  gcu_fiber_yield();
  gSink = big[7];
}

TEST_F(Fiber, ANewStackStartsCleanOfADestroyedFibersRedzones) {
  // Only meaningful with use-after-return detection off, when locals are on
  // the real stack and a parked fiber's redzones are poisoned there; make
  // test-asan runs this binary in that mode as well.
  REQUIRE_FIBERS();
  const size_t size = GCU_FIBER_MIN_STACK_SIZE * 2;
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, redzoneEntry, nullptr, size,
    nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  uintptr_t parkedFrame = gRedzoneFrame;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));

  GCU_Fiber * g = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&g, redzoneEntry, nullptr, size,
    nullptr));
  // The new stack, before anything has run on it.  The parked fiber's frame
  // lay within a kilobyte or two below its frame address.
  void * bad = __asan_region_is_poisoned(
    reinterpret_cast<void *>(parkedFrame - 2048), 2048 - 16);
  // Learn whether the mapping was in fact reused; a test that checked some
  // other region would pass for nothing.
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(g));
  if (gRedzoneFrame != parkedFrame) {
    gcu_fiber_destroy(g);
    GTEST_SKIP() << "the new stack did not reuse the old one's address";
  }
  EXPECT_EQ(nullptr, bad)
    << "the new fiber's stack still carries the destroyed fiber's poison";
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(g));
}
#endif

//
// Destroying fibers in every state; the stacks are reused at one address.
//

struct Parked {
  volatile int arrived = 0;
  int resumed_after_destroy = 0;
};
void parkedEntry(void * arg) {
  Parked * p = static_cast<Parked *>(arg);
  // Locals a sanitizer will have put redzones around, live across the yield.
  volatile char buffer[200];
  volatile int counters[16];
  for (int i = 0; i < 200; i++) buffer[i] = (char)i;
  for (int i = 0; i < 16; i++) counters[i] = i;
  p->arrived = buffer[199] + counters[15];
  gcu_fiber_yield();
  p->resumed_after_destroy = 1; // never reached once destroyed
}

TEST_F(Fiber, DestroyingASuspendedFiberReleasesItsStackWithoutUnwinding) {
  REQUIRE_FIBERS();
  // Repeated, so the stack mappings are reused at the same addresses and
  // whatever poison the abandoned frames left behind is met by the next fiber.
  for (int round = 0; round < 40; round++) {
    Parked p;
    GCU_Fiber * f = nullptr;
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, parkedEntry, &p,
      GCU_FIBER_MIN_STACK_SIZE * 2, nullptr));
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
    ASSERT_NE(0, p.arrived);
    ASSERT_FALSE(gcu_fiber_is_finished(f));
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
    EXPECT_EQ(0, p.resumed_after_destroy)
      << "destroy must not run any more of the fiber";
  }
}

TEST_F(Fiber, DestroyingANeverStartedAndAFinishedFiberBothWork) {
  REQUIRE_FIBERS();
  GCU_Fiber * fresh = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fresh, nothingEntry, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fresh));

  GCU_Fiber * done = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&done, nothingEntry, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(done));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(done));
}

//
// A fiber stays on its thread.
//

struct Tick {
  atomic<int> ticks{0};
};
void tickEntry(void * arg) {
  Tick * t = static_cast<Tick *>(arg);
  t->ticks++;
  gcu_fiber_yield();
  t->ticks++;
}

TEST_F(Fiber, ResumingOrDestroyingFromAnotherThreadIsRefusedAndHarmless) {
  REQUIRE_FIBERS();
  Tick t;
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, tickEntry, &t,
    GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  ASSERT_EQ(1, t.ticks.load());

  GCU_Fiber_Result resumed = GCU_FIBER_OK;
  GCU_Fiber_Result destroyed = GCU_FIBER_OK;
  GCU_Fiber * currentThere = reinterpret_cast<GCU_Fiber *>(0x1);
  GCU_Fiber_Result yielded = GCU_FIBER_OK;
  thread other([&] {
    resumed = gcu_fiber_switch_to(f);
    destroyed = gcu_fiber_destroy(f);
    currentThere = gcu_fiber_current();
    yielded = gcu_fiber_yield();
  });
  other.join();

  EXPECT_EQ(GCU_FIBER_ERR_THREAD, resumed);
  EXPECT_EQ(GCU_FIBER_ERR_THREAD, destroyed);
  EXPECT_EQ(nullptr, currentThere);
  EXPECT_EQ(GCU_FIBER_ERR_STATE, yielded);
  EXPECT_EQ(1, t.ticks.load()) << "the refused resume must not have run it";

  // Untouched: it resumes on its own thread exactly where it left off.
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  EXPECT_EQ(2, t.ticks.load());
  EXPECT_TRUE(gcu_fiber_is_finished(f));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
}

TEST_F(Fiber, AFiberMadeOnAnotherThreadIsRefusedHere) {
  REQUIRE_FIBERS();
  Tick t;
  GCU_Fiber * f = nullptr;
  GCU_Fiber_Result made = GCU_FIBER_ERR_STATE;
  GCU_Fiber_Result finishedThere = GCU_FIBER_ERR_STATE;
  GCU_Fiber_Result destroyedThere = GCU_FIBER_ERR_STATE;
  atomic<int> ready{0};
  atomic<int> release{0};
  thread other([&] {
    made = gcu_fiber_create(&f, tickEntry, &t, GCU_FIBER_DEFAULT_STACK_SIZE,
      nullptr);
    if (made != GCU_FIBER_OK) {
      ready = 1;
      return;
    }
    gcu_fiber_switch_to(f);
    ready = 1;
    // Held open so the refusals below happen while the owner is alive; the
    // owner then finishes and releases its own fiber, which is the only party
    // that may.
    while (release.load() == 0) {
      this_thread::yield();
    }
    finishedThere = gcu_fiber_switch_to(f);
    destroyedThere = gcu_fiber_destroy(f);
  });
  while (ready.load() == 0) {
    this_thread::yield();
  }
  ASSERT_EQ(GCU_FIBER_OK, made);
  ASSERT_EQ(1, t.ticks.load());

  EXPECT_EQ(GCU_FIBER_ERR_THREAD, gcu_fiber_switch_to(f));
  EXPECT_EQ(GCU_FIBER_ERR_THREAD, gcu_fiber_destroy(f));
  EXPECT_EQ(1, t.ticks.load());

  release = 1;
  other.join();
  EXPECT_EQ(GCU_FIBER_OK, finishedThere);
  EXPECT_EQ(GCU_FIBER_OK, destroyedThere);
  EXPECT_EQ(2, t.ticks.load());
}

//
// Allocation failure.
//

struct AllocCounts {
  atomic<int> allocs{0};
  atomic<int> frees{0};
  bool fail = false;
};
void * countingMalloc(void * ctx, size_t n) {
  AllocCounts * c = static_cast<AllocCounts *>(ctx);
  if (c->fail) return nullptr;
  c->allocs++;
  return malloc(n);
}
void * countingCalloc(void * ctx, size_t items, size_t n) {
  AllocCounts * c = static_cast<AllocCounts *>(ctx);
  if (c->fail) return nullptr;
  c->allocs++;
  return calloc(items, n);
}
void * countingRealloc(void * ctx, void * p, size_t n) {
  AllocCounts * c = static_cast<AllocCounts *>(ctx);
  if (c->fail) return nullptr;
  if (p == nullptr) c->allocs++;
  return realloc(p, n);
}
void countingFree(void * ctx, void * p) {
  AllocCounts * c = static_cast<AllocCounts *>(ctx);
  if (p != nullptr) c->frees++;
  free(p);
  // A free that disturbs errno, as a real one is allowed to: the library has
  // to keep the mapping error across the free it makes on that path.
  errno = 0;
}

GCU_Allocator countingAllocator(AllocCounts * c) {
  GCU_Allocator a;
  a.ctx = c;
  a.malloc_fn = countingMalloc;
  a.calloc_fn = countingCalloc;
  a.realloc_fn = countingRealloc;
  a.free_fn = countingFree;
  return a;
}

TEST_F(Fiber, ADescriptorAllocationFailureIsAnErrorAndLeavesNothing) {
  REQUIRE_FIBERS();
  AllocCounts counts;
  GCU_Allocator alloc = countingAllocator(&counts);
  GCU_Fiber * f = reinterpret_cast<GCU_Fiber *>(0x1);
  counts.fail = true;
  EXPECT_EQ(GCU_FIBER_ERR_OOM, gcu_fiber_create(&f, nothingEntry, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, &alloc));
  EXPECT_EQ(reinterpret_cast<GCU_Fiber *>(0x1), f);
  counts.fail = false;
  EXPECT_EQ(0, counts.allocs.load());
}

#ifndef _WIN32
TEST_F(Fiber, EveryOutOfMemoryLeavesENOMEMForTheErrorApi) {
  REQUIRE_FIBERS();
  GCU_Fiber * f = nullptr;
  // A failing allocator is not required to set errno; the library says so.
  AllocCounts counts;
  GCU_Allocator alloc = countingAllocator(&counts);
  counts.fail = true;
  errno = 0;
  EXPECT_EQ(GCU_FIBER_ERR_OOM, gcu_fiber_create(&f, nothingEntry, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, &alloc));
  EXPECT_EQ(ENOMEM, gcu_error_last()) << "descriptor allocation failed";
  // A size no mapping can have, before anything is allocated.
  errno = 0;
  EXPECT_EQ(GCU_FIBER_ERR_OOM, gcu_fiber_create(&f, nothingEntry, nullptr,
    SIZE_MAX, nullptr));
  EXPECT_EQ(ENOMEM, gcu_error_last()) << "a size beyond any mapping";
}
#endif

TEST_F(Fiber, EveryAllocationIsReturnedOnDestroy) {
  REQUIRE_FIBERS();
  AllocCounts counts;
  GCU_Allocator alloc = countingAllocator(&counts);
  GCU_Fiber * f = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, nothingEntry, nullptr,
    GCU_FIBER_DEFAULT_STACK_SIZE, &alloc));
  EXPECT_GT(counts.allocs.load(), 0);
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
  EXPECT_EQ(counts.allocs.load(), counts.frees.load());
}

#ifndef _WIN32
/**
 * Run @p body in a forked child with a hard time limit and return its wait
 * status.  The child never returns into gtest.
 */
template <class F>
int inChild(F body) {
  fflush(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(60);
    body();
    _exit(0);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid) return -1;
  return status;
}

uint64_t addressSpaceBytes() {
  FILE * f = fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  unsigned long pages = 0;
  int got = fscanf(f, "%lu", &pages);
  fclose(f);
  return got == 1 ? (uint64_t)pages * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
}

TEST_F(Fiber, AFailedStackMappingIsAnErrorNotACrashOrALeak) {
  REQUIRE_FIBERS();
  REQUIRE_FORK();
  // Forked, because the limit is permanent for the process that sets it.
  // The cap is set just above what is mapped now, so only the large request
  // below can fail and everything the runtime does around it still works.
  int status = inChild([] {
    uint64_t used = addressSpaceBytes();
    if (used == 0) _exit(70);
    struct rlimit lim;
    lim.rlim_cur = lim.rlim_max = (rlim_t)(used + (8u << 20));
    if (setrlimit(RLIMIT_AS, &lim) != 0) _exit(71);

    AllocCounts counts;
    GCU_Allocator alloc = countingAllocator(&counts);
    GCU_Fiber * f = reinterpret_cast<GCU_Fiber *>(0x1);
    GCU_Fiber_Result r = gcu_fiber_create(&f, nothingEntry, nullptr,
      (size_t)256 << 20, &alloc);
    if (r != GCU_FIBER_ERR_OOM) _exit(72);
    int osError = gcu_error_last(); // first thing after the call
    if (f != reinterpret_cast<GCU_Fiber *>(0x1)) _exit(73);
    if (counts.allocs.load() != counts.frees.load()) _exit(74);
    // The OS error is reported through the library's own error API, and is
    // the mmap failure: errno 0 would also have a message, so compare it.
    if (osError != ENOMEM) _exit(75);

    // And a request that fits still works under the same limit.
    r = gcu_fiber_create(&f, nothingEntry, nullptr,
      GCU_FIBER_DEFAULT_STACK_SIZE, &alloc);
    if (r != GCU_FIBER_OK) _exit(76);
    if (gcu_fiber_switch_to(f) != GCU_FIBER_OK) _exit(77);
    if (gcu_fiber_destroy(f) != GCU_FIBER_OK) _exit(78);
    if (counts.allocs.load() != counts.frees.load()) _exit(79);
  });
  ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
  EXPECT_EQ(0, WEXITSTATUS(status))
    << "70 no /proc, 71 setrlimit, 72 not OOM, 73 wrote output, 74 leaked, "
       "75 no OS error, 76-79 a later fiber failed";
}

#ifdef __linux__
//
// The guard page, read from the kernel's own map of the process.  This does
// not depend on where the stacks happen to land, which the overflow tests do.
//

struct MapsProbe {
  size_t stackSize = 0;
  size_t page = 0;
  bool found = false;
  string problem;
};

void mapsEntry(void * arg) {
  MapsProbe * probe = static_cast<MapsProbe *>(arg);
  // The frame address, not a local's: under AddressSanitizer's use-after-
  // return detection locals live on a heap-backed fake stack.
  uintptr_t here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  FILE * maps = fopen("/proc/self/maps", "r");
  if (maps == nullptr) {
    probe->problem = "cannot open /proc/self/maps";
    return;
  }
  struct Line {
    uintptr_t start, end;
    string perms;
  };
  vector<Line> lines;
  char text[512];
  while (fgets(text, sizeof(text), maps) != nullptr) {
    unsigned long start = 0, end = 0;
    char perms[8] = {0};
    if (sscanf(text, "%lx-%lx %7s", &start, &end, perms) == 3) {
      lines.push_back({start, end, perms});
    }
  }
  fclose(maps);
  for (size_t i = 0; i < lines.size(); i++) {
    if (here >= lines[i].start && here < lines[i].end) {
      probe->found = true;
      if (lines[i].perms.substr(0, 3) != "rw-") {
        probe->problem = "the stack is not readable and writable: " +
          lines[i].perms;
      } else if (lines[i].end - lines[i].start < probe->stackSize) {
        // At least: the kernel merges adjacent anonymous mappings with the
        // same permissions, so a neighbour above may make it longer.
        probe->problem = "the stack mapping is only " +
          to_string(lines[i].end - lines[i].start) + " bytes, not " +
          to_string(probe->stackSize);
      } else if (i == 0 || lines[i - 1].end != lines[i].start) {
        probe->problem = "nothing is mapped directly below the stack";
      } else if (lines[i - 1].perms.substr(0, 3) != "---") {
        probe->problem = "the page below the stack is accessible: " +
          lines[i - 1].perms;
      } else if (lines[i - 1].end - lines[i - 1].start != probe->page) {
        probe->problem = "the guard is " +
          to_string(lines[i - 1].end - lines[i - 1].start) +
          " bytes, not one page";
      }
      return;
    }
  }
  probe->problem = "the fiber's stack is in no mapping";
}

TEST_F(Fiber, EveryStackHasOneInaccessibleGuardPageBelowIt) {
  REQUIRE_FIBERS();
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  // Requested sizes, and what each rounds up to.
  const size_t requested[] = {GCU_FIBER_MIN_STACK_SIZE, 100000,
    GCU_FIBER_DEFAULT_STACK_SIZE, 1 << 20};
  for (size_t want : requested) {
    MapsProbe probe;
    probe.page = page;
    probe.stackSize = (want + page - 1) / page * page;
    GCU_Fiber * f = nullptr;
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&f, mapsEntry, &probe, want,
      nullptr));
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(f));
    EXPECT_TRUE(probe.found) << "size " << want;
    EXPECT_EQ("", probe.problem) << "size " << want;
    EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(f));
  }
}
#endif // __linux__

//
// Stack overflow.
//

long gOverflowLimit = 1L << 40;

__attribute__((noinline)) long overflowRecurse(long depth) {
  volatile char pad[512];
  pad[0] = (char)depth;
  if (depth >= gOverflowLimit) {
    return pad[0];
  }
  return overflowRecurse(depth + 1) + pad[0];
}

void overflowEntry(void *) {
  overflowRecurse(0);
}

TEST_F(Fiber, OverflowingTheStackFaultsOnTheGuardPage) {
  REQUIRE_FIBERS();
  REQUIRE_FORK();
  int status = inChild([] {
    // The runtimes under test install a SIGSEGV handler of their own; the
    // question here is what the hardware does, so take the default.
    signal(SIGSEGV, SIG_DFL);
    GCU_Fiber * f = nullptr;
    if (gcu_fiber_create(&f, overflowEntry, nullptr,
          GCU_FIBER_MIN_STACK_SIZE * 2, nullptr) != GCU_FIBER_OK) {
      _exit(70);
    }
    gcu_fiber_switch_to(f);
    _exit(71); // returning means the overflow was not stopped
  });
  ASSERT_TRUE(WIFSIGNALED(status)) << "status " << status
    << " (exit code 70: create failed; 71: ran past the end of its stack)";
  EXPECT_EQ(SIGSEGV, WTERMSIG(status));
}

// State for the handler, which must use nothing that is not async-signal-safe.
struct OverflowWatch {
  volatile unsigned char * neighbour = nullptr;
  size_t neighbourBytes = 0;
  uintptr_t neighbourFrame = 0;
  uintptr_t overflowerLocal = 0;
  size_t stackSize = 0;
  size_t page = 0;
};
OverflowWatch gWatch;

extern "C" void overflowHandler(int, siginfo_t * info, void *) {
  uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
  // The overflower's frame sits within a few KiB of the top of its stack, so
  // the bottom of the stack is one stack size below it, and the guard page is
  // the page under that.  The first byte touched past the bottom is in it.
  uintptr_t stackLow = gWatch.overflowerLocal - gWatch.stackSize;
  bool onGuard = fault >= stackLow - gWatch.page - 4096 &&
                 fault < stackLow + 4096;
  bool intact = true;
  for (size_t i = 0; i < gWatch.neighbourBytes; i++) {
    if (gWatch.neighbour[i] != 0xA5) intact = false;
  }
  _exit(!intact ? 43 : (onGuard ? 42 : 44));
}

void neighbourEntry(void *) {
  // At the top of this fiber's stack, which is what an overflow with no guard
  // would reach first.
  volatile unsigned char local[1024];
  for (size_t i = 0; i < sizeof(local); i++) local[i] = 0xA5;
  gWatch.neighbour = local;
  gWatch.neighbourBytes = sizeof(local);
  // The frame address, not a local's: under AddressSanitizer's use-after-
  // return detection the locals live on a heap-backed fake stack, so only the
  // frame address says where on the real stack this fiber is.
  gWatch.neighbourFrame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  gcu_fiber_yield();
  // Resumed only if the test is broken; leave evidence.
  for (size_t i = 0; i < sizeof(local); i++) local[i] = 0;
}

void overflowerEntry(void *) {
  gWatch.overflowerLocal =
    reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  gcu_fiber_yield();
  overflowRecurse(0);
}

TEST_F(Fiber, AnOverflowStopsAtTheGuardAndTheNeighbouringStackIsUntouched) {
  REQUIRE_FIBERS();
  REQUIRE_FORK();
  int status = inChild([] {
    stack_t alt;
    static char altStack[1 << 16];
    alt.ss_sp = altStack;
    alt.ss_size = sizeof(altStack);
    alt.ss_flags = 0;
    if (sigaltstack(&alt, nullptr) != 0) _exit(60);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = overflowHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, nullptr) != 0) _exit(61);

    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t stackSize =
      (GCU_FIBER_MIN_STACK_SIZE * 2 + page - 1) / page * page;
    gWatch.stackSize = stackSize;
    gWatch.page = page;

    // The overflowing fiber is made first and the neighbour second.  Mappings
    // are handed out downward, so the neighbour normally sits directly below
    // and an overflow with no guard would write into it.  The layout is
    // measured, not assumed: if the two did not land adjacent the attempt is
    // thrown away and made again, so the check cannot pass for want of a
    // neighbour in the way.
    GCU_Fiber * over = nullptr;
    GCU_Fiber * near = nullptr;
    bool adjacent = false;
    for (int attempt = 0; attempt < 32 && !adjacent; attempt++) {
      if (gcu_fiber_create(&over, overflowerEntry, nullptr, stackSize,
            nullptr) != GCU_FIBER_OK) _exit(62);
      if (gcu_fiber_create(&near, neighbourEntry, nullptr, stackSize,
            nullptr) != GCU_FIBER_OK) _exit(63);
      if (gcu_fiber_switch_to(near) != GCU_FIBER_OK) _exit(64);
      if (gcu_fiber_switch_to(over) != GCU_FIBER_OK) _exit(65);
      uintptr_t nb = gWatch.neighbourFrame;
      uintptr_t ob = gWatch.overflowerLocal;
      // Low to high: the neighbour's stack, the overflower's guard page,
      // the overflower's stack.  Both frames are near the top of theirs.
      uintptr_t expected = stackSize + page;
      adjacent = ob > nb && ob - nb + 8192 >= expected &&
                 ob - nb <= expected + 8192;
      if (!adjacent && getenv("GCU_TEST_FIBER_DEBUG")) {
        fprintf(stderr, "attempt %d: over %lx near %lx diff %ld want %zu\n",
          attempt, (unsigned long)ob, (unsigned long)nb, (long)(ob - nb),
          expected);
      }
      if (!adjacent) {
        gcu_fiber_destroy(over);
        gcu_fiber_destroy(near);
      }
    }
    if (!adjacent) _exit(66);

    gcu_fiber_switch_to(over); // recurses until the guard page stops it
    _exit(67); // returning means the overflow was not stopped
  });
  ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
  if (WEXITSTATUS(status) == 66) {
    // A skip, not a pass: with no neighbour in the way nothing was checked.
    // Sanitizer runtimes allocate their own per-fiber state between two
    // stacks, so there it is the usual outcome; the layout test below checks
    // the guard page directly and does not depend on where stacks land.
    GTEST_SKIP() << "the two stacks never landed adjacent in 32 attempts";
  }
  EXPECT_EQ(42, WEXITSTATUS(status))
    << "43: the neighbouring stack was written; 44: faulted somewhere other "
       "than just below the overflowing stack; 60-66: setup (66: the two "
       "stacks never landed adjacent); 67: no fault";
}
#endif // !_WIN32

} // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
