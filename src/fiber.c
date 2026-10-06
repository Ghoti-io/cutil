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
 * Fibers.  See fiber.h for the contract and `documentation/fiber.md` for the
 * reasoning.
 *
 * Three implementations share this file, chosen at compile time: a pair of
 * hand-written context switches for ELF targets (x86-64 System V and arm64),
 * the Windows Fiber API, and a stub for everything else that reports
 * ::GCU_FIBER_ERR_UNSUPPORTED so that a build for any architecture still
 * links.
 *
 * ## Planted defects
 *
 * The `GCU_FIBER_PLANT_*` macros each compile one deliberate defect into the
 * switch.  They exist for `make check-fiber-defects`, which builds the
 * library with one of them and requires the fiber tests, or the sanitizer
 * running them, to fail.  A gate that has never been seen to fail may be
 * checking nothing.  No ordinary build defines one; defining one anywhere
 * else produces a library that is wrong on purpose.
 *
 *   - `GCU_FIBER_PLANT_NO_MXCSR`: the x86-64 switch does not save or restore
 *     the MXCSR control bits.
 *   - `GCU_FIBER_PLANT_NO_X87CW`: the x86-64 switch does not save or restore
 *     the x87 control word.
 *   - `GCU_FIBER_PLANT_NO_FPCR`: the arm64 switch does not save or restore
 *     FPCR.
 *   - `GCU_FIBER_PLANT_NO_ASAN`: the switch does not tell AddressSanitizer
 *     about the new stack.
 *   - `GCU_FIBER_PLANT_NO_TSAN`: the switch does not tell ThreadSanitizer
 *     about the new fiber.
 *   - `GCU_FIBER_PLANT_NO_CALLEE_SAVED`: the x86-64 switch does not save or
 *     restore r12; the arm64 switch does not save or restore x28.
 *
 * The Windows arm honours `GCU_FIBER_PLANT_NO_MXCSR` and
 * `GCU_FIBER_PLANT_NO_X87CW` as well, since it saves the same two registers,
 * and `tools/xwin/fiber.sh` requires the rounding test to fail on both.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/fiber.h>

//
// Sanitizer detection.  GCC defines a macro per sanitizer; clang answers
// __has_feature.
//
#if defined(__SANITIZE_ADDRESS__)
#define GCU_FIBER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GCU_FIBER_ASAN 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define GCU_FIBER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GCU_FIBER_TSAN 1
#endif
#endif

#ifdef GCU_FIBER_PLANT_NO_ASAN
#undef GCU_FIBER_ASAN_SWITCH
#elif defined(GCU_FIBER_ASAN)
#define GCU_FIBER_ASAN_SWITCH 1
#endif

#ifdef GCU_FIBER_PLANT_NO_TSAN
#undef GCU_FIBER_TSAN_SWITCH
#elif defined(GCU_FIBER_TSAN)
#define GCU_FIBER_TSAN_SWITCH 1
#endif

#ifdef GCU_FIBER_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

#ifdef GCU_FIBER_TSAN
#include <sanitizer/tsan_interface.h>
#endif

#if !defined(_WIN32) && defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GCU_FIBER_VALGRIND 1
#endif
#endif

static const char * const gcu_fiber_result_names[GCU_FIBER_RESULT_COUNT] = {
  [GCU_FIBER_OK] = "ok",
  [GCU_FIBER_ERR_INVALID] = "invalid argument",
  [GCU_FIBER_ERR_OOM] = "out of memory",
  [GCU_FIBER_ERR_UNSUPPORTED] = "fibers are not supported on this platform",
  [GCU_FIBER_ERR_THREAD] = "not the thread that owns the fiber",
  [GCU_FIBER_ERR_STATE] = "not valid in the fiber's current state",
};

const char * gcu_fiber_result_string(GCU_Fiber_Result result) {
  if ((unsigned)result >= (unsigned)GCU_FIBER_RESULT_COUNT) {
    return "unknown fiber result";
  }
  return gcu_fiber_result_names[result];
}

/**
 * Where a fiber is in its life.
 *
 * RUNNING is the fiber on the CPU.  RESUMING is a fiber that has switched to
 * another and waits for it; it is on the resumer chain, so it can be neither
 * resumed (it would be re-entered) nor destroyed (its stack is live).
 */
typedef enum GCU_Fiber_State {
  GCU_FIBER_STATE_NEW,       ///< Created, never run.
  GCU_FIBER_STATE_SUSPENDED, ///< Yielded; waiting to be resumed.
  GCU_FIBER_STATE_RUNNING,   ///< On the CPU now.
  GCU_FIBER_STATE_RESUMING,  ///< Has resumed another fiber, awaiting it.
  GCU_FIBER_STATE_FINISHED,  ///< The entry function has returned.
} GCU_Fiber_State;

#if defined(_WIN32)
//==========================================================================
// Windows: the Fiber API.
//
// The x64 ABI makes XMM6-XMM15, MXCSR[6:15] and the x87 control word
// non-volatile, and the thread information block carries the stack limits;
// a hand-written switch would have to maintain all of it.  The Fiber API
// already does.  Stack guard pages are the system's own.
//==========================================================================
#include <windows.h>

#include <ghoti.io/cutil/error.h>

struct GCU_Fiber {
  const GCU_Allocator * allocator; ///< Private.
  GCU_Fiber_Entry entry;           ///< Private.
  void * arg;                      ///< Private.
  LPVOID handle;                   ///< Private.  This fiber's Windows fiber.
  LPVOID resumer_handle;           ///< Private.  Who to give control back to.
  GCU_Fiber * resumer;             ///< Private.  NULL for the thread context.
  GCU_Fiber_State state;           ///< Private.
  DWORD owner;                     ///< Private.  Creating thread's id.
};

static _Thread_local GCU_Fiber * gcu_fiber_running = NULL;
static _Thread_local LPVOID gcu_fiber_thread_handle = NULL;

/*
 * Floating-point state.  The Fiber API can be asked to switch it
 * (FIBER_FLAG_FLOAT_SWITCH), and that flag is NOT relied on here: under wine
 * the two-fiber rounding test fails with the flag passed, on every probe, so
 * the flag cannot be what makes the test pass, and wine is the only
 * Windows-API implementation this was run against.  On x86-64 the same two
 * registers the System V switch saves are saved and restored around every
 * SwitchToFiber(), so the behaviour is this library's own and the same on
 * both platforms, whatever the Fiber API does with the flag.  Where there is
 * no such code (Windows on arm64) the flag is passed and nothing has measured
 * whether it is enough.
 */
#if defined(__x86_64__)
#include <xmmintrin.h>

#define GCU_FIBER_WIN_FLAGS 0
#define GCU_FIBER_WIN_OWN_FP 1

typedef struct GCU_Fiber_FPState {
  uint32_t mxcsr;
  uint16_t x87;
} GCU_Fiber_FPState;

static void gcu_fiber_fp_save(GCU_Fiber_FPState * state) {
  state->mxcsr = _mm_getcsr();
  __asm__ volatile("fnstcw %0" : "=m"(state->x87));
}

/**
 * Restore the control bits of @p state.  The MXCSR status flags (bits 0-5)
 * stay as they are now: they accumulate on the thread and are not part of a
 * fiber's environment, as in the System V switch.
 */
static void gcu_fiber_fp_restore(const GCU_Fiber_FPState * state) {
#ifndef GCU_FIBER_PLANT_NO_MXCSR
  _mm_setcsr((_mm_getcsr() & 0x3fu) | (state->mxcsr & 0xffc0u));
#endif
#ifndef GCU_FIBER_PLANT_NO_X87CW
  __asm__ volatile("fldcw %0" : : "m"(state->x87));
#endif
}

static const GCU_Fiber_FPState gcu_fiber_fp_default = {0x1f80u, 0x037fu};

/** SwitchToFiber() that leaves floating-point state with each context. */
static void gcu_fiber_win_switch(LPVOID target) {
  GCU_Fiber_FPState mine;
  gcu_fiber_fp_save(&mine);
  SwitchToFiber(target);
  gcu_fiber_fp_restore(&mine);
}
#else
#define GCU_FIBER_WIN_FLAGS FIBER_FLAG_FLOAT_SWITCH

static void gcu_fiber_win_switch(LPVOID target) {
  SwitchToFiber(target);
}
#endif

/**
 * Make the calling thread able to switch fibers.  A thread must itself be a
 * fiber before it can SwitchToFiber(); it stays one for the rest of its life.
 * The handle of the thread's own fiber is what a fiber switches back to when
 * the thread is its resumer.
 *
 * GetCurrentFiber() is an inline read of the thread information block, and
 * GCC 14 with the MinGW-w64 headers reports it as an out-of-bounds array
 * access under -Werror=array-bounds.  It is not one.  It is called only here,
 * only for a thread that was already a fiber before this library met it, and
 * the warning is switched off around this one function rather than the file.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static bool gcu_fiber_convert_thread(void) {
  if (gcu_fiber_thread_handle != NULL) {
    return true;
  }
  LPVOID handle = ConvertThreadToFiberEx(NULL, GCU_FIBER_WIN_FLAGS);
  if (handle == NULL) {
    if (GetLastError() != ERROR_ALREADY_FIBER) {
      return false;
    }
    handle = GetCurrentFiber();
  }
  gcu_fiber_thread_handle = handle;
  return true;
}
#pragma GCC diagnostic pop

static VOID WINAPI gcu_fiber_main(LPVOID param) {
  GCU_Fiber * self = (GCU_Fiber *)param;
#ifdef GCU_FIBER_WIN_OWN_FP
  // A new fiber starts in the default environment, not its resumer's.
  gcu_fiber_fp_restore(&gcu_fiber_fp_default);
#endif
  self->entry(self->arg);
  self->state = GCU_FIBER_STATE_FINISHED;
  gcu_fiber_win_switch(self->resumer_handle);
  // A finished fiber is never resumed; returning would end the thread.
  abort();
}

GCU_Fiber_Result gcu_fiber_create(GCU_Fiber ** out_fiber,
  GCU_Fiber_Entry entry, void * arg, size_t stack_size,
  const GCU_Allocator * allocator) {
  if (out_fiber == NULL || entry == NULL ||
      stack_size < GCU_FIBER_MIN_STACK_SIZE) {
    return GCU_FIBER_ERR_INVALID;
  }
  GCU_Fiber * fiber = gcu_allocator_calloc(allocator, 1, sizeof(*fiber));
  if (fiber == NULL) {
    return GCU_FIBER_ERR_OOM;
  }
  fiber->allocator = allocator;
  fiber->entry = entry;
  fiber->arg = arg;
  fiber->state = GCU_FIBER_STATE_NEW;
  fiber->owner = GetCurrentThreadId();
  fiber->handle = CreateFiberEx(0, stack_size, GCU_FIBER_WIN_FLAGS,
    gcu_fiber_main, fiber);
  if (fiber->handle == NULL) {
    gcu_allocator_free(allocator, fiber);
    return GCU_FIBER_ERR_OOM;
  }
  *out_fiber = fiber;
  return GCU_FIBER_OK;
}

GCU_Fiber_Result gcu_fiber_switch_to(GCU_Fiber * fiber) {
  if (fiber == NULL) {
    return GCU_FIBER_ERR_INVALID;
  }
  if (fiber->owner != GetCurrentThreadId()) {
    return GCU_FIBER_ERR_THREAD;
  }
  if (fiber->state != GCU_FIBER_STATE_NEW &&
      fiber->state != GCU_FIBER_STATE_SUSPENDED) {
    return GCU_FIBER_ERR_STATE;
  }
  if (!gcu_fiber_convert_thread()) {
    return GCU_FIBER_ERR_OOM;
  }
  GCU_Fiber * caller = gcu_fiber_running;
  if (caller != NULL) {
    caller->state = GCU_FIBER_STATE_RESUMING;
  }
  fiber->resumer = caller;
  fiber->resumer_handle = caller != NULL ? caller->handle
                                         : gcu_fiber_thread_handle;
  fiber->state = GCU_FIBER_STATE_RUNNING;
  gcu_fiber_running = fiber;
  gcu_fiber_win_switch(fiber->handle);
  gcu_fiber_running = caller;
  if (caller != NULL) {
    caller->state = GCU_FIBER_STATE_RUNNING;
  }
  fiber->resumer = NULL;
  return GCU_FIBER_OK;
}

GCU_Fiber_Result gcu_fiber_yield(void) {
  GCU_Fiber * self = gcu_fiber_running;
  if (self == NULL) {
    return GCU_FIBER_ERR_STATE;
  }
  self->state = GCU_FIBER_STATE_SUSPENDED;
  gcu_fiber_win_switch(self->resumer_handle);
  return GCU_FIBER_OK;
}

GCU_Fiber_Result gcu_fiber_destroy(GCU_Fiber * fiber) {
  if (fiber == NULL) {
    return GCU_FIBER_OK;
  }
  if (fiber->owner != GetCurrentThreadId()) {
    return GCU_FIBER_ERR_THREAD;
  }
  if (fiber->state == GCU_FIBER_STATE_RUNNING ||
      fiber->state == GCU_FIBER_STATE_RESUMING) {
    return GCU_FIBER_ERR_STATE;
  }
  DeleteFiber(fiber->handle);
  gcu_allocator_free(fiber->allocator, fiber);
  return GCU_FIBER_OK;
}

GCU_Fiber * gcu_fiber_current(void) {
  return gcu_fiber_running;
}

bool gcu_fiber_is_finished(const GCU_Fiber * fiber) {
  return fiber == NULL || fiber->state == GCU_FIBER_STATE_FINISHED;
}

#elif GCU_FIBER_SUPPORTED
//==========================================================================
// ELF, x86-64 System V and arm64: a hand-written switch.
//
// makecontext/swapcontext are not used: POSIX removed them in 2008, and
// swapcontext makes a system call on every switch to save the signal mask.
// The routine below saves exactly what the ABI makes callee-saved, plus the
// floating-point control state, and nothing else.
//==========================================================================
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ghoti.io/cutil/error.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/**
 * Save the running context's registers on its own stack, store the resulting
 * stack pointer through @p save_sp, switch to @p new_sp and restore what is
 * saved there.  Returns, in the new context, to wherever that context last
 * left from -- or, for a fresh stack, into gcu_fiber_main().
 */
void gcu_fiber_switch_asm(void ** save_sp, void * new_sp);

#if defined(__x86_64__)
//
// x86-64 System V.  Callee-saved: rbx, rbp, r12-r15; the x87 control word
// and MXCSR[6:15] (the control bits).  The MXCSR status flags (bits 0-5) are
// not callee-saved: they accumulate across a call and are left on the thread,
// so the restore keeps the flags as they are now and takes only the control
// bits from the saved word.
//
// Frame, at the saved stack pointer, lowest address first:
//    0  MXCSR (4 bytes)       4  x87 control word (2 bytes)
//    8  scratch for the merge
//   16  r15  24 r14  32 r13  40 r12  48 rbx  56 rbp   64  return address
//
// Not CET shadow-stack safe: `ret` into a stack the shadow stack has not seen
// faults where it is enforced.  Linux user space does not enforce it by
// default.
//
#ifdef GCU_FIBER_PLANT_NO_MXCSR
#define GCU_FIBER_ASM_MXCSR_SAVE ""
#define GCU_FIBER_ASM_MXCSR_LOAD ""
#else
#define GCU_FIBER_ASM_MXCSR_SAVE "  stmxcsr (%rsp)\n"
#define GCU_FIBER_ASM_MXCSR_LOAD \
  "  stmxcsr 8(%rsp)\n" \
  "  movl 8(%rsp), %eax\n" \
  "  andl $0x3f, %eax\n" \
  "  movl (%rsp), %ecx\n" \
  "  andl $0xffc0, %ecx\n" \
  "  orl %ecx, %eax\n" \
  "  movl %eax, 8(%rsp)\n" \
  "  ldmxcsr 8(%rsp)\n"
#endif

#ifdef GCU_FIBER_PLANT_NO_X87CW
#define GCU_FIBER_ASM_X87_SAVE ""
#define GCU_FIBER_ASM_X87_LOAD ""
#else
#define GCU_FIBER_ASM_X87_SAVE "  fnstcw 4(%rsp)\n"
#define GCU_FIBER_ASM_X87_LOAD "  fldcw 4(%rsp)\n"
#endif

#ifdef GCU_FIBER_PLANT_NO_CALLEE_SAVED
// Keep the frame the same size, so only r12 is wrong.
#define GCU_FIBER_ASM_PUSH_R12 "  subq $8, %rsp\n"
#define GCU_FIBER_ASM_POP_R12 "  addq $8, %rsp\n"
#else
#define GCU_FIBER_ASM_PUSH_R12 "  pushq %r12\n"
#define GCU_FIBER_ASM_POP_R12 "  popq %r12\n"
#endif

__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl gcu_fiber_switch_asm\n"
  ".hidden gcu_fiber_switch_asm\n"
  ".type gcu_fiber_switch_asm, @function\n"
  "gcu_fiber_switch_asm:\n"
  "  pushq %rbp\n"
  "  pushq %rbx\n"
  GCU_FIBER_ASM_PUSH_R12
  "  pushq %r13\n"
  "  pushq %r14\n"
  "  pushq %r15\n"
  "  subq $16, %rsp\n"
  GCU_FIBER_ASM_MXCSR_SAVE
  GCU_FIBER_ASM_X87_SAVE
  "  movq %rsp, (%rdi)\n"
  "  movq %rsi, %rsp\n"
  GCU_FIBER_ASM_MXCSR_LOAD
  GCU_FIBER_ASM_X87_LOAD
  "  addq $16, %rsp\n"
  "  popq %r15\n"
  "  popq %r14\n"
  "  popq %r13\n"
  GCU_FIBER_ASM_POP_R12
  "  popq %rbx\n"
  "  popq %rbp\n"
  "  ret\n"
  ".size gcu_fiber_switch_asm, .-gcu_fiber_switch_asm\n"
);

#define GCU_FIBER_DEFAULT_MXCSR 0x1f80u /* all exceptions masked, nearest */
#define GCU_FIBER_DEFAULT_X87CW 0x037fu /* all masked, 64-bit, nearest */

/**
 * Build the first frame on a new stack so that switching to it "returns" into
 * @p entry with the stack aligned as at a function's first instruction.
 *
 * @param top One past the highest usable byte; 16-byte aligned.
 * @return The stack pointer to switch to.
 */
static void * gcu_fiber_prepare_stack(char * top, void (*entry)(void)) {
  uint64_t * frame = (uint64_t *)(void *)(top - 80);
  memset(frame, 0, 80);
  uint32_t mxcsr = GCU_FIBER_DEFAULT_MXCSR;
  uint16_t cw = GCU_FIBER_DEFAULT_X87CW;
  memcpy((char *)frame, &mxcsr, sizeof(mxcsr));
  memcpy((char *)frame + 4, &cw, sizeof(cw));
  // Frame slot 1 is the merge scratch, slots 2..7 are the six registers, all
  // zero; slot 8 is the return address.  Slot 9 is a null return address
  // for a debugger's benefit and keeps the alignment at entry right: rsp is
  // 8 mod 16 there.
  frame[8] = (uint64_t)(uintptr_t)entry;
  return frame;
}

#elif defined(__aarch64__)
//
// arm64 (AAPCS64).  Callee-saved: x19-x28, x29 (frame pointer), x30 (link
// register), the low 64 bits of v8-v15 (d8-d15), and the stack pointer.  FPCR
// holds the rounding mode and trap enables and is switched with the rest;
// the FPSR status flags are not.
//
// Frame, 176 bytes, lowest address first:
//    0  x19..x28 (80 bytes)   80  x29, x30   96  d8..d15 (64 bytes)
//  160  FPCR
//
#ifdef GCU_FIBER_PLANT_NO_CALLEE_SAVED
// Only x27 is saved of the pair, so only x28 is wrong.  x28 and not x19: the
// compiler uses the low callee-saved registers for its own values, and a
// plant that crashes the harness shows nothing.
#define GCU_FIBER_ASM_SAVE_X28 "  str x27, [sp, #64]\n"
#define GCU_FIBER_ASM_LOAD_X28 "  ldr x27, [sp, #64]\n"
#else
#define GCU_FIBER_ASM_SAVE_X28 "  stp x27, x28, [sp, #64]\n"
#define GCU_FIBER_ASM_LOAD_X28 "  ldp x27, x28, [sp, #64]\n"
#endif

#ifdef GCU_FIBER_PLANT_NO_FPCR
#define GCU_FIBER_ASM_FPCR_SAVE ""
#define GCU_FIBER_ASM_FPCR_LOAD ""
#else
#define GCU_FIBER_ASM_FPCR_SAVE \
  "  mrs x2, fpcr\n" \
  "  str x2, [sp, #160]\n"
#define GCU_FIBER_ASM_FPCR_LOAD \
  "  ldr x2, [sp, #160]\n" \
  "  msr fpcr, x2\n"
#endif

__asm__(
  ".text\n"
  ".p2align 4\n"
  ".globl gcu_fiber_switch_asm\n"
  ".hidden gcu_fiber_switch_asm\n"
  ".type gcu_fiber_switch_asm, %function\n"
  "gcu_fiber_switch_asm:\n"
  "  sub sp, sp, #176\n"
  "  stp x19, x20, [sp, #0]\n"
  "  stp x21, x22, [sp, #16]\n"
  "  stp x23, x24, [sp, #32]\n"
  "  stp x25, x26, [sp, #48]\n"
  GCU_FIBER_ASM_SAVE_X28
  "  stp x29, x30, [sp, #80]\n"
  "  stp d8, d9, [sp, #96]\n"
  "  stp d10, d11, [sp, #112]\n"
  "  stp d12, d13, [sp, #128]\n"
  "  stp d14, d15, [sp, #144]\n"
  GCU_FIBER_ASM_FPCR_SAVE
  "  mov x2, sp\n"
  "  str x2, [x0]\n"
  "  mov sp, x1\n"
  GCU_FIBER_ASM_FPCR_LOAD
  "  ldp x19, x20, [sp, #0]\n"
  "  ldp x21, x22, [sp, #16]\n"
  "  ldp x23, x24, [sp, #32]\n"
  "  ldp x25, x26, [sp, #48]\n"
  GCU_FIBER_ASM_LOAD_X28
  "  ldp x29, x30, [sp, #80]\n"
  "  ldp d8, d9, [sp, #96]\n"
  "  ldp d10, d11, [sp, #112]\n"
  "  ldp d12, d13, [sp, #128]\n"
  "  ldp d14, d15, [sp, #144]\n"
  "  add sp, sp, #176\n"
  "  ret\n"
  ".size gcu_fiber_switch_asm, .-gcu_fiber_switch_asm\n"
);

/**
 * Build the first frame on a new stack so that switching to it "returns" into
 * @p entry.  FPCR starts at zero: round to nearest, no traps, no flush.
 *
 * @param top One past the highest usable byte; 16-byte aligned.
 * @return The stack pointer to switch to.
 */
static void * gcu_fiber_prepare_stack(char * top, void (*entry)(void)) {
  uint64_t * frame = (uint64_t *)(void *)(top - 176);
  memset(frame, 0, 176);
  frame[11] = (uint64_t)(uintptr_t)entry; // x30, the link register
  return frame;
}
#endif

struct GCU_Fiber {
  const GCU_Allocator * allocator; ///< Private.
  GCU_Fiber_Entry entry;           ///< Private.
  void * arg;                      ///< Private.
  GCU_Fiber * resumer;             ///< Private.  NULL for the thread context.
  void * sp;                       ///< Private.  Saved while suspended.
  void * resumer_sp;               ///< Private.  Where to return on a yield.
  void * mapping;                  ///< Private.  Start of guard + stack.
  size_t mapping_size;             ///< Private.  Guard page plus stack.
  char * stack_low;                ///< Private.  Lowest usable stack byte.
  size_t stack_size;               ///< Private.  Usable bytes.
  GCU_Fiber_State state;           ///< Private.
  pthread_t owner;                 ///< Private.  The creating thread.
#ifdef GCU_FIBER_ASAN_SWITCH
  const void * resumer_bottom;     ///< Private.  The resumer's stack, for ASan.
  size_t resumer_stack_size;       ///< Private.
#endif
#ifdef GCU_FIBER_TSAN_SWITCH
  void * tsan_fiber;               ///< Private.  This fiber's TSan context.
  void * tsan_resumer;             ///< Private.  Whose context to go back to.
#endif
#ifdef GCU_FIBER_VALGRIND
  unsigned valgrind_id;            ///< Private.  Valgrind stack registration.
#endif
};

/**
 * The fiber on the CPU for this thread.  A fiber never changes thread, so the
 * address of this variable is stable across a switch.
 */
static _Thread_local GCU_Fiber * gcu_fiber_running = NULL;

/**
 * Tell the sanitizers about a switch, and do it.  Written once so that the
 * three places that switch (resume, yield, finish) cannot disagree about the
 * annotation order.
 *
 * @param save_sp Where the departing context's stack pointer goes.
 * @param new_sp The destination's.
 * @param dest_bottom Destination stack, for ASan.
 * @param dest_size Its size.
 * @param leaving_for_good True when the departing context will never be
 *   resumed (a finishing fiber), so ASan may free its fake stack.
 * @param[out] asan_cookie Receives ASan's saved fake stack for the departing
 *   context; hand it to gcu_fiber_arrived() when this context next runs.
 */
static void gcu_fiber_leave(void ** save_sp, void * new_sp,
  const void * dest_bottom, size_t dest_size, bool leaving_for_good,
  void ** asan_cookie) {
#ifdef GCU_FIBER_ASAN_SWITCH
  __sanitizer_start_switch_fiber(
    leaving_for_good ? NULL : asan_cookie, dest_bottom, dest_size);
#else
  (void)dest_bottom;
  (void)dest_size;
  (void)leaving_for_good;
  (void)asan_cookie;
#endif
  gcu_fiber_switch_asm(save_sp, new_sp);
}

/**
 * Called by whatever context has just gained control, as the first thing it
 * does.  Completes the ASan switch begun in gcu_fiber_leave().
 *
 * @param asan_cookie The cookie gcu_fiber_leave() filled in when this
 *   context departed, or NULL on a fiber's first entry.
 * @param[out] from_bottom Receives the stack control came from.
 * @param[out] from_size Its size.
 */
static void gcu_fiber_arrived(void * asan_cookie, const void ** from_bottom,
  size_t * from_size) {
#ifdef GCU_FIBER_ASAN_SWITCH
  __sanitizer_finish_switch_fiber(asan_cookie, from_bottom, from_size);
#else
  (void)asan_cookie;
  (void)from_bottom;
  (void)from_size;
#endif
}

/**
 * Where a new fiber begins.  Reached by the `ret` of the first switch to it,
 * with no arguments: the fiber to run is the thread's running one.
 */
static void gcu_fiber_main(void) {
  GCU_Fiber * self = gcu_fiber_running;
#ifdef GCU_FIBER_ASAN_SWITCH
  gcu_fiber_arrived(NULL, &self->resumer_bottom, &self->resumer_stack_size);
#else
  gcu_fiber_arrived(NULL, NULL, NULL);
#endif

  self->entry(self->arg);

  self->state = GCU_FIBER_STATE_FINISHED;
#ifdef GCU_FIBER_TSAN_SWITCH
  __tsan_switch_to_fiber(self->tsan_resumer, 0);
#endif
#ifdef GCU_FIBER_ASAN_SWITCH
  gcu_fiber_leave(&self->sp, self->resumer_sp, self->resumer_bottom,
    self->resumer_stack_size, true, NULL);
#else
  gcu_fiber_leave(&self->sp, self->resumer_sp, NULL, 0, true, NULL);
#endif
  // A finished fiber is never resumed.
  abort();
}

/**
 * The size of a page, which is the guard's size and the stack's granule.
 */
static size_t gcu_fiber_page_size(void) {
  long page = sysconf(_SC_PAGESIZE);
  return page > 0 ? (size_t)page : (size_t)4096;
}

GCU_Fiber_Result gcu_fiber_create(GCU_Fiber ** out_fiber,
  GCU_Fiber_Entry entry, void * arg, size_t stack_size,
  const GCU_Allocator * allocator) {
  if (out_fiber == NULL || entry == NULL ||
      stack_size < GCU_FIBER_MIN_STACK_SIZE) {
    return GCU_FIBER_ERR_INVALID;
  }

  size_t page = gcu_fiber_page_size();
  if (stack_size > SIZE_MAX - 2 * page) {
    // No mapping that large can exist.  Say so through the OS error, as a
    // failed mmap() would.
    errno = ENOMEM;
    return GCU_FIBER_ERR_OOM;
  }
  stack_size = (stack_size + page - 1) / page * page;

  GCU_Fiber * fiber = gcu_allocator_calloc(allocator, 1, sizeof(*fiber));
  if (fiber == NULL) {
    // A caller's allocator is not required to set errno.
    errno = ENOMEM;
    return GCU_FIBER_ERR_OOM;
  }

  // The whole range is mapped inaccessible and the stack opened up above the
  // guard, so the guard is never a window between two calls.
  size_t total = stack_size + page;
  void * mapping = mmap(NULL, total, PROT_NONE,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    // The free may change errno; the header promises the mmap error.
    int saved = errno;
    gcu_allocator_free(allocator, fiber);
    errno = saved;
    return GCU_FIBER_ERR_OOM;
  }
  if (mprotect((char *)mapping + page, stack_size,
        PROT_READ | PROT_WRITE) != 0) {
    int saved = errno;
    munmap(mapping, total);
    gcu_allocator_free(allocator, fiber);
    errno = saved;
    return GCU_FIBER_ERR_OOM;
  }

  fiber->allocator = allocator;
  fiber->entry = entry;
  fiber->arg = arg;
  fiber->mapping = mapping;
  fiber->mapping_size = total;
  fiber->stack_low = (char *)mapping + page;
  fiber->stack_size = stack_size;
  fiber->state = GCU_FIBER_STATE_NEW;
  fiber->owner = pthread_self();

#ifdef GCU_FIBER_ASAN
  // The mapping may reuse addresses an earlier fiber left poisoned.  Measured
  // redundant with gcc 14's libasan, whose mmap and munmap interceptors clear
  // the shadow (removing both calls changed no test result, and a probe saw
  // the shadow clean after destroy without them), so no planted defect can
  // show it.  Kept because it costs a memset of the shadow and is what makes
  // this correct for a runtime that does not clear on munmap, or for stacks a
  // pool reuses without unmapping them.
  ASAN_UNPOISON_MEMORY_REGION(fiber->stack_low, stack_size);
#endif
#ifdef GCU_FIBER_TSAN_SWITCH
  fiber->tsan_fiber = __tsan_create_fiber(0);
#endif
#ifdef GCU_FIBER_VALGRIND
  fiber->valgrind_id = VALGRIND_STACK_REGISTER(
    fiber->stack_low, fiber->stack_low + stack_size);
#endif

  // The top of the stack is page-aligned, hence 16-byte aligned.
  fiber->sp = gcu_fiber_prepare_stack(fiber->stack_low + stack_size,
    gcu_fiber_main);
  *out_fiber = fiber;
  return GCU_FIBER_OK;
}

GCU_Fiber_Result gcu_fiber_switch_to(GCU_Fiber * fiber) {
  if (fiber == NULL) {
    return GCU_FIBER_ERR_INVALID;
  }
  if (!pthread_equal(fiber->owner, pthread_self())) {
    return GCU_FIBER_ERR_THREAD;
  }
  if (fiber->state != GCU_FIBER_STATE_NEW &&
      fiber->state != GCU_FIBER_STATE_SUSPENDED) {
    return GCU_FIBER_ERR_STATE;
  }

  GCU_Fiber * caller = gcu_fiber_running;
  if (caller != NULL) {
    caller->state = GCU_FIBER_STATE_RESUMING;
  }
  fiber->resumer = caller;
  fiber->state = GCU_FIBER_STATE_RUNNING;
  gcu_fiber_running = fiber;

#ifdef GCU_FIBER_TSAN_SWITCH
  fiber->tsan_resumer = __tsan_get_current_fiber();
  __tsan_switch_to_fiber(fiber->tsan_fiber, 0);
#endif

  void * asan_cookie = NULL;
  gcu_fiber_leave(&fiber->resumer_sp, fiber->sp, fiber->stack_low,
    fiber->stack_size, false, &asan_cookie);
  // Control is back: the fiber yielded or finished.
  gcu_fiber_arrived(asan_cookie, NULL, NULL);

  gcu_fiber_running = caller;
  if (caller != NULL) {
    caller->state = GCU_FIBER_STATE_RUNNING;
  }
  fiber->resumer = NULL;
  return GCU_FIBER_OK;
}

GCU_Fiber_Result gcu_fiber_yield(void) {
  GCU_Fiber * self = gcu_fiber_running;
  if (self == NULL) {
    return GCU_FIBER_ERR_STATE;
  }
  self->state = GCU_FIBER_STATE_SUSPENDED;

#ifdef GCU_FIBER_TSAN_SWITCH
  __tsan_switch_to_fiber(self->tsan_resumer, 0);
#endif

  void * asan_cookie = NULL;
#ifdef GCU_FIBER_ASAN_SWITCH
  gcu_fiber_leave(&self->sp, self->resumer_sp, self->resumer_bottom,
    self->resumer_stack_size, false, &asan_cookie);
  // Resumed, perhaps by a different context than last time.
  gcu_fiber_arrived(asan_cookie, &self->resumer_bottom,
    &self->resumer_stack_size);
#else
  gcu_fiber_leave(&self->sp, self->resumer_sp, NULL, 0, false, &asan_cookie);
  gcu_fiber_arrived(asan_cookie, NULL, NULL);
#endif
  return GCU_FIBER_OK;
}

GCU_Fiber_Result gcu_fiber_destroy(GCU_Fiber * fiber) {
  if (fiber == NULL) {
    return GCU_FIBER_OK;
  }
  if (!pthread_equal(fiber->owner, pthread_self())) {
    return GCU_FIBER_ERR_THREAD;
  }
  if (fiber->state == GCU_FIBER_STATE_RUNNING ||
      fiber->state == GCU_FIBER_STATE_RESUMING) {
    return GCU_FIBER_ERR_STATE;
  }

#ifdef GCU_FIBER_VALGRIND
  VALGRIND_STACK_DEREGISTER(fiber->valgrind_id);
#endif
#ifdef GCU_FIBER_TSAN_SWITCH
  __tsan_destroy_fiber(fiber->tsan_fiber);
#endif
#ifdef GCU_FIBER_ASAN
  // A suspended fiber's frames leave redzones poisoned that nothing will
  // clear; the next mapping at this address must not inherit them.
  ASAN_UNPOISON_MEMORY_REGION(fiber->stack_low, fiber->stack_size);
#endif
  munmap(fiber->mapping, fiber->mapping_size);
  gcu_allocator_free(fiber->allocator, fiber);
  return GCU_FIBER_OK;
}

GCU_Fiber * gcu_fiber_current(void) {
  return gcu_fiber_running;
}

bool gcu_fiber_is_finished(const GCU_Fiber * fiber) {
  return fiber == NULL || fiber->state == GCU_FIBER_STATE_FINISHED;
}

#else
//==========================================================================
// No implementation for this architecture.  Everything links and refuses.
//==========================================================================

GCU_Fiber_Result gcu_fiber_create(GCU_Fiber ** out_fiber,
  GCU_Fiber_Entry entry, void * arg, size_t stack_size,
  const GCU_Allocator * allocator) {
  (void)out_fiber;
  (void)entry;
  (void)arg;
  (void)stack_size;
  (void)allocator;
  return GCU_FIBER_ERR_UNSUPPORTED;
}

GCU_Fiber_Result gcu_fiber_switch_to(GCU_Fiber * fiber) {
  (void)fiber;
  return GCU_FIBER_ERR_UNSUPPORTED;
}

GCU_Fiber_Result gcu_fiber_yield(void) {
  return GCU_FIBER_ERR_UNSUPPORTED;
}

GCU_Fiber_Result gcu_fiber_destroy(GCU_Fiber * fiber) {
  // As in the real arms, destroying nothing is not an error.
  return fiber == NULL ? GCU_FIBER_OK : GCU_FIBER_ERR_UNSUPPORTED;
}

GCU_Fiber * gcu_fiber_current(void) {
  return NULL;
}

bool gcu_fiber_is_finished(const GCU_Fiber * fiber) {
  (void)fiber;
  return true;
}

#endif
