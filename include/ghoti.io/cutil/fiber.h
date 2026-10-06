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
 * Fibers: stackful coroutines.  Each one runs on a stack of its own, and
 * control passes between it and its resumer only at calls that say so.
 *
 * This is the mechanism and nothing else.  Which fiber runs next, for how
 * long, and what a fiber waits for are policy, and belong to whoever
 * schedules them.
 *
 * The design and the reasoning behind each decision are recorded in
 * `documentation/fiber.md`.
 *
 * ## The model
 *
 * A fiber is *resumed* by gcu_fiber_switch_to() and gives control back with
 * gcu_fiber_yield(), which returns to whoever resumed it.  A fiber may itself
 * resume another, so resumers form a chain, and each yield goes one step back
 * along it.  A fiber is finished when its entry function returns; control
 * then passes to its resumer exactly as for a yield, and the fiber can only
 * be destroyed.
 *
 * ## A fiber never changes thread
 *
 * **A fiber belongs to the thread that created it, for its whole life.**
 * Creating it, resuming it and destroying it must all happen on that thread;
 * a call from any other thread is refused with ::GCU_FIBER_ERR_THREAD and
 * leaves the fiber untouched.  Do not rely on a fiber being movable between
 * threads: its stack holds pointers into thread-local state, sanitizer
 * bookkeeping is per thread, and on Windows the Fiber API itself is bound to
 * a thread.  A scheduler that wants fibers on several threads makes them on
 * the thread that will run them.
 *
 * A consequence to plan for: **if the owner thread exits while a fiber it
 * made is still alive, nobody can destroy that fiber** (every other thread is
 * refused), and its stack mapping and descriptor are leaked.  Finish or
 * destroy a thread's fibers before the thread ends.
 *
 * ## Floating-point state
 *
 * Each fiber has its own rounding mode and exception masks.  The switch saves
 * and restores the callee-saved registers and the floating-point control
 * state (on x86-64 the MXCSR control bits and the x87 control word; on arm64
 * FPCR), so a fiber that calls `fesetround()` does not change the mode seen
 * by the fiber that resumed it, or by any other.  The exception *status*
 * flags are not switched: they accumulate on the thread, as they do across a
 * function call.  A new fiber starts in the default environment (round to
 * nearest, no unmasked exceptions) on x86-64 and arm64, whatever mode its
 * creator was in, on Windows x86-64 as well.  (Windows on arm64 relies on the
 * Fiber API's own switching, which has not been measured.)
 *
 * ## Destroying a suspended fiber
 *
 * A fiber that has started and not finished can be destroyed.  Its stack is
 * released as it stands: **nothing on it is unwound.**  Destructors do not
 * run, cleanup the entry function would have done after its last yield is
 * not done, and any lock or resource the fiber held is still held.  Finish a
 * fiber, or make sure it holds nothing, before abandoning it.
 *
 * ## Stacks
 *
 * Each stack is mapped separately with a guard page below it.  A fiber that
 * overflows its stack faults on the guard page (SIGSEGV on POSIX) instead of
 * writing into whatever lies below.  The size is chosen per fiber, rounded up
 * to a whole number of pages, and may not be less than
 * ::GCU_FIBER_MIN_STACK_SIZE.  The stack is not zeroed on creation or on
 * destruction.
 *
 * ## Platforms
 *
 * Hand-written switches exist for x86-64 System V and for arm64 on ELF
 * platforms (Linux, the BSDs).  Windows uses the Fiber API.  On any other
 * target, macOS included, ::GCU_FIBER_SUPPORTED is 0 and every call that
 * would create or switch returns ::GCU_FIBER_ERR_UNSUPPORTED.  (destroy(NULL)
 * is still ignored and returns ::GCU_FIBER_OK.)
 *
 * On the hand-written (ELF) targets AddressSanitizer and ThreadSanitizer are
 * told about every stack switch, and Valgrind about every stack, so the
 * sanitizer gates see through this library rather than stopping at it.  The
 * Windows arm annotates nothing: it has no sanitizer gate.
 */

#ifndef GHOTI_IO_GCU_FIBER_H
#define GHOTI_IO_GCU_FIBER_H

#include <stdbool.h>
#include <stddef.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 1 when this build has a working fiber implementation, 0 when it does not.
 *
 * Test it, rather than the architecture, to decide whether a caller can use
 * fibers at all.
 */
#if defined(_WIN32)
#define GCU_FIBER_SUPPORTED 1
#elif defined(__ELF__) && (defined(__x86_64__) || defined(__aarch64__))
#define GCU_FIBER_SUPPORTED 1
#else
#define GCU_FIBER_SUPPORTED 0
#endif

/**
 * The smallest stack gcu_fiber_create() accepts, in bytes.
 *
 * Large enough for the entry function's own frame, the library's trampoline
 * and the frames a sanitizer inserts.  It is a floor on the call, not advice:
 * a fiber that calls anything substantial needs far more.
 */
#define GCU_FIBER_MIN_STACK_SIZE ((size_t)16384)

/**
 * A stack size that is enough for ordinary code: 256 KiB.
 */
#define GCU_FIBER_DEFAULT_STACK_SIZE ((size_t)262144)

/**
 * The outcome of a fiber call.
 */
typedef enum GCU_Fiber_Result {
  GCU_FIBER_OK = 0,          ///< Succeeded.
  GCU_FIBER_ERR_INVALID,     ///< A bad argument: NULL, or a stack too small.
  GCU_FIBER_ERR_OOM,         ///< No memory for the fiber or its stack.
  GCU_FIBER_ERR_UNSUPPORTED, ///< This build has no fiber implementation.
  GCU_FIBER_ERR_THREAD,      ///< Called from a thread that is not the owner.
  GCU_FIBER_ERR_STATE,       ///< Not valid now: finished, running, or no fiber.
  GCU_FIBER_RESULT_COUNT,    ///< The number of results; not itself a result.
} GCU_Fiber_Result;

/**
 * A fiber.  Opaque.
 */
typedef struct GCU_Fiber GCU_Fiber;

/**
 * The function a fiber runs.
 *
 * When it returns the fiber is finished and control passes to the fiber's
 * resumer.  It must not call `exit()`-like functions that skip a return
 * path it cares about, and it must not unwind past itself (no C++ exception
 * may leave it, and no `longjmp` may leave the fiber's stack).
 *
 * @param arg The pointer given to gcu_fiber_create().
 */
typedef void (*GCU_Fiber_Entry)(void * arg);

/**
 * Create a fiber.  It does not start running until it is resumed.
 *
 * On failure nothing is allocated and `*out_fiber` is not written.  When the
 * stack cannot be mapped the OS error is left in gcu_error_last().
 *
 * @param out_fiber Receives the fiber.  Release it with gcu_fiber_destroy()
 *   on the thread that created it.
 * @param entry What the fiber runs.
 * @param arg Passed to @p entry; borrowed, never dereferenced here.
 * @param stack_size The stack size in bytes, at least
 *   ::GCU_FIBER_MIN_STACK_SIZE.  Rounded up to whole pages.
 * @param allocator Allocator for the fiber's descriptor, or `NULL` for the
 *   default.  The stack is mapped from the OS and does not use it.
 * @return ::GCU_FIBER_OK; ::GCU_FIBER_ERR_INVALID for a NULL @p out_fiber or
 *   @p entry or a small stack; ::GCU_FIBER_ERR_OOM; or
 *   ::GCU_FIBER_ERR_UNSUPPORTED.
 */
GCU_API GCU_Fiber_Result gcu_fiber_create(GCU_Fiber ** out_fiber,
  GCU_Fiber_Entry entry, void * arg, size_t stack_size,
  const GCU_Allocator * allocator);

/**
 * Run a fiber until it yields or finishes.
 *
 * The caller is suspended and becomes the fiber's resumer; this returns when
 * the fiber calls gcu_fiber_yield() or its entry function returns.  The
 * caller may be the thread's own context or another fiber.
 *
 * @param fiber The fiber to run.  It must be new or suspended: not finished,
 *   and not the caller or anything in the caller's chain of resumers.
 * @return ::GCU_FIBER_OK once control has come back;
 *   ::GCU_FIBER_ERR_INVALID for NULL; ::GCU_FIBER_ERR_THREAD when called from
 *   a thread other than the fiber's owner; ::GCU_FIBER_ERR_STATE when the
 *   fiber cannot be resumed; ::GCU_FIBER_ERR_UNSUPPORTED.
 */
GCU_API GCU_Fiber_Result gcu_fiber_switch_to(GCU_Fiber * fiber);

/**
 * Suspend the running fiber and return to the one that resumed it.
 *
 * Returns when something resumes this fiber again.
 *
 * @return ::GCU_FIBER_OK once resumed; ::GCU_FIBER_ERR_STATE when the caller
 *   is not running on a fiber.
 */
GCU_API GCU_Fiber_Result gcu_fiber_yield(void);

/**
 * Free a fiber and its stack.
 *
 * A suspended fiber is released without being unwound; see the file comment.
 * The fiber must not be running or be one of the running fiber's resumers.
 *
 * @param fiber The fiber, or `NULL`, which is ignored.
 * @return ::GCU_FIBER_OK; ::GCU_FIBER_ERR_THREAD when called from a thread
 *   other than the owner, with the fiber untouched; ::GCU_FIBER_ERR_STATE
 *   when the fiber is running or resuming another.
 */
GCU_API GCU_Fiber_Result gcu_fiber_destroy(GCU_Fiber * fiber);

/**
 * The fiber running on the calling thread.
 *
 * @return The fiber, or `NULL` when the thread is running its own context.
 */
GCU_API GCU_Fiber * gcu_fiber_current(void);

/**
 * Whether a fiber's entry function has returned.
 *
 * @param fiber The fiber; `NULL` counts as finished.
 * @return `true` when the fiber has finished.
 */
GCU_API bool gcu_fiber_is_finished(const GCU_Fiber * fiber);

/**
 * A short name for a result, for messages.
 *
 * @param result Any value; one out of range gets a placeholder.
 * @return A string with static storage.
 */
GCU_API const char * gcu_fiber_result_string(GCU_Fiber_Result result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_FIBER_H
