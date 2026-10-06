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
 * A completion-shaped event loop: start a read, a write, an accept, a
 * connect, a datagram send or receive, or a timer, and learn when it has
 * completed.
 *
 * This is the mechanism and nothing else.  Which fiber runs next, how work
 * is shared out and how long a request may take are policy, and belong to
 * whoever builds a scheduler on this.
 *
 * The design and the reasoning behind each decision are recorded in
 * `documentation/loop.md`.
 *
 * ## Completion, not readiness
 *
 * Linux (`epoll`) reports that a socket is *ready*; Windows (I/O completion
 * ports) reports that an operation has *finished*.  A completion interface
 * can be built on readiness by doing the call when readiness is reported, and
 * a readiness interface cannot be built on completion without undocumented
 * calls.  So the only thing a caller ever learns is "this operation is done,
 * and here is what it moved".  The Linux arm attempts each call at once and
 * arms `epoll` only when the OS says it would block.  kqueue is intended
 * and not written: macOS and the BSDs link, and every call that would create
 * a loop returns ::GCU_LOOP_ERR_UNSUPPORTED (as fibers do for macOS).
 *
 * ## Operations are caller-owned records
 *
 * An operation is a ::GCU_Loop_Op that the caller provides and keeps alive.
 * Nothing is allocated per operation, and the buffer rule is visible in the
 * type: **the loop owns the operation record and the buffer it names from the
 * moment a start call returns ::GCU_LOOP_OK until its completion callback is
 * entered.**  Cancelling does not end that earlier.  gcu_loop_cancel()
 * reports through the completion, `GCU_LOOP_CANCELLED`, and only then may
 * the buffer be freed.  A caller that frees a buffer because it asked for a
 * cancel has freed it under an operation the OS may still be writing to.
 *
 * A completion callback is only ever entered from gcu_loop_run() or
 * gcu_loop_run_once(), on the loop's thread, never from inside a start call
 * or gcu_loop_cancel(), even for an operation that finished at once.  The
 * record is idle again when the callback is entered and may be started again
 * from inside it.
 *
 * ## Results
 *
 * Every completed operation reports through the one ::GCU_Loop_Completion in
 * its record: how many bytes moved, or why it failed.  `EAGAIN` and
 * `WSAEWOULDBLOCK` never reach the caller.  A failure of the OS call is a
 * completion with status ::GCU_LOOP_ERR_OS and the OS code in `os_error`
 * (readable with gcu_error_string() and sortable with
 * gcu_socket_error_kind()); a call that cannot be started returns its result
 * from the start function and has no completion.
 *
 * | operation | completes with |
 * | --- | --- |
 * | read | `bytes` > 0 as soon as any data arrived; 0 is end of stream, except that a read into a zero-length buffer completes at once with 0 and says nothing about the stream |
 * | write | all of the buffer, or the error with `bytes` already moved |
 * | accept | `accepted` and `peer`; close it with gcu_socket_close() |
 * | connect | OK once connected |
 * | send to | the datagram sent, `bytes` of it |
 * | receive from | one datagram, `bytes` of it and the sender in `peer` |
 * | timer | OK once the delay has passed, never before |
 *
 * ## Threads
 *
 * **A loop is used by one thread**, the one that created it.  Every call
 * except gcu_loop_post() and gcu_loop_wake() made from another thread is
 * refused with ::GCU_LOOP_ERR_THREAD and changes nothing.  Work from
 * another thread reaches a loop only by posting, which is how an offload
 * pool hands a result back: post a record whose callback does the rest on
 * the loop's thread.  Calling post or wake while the loop is being destroyed
 * is the caller's to prevent; a loop that has begun to be destroyed refuses
 * a post with ::GCU_LOOP_ERR_STATE.
 *
 * ## Timers
 *
 * One-shot, in integer milliseconds on a monotonic clock, started from the
 * moment of the call.  A timer never fires early; how late it fires depends
 * on what else the loop is doing.  There is no periodic timer: start another
 * from the callback.
 *
 * ## Fibers
 *
 * gcu_loop_fiber_wait() suspends the running fiber until an operation
 * completes, and the loop resumes it on its own thread (fibers never change
 * thread) when the completion is delivered.  The loop switches to the fiber
 * from inside its iteration, so the fiber runs until it waits again or
 * finishes and then control returns to the loop.  The helper does not decide
 * which fiber runs next in any other sense; a scheduler that wants to keep
 * its own run queue starts operations with a callback instead and resumes the
 * fiber itself.
 *
 * ## Destroying a loop
 *
 * gcu_loop_destroy() cancels every operation in flight and every posted
 * record that has not run, delivers each one's completion with
 * `GCU_LOOP_CANCELLED`, and only then returns.  Callbacks run during that
 * drain may start no new operation (they get ::GCU_LOOP_ERR_STATE).  Sockets
 * that were used with the loop outlive it but may not be used with another
 * loop.
 *
 * ## Platforms
 *
 * `epoll` on Linux, an I/O completion port (with `AcceptEx` and `ConnectEx`)
 * on Windows.  Elsewhere ::GCU_LOOP_SUPPORTED is 0.
 */

#ifndef GHOTI_IO_GCU_LOOP_H
#define GHOTI_IO_GCU_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 1 when this build has a working loop, 0 when it does not.
 *
 * Test it, rather than the platform, to decide whether a caller can use the
 * loop.  Defining `GCU_LOOP_FORCE_UNSUPPORTED` when compiling the library
 * selects the unsupported arm anywhere, which is how that arm is tested.
 */
#if defined(GCU_LOOP_FORCE_UNSUPPORTED)
#define GCU_LOOP_SUPPORTED 0
#elif defined(_WIN32) || defined(__linux__)
#define GCU_LOOP_SUPPORTED 1
#else
#define GCU_LOOP_SUPPORTED 0
#endif

/**
 * The outcome of a loop call, and the status of a completed operation.
 */
typedef enum GCU_Loop_Result {
  GCU_LOOP_OK = 0,          ///< Succeeded.
  GCU_LOOP_ERR_INVALID,     ///< A bad argument: NULL, or a record never initialised.
  GCU_LOOP_ERR_OOM,         ///< No memory.
  GCU_LOOP_ERR_UNSUPPORTED, ///< This build has no loop.
  GCU_LOOP_ERR_THREAD,      ///< Called from a thread that is not the owner.
  GCU_LOOP_ERR_STATE,       ///< Not valid now: busy, already completed, closed.
  GCU_LOOP_ERR_OS,          ///< The OS refused; the code is in the error API
                            ///<   (a start call) or in `os_error` (a completion).
  GCU_LOOP_CANCELLED,       ///< A completion only: the operation was cancelled.
  GCU_LOOP_RESULT_COUNT,    ///< The number of results; not itself a result.
} GCU_Loop_Result;

/**
 * A loop.  Opaque.
 */
typedef struct GCU_Loop GCU_Loop;

/**
 * An operation record.  See ::GCU_Loop_Op.
 */
typedef struct GCU_Loop_Op GCU_Loop_Op;

/**
 * What an operation reports when it completes.
 */
typedef struct GCU_Loop_Completion {
  size_t size;               ///< `sizeof(GCU_Loop_Completion)`.
  GCU_Loop_Result status;    ///< ::GCU_LOOP_OK, ::GCU_LOOP_CANCELLED,
                             ///<   ::GCU_LOOP_ERR_OS or ::GCU_LOOP_ERR_OOM.
  size_t bytes;              ///< Bytes moved; also on a failed write.
  int os_error;              ///< With ::GCU_LOOP_ERR_OS, the OS code.
  GCU_Socket * accepted;     ///< An accept's new socket, else NULL.
  GCU_Socket_Address peer;   ///< An accept's or receive's peer; else zeros.
} GCU_Loop_Completion;

/**
 * Called, on the loop's thread, when an operation has completed.
 *
 * The record is idle and its `result` is filled in.  It may be started again
 * from here, and the socket it named may be closed.  The loop must not be
 * destroyed from inside a callback.
 *
 * @param op The record, now complete.
 * @param user_data The pointer set when the record was initialised.
 */
typedef void (*GCU_Loop_Callback)(GCU_Loop_Op * op, void * user_data);

/**
 * An operation record: one outstanding read, write, accept, connect,
 * datagram call, timer or posted callback.
 *
 * Initialise it with gcu_loop_op_init(), start it with one of the calls
 * below, and do not touch it, its buffer or its socket's lifetime until its
 * completion arrives (see the file comment).  It may be reused once idle,
 * and it may be embedded in a larger structure of the caller's: the layout
 * is published for that, and **every member after `result` is private.**
 *
 * It begins with `size`, so a later library can tell a record built against
 * an older layout and refuse it.
 */
struct GCU_Loop_Op {
  size_t size;                  ///< `sizeof(GCU_Loop_Op)`; set by init.
  GCU_Loop_Callback callback;   ///< Called on completion; may be NULL.
  void * user_data;             ///< Passed to the callback.
  GCU_Loop_Completion result;   ///< Valid in the callback and after.

  /// @cond HIDDEN_SYMBOLS
  struct GCU_Loop * priv_loop;  ///< Private.
  GCU_Socket * priv_socket;     ///< Private.
  void * priv_buffer;           ///< Private.
  size_t priv_length;           ///< Private.
  size_t priv_done;             ///< Private.
  GCU_Socket_Address priv_address; ///< Private.
  GCU_Loop_Op * priv_q_next;    ///< Private.
  GCU_Loop_Op * priv_q_prev;    ///< Private.
  GCU_Loop_Op * priv_all_next;  ///< Private.
  GCU_Loop_Op * priv_all_prev;  ///< Private.
  GCU_Loop_Op * priv_ready_next;///< Private.
  GCU_Fiber * priv_waiter;      ///< Private.
  GCU_Socket * priv_pending_socket; ///< Private.  Windows accept.
  int64_t priv_deadline;        ///< Private.
  uint64_t priv_seq;            ///< Private.
  size_t priv_heap_index;       ///< Private.
  uint32_t priv_generation;     ///< Private.
  int priv_aux_length;          ///< Private.
  uint8_t priv_kind;            ///< Private.
  uint8_t priv_state;           ///< Private.
  uint8_t priv_flags;           ///< Private.
  uint64_t priv_ov[4];          ///< Private.  Room for an OVERLAPPED.
  uint8_t priv_aux[96];         ///< Private.  Address and accept buffers.
  /// @endcond
};

/**
 * Prepare a record for use.  Call it once before the first start; after
 * that a completed record may simply be started again.
 *
 * @param op The record.  Must not be in flight.
 * @param callback Called when the operation completes, or `NULL` when the
 *   caller waits with gcu_loop_fiber_wait().
 * @param user_data Passed to the callback.
 */
GCU_API void gcu_loop_op_init(
  GCU_Loop_Op * op, GCU_Loop_Callback callback, void * user_data);

/**
 * Whether a record is in flight: started, and its callback not yet entered.
 *
 * @param op The record.
 * @return `true` while the loop holds it; `false` for NULL.  Owner-thread
 *   only: the answer changes only on the loop's thread, so another thread
 *   reading it races that thread (a posted record's state is not a thing to
 *   poll; wait for its callback).
 */
GCU_API bool gcu_loop_op_is_pending(const GCU_Loop_Op * op);

/**
 * Create a loop bound to the calling thread.
 *
 * @param out_loop Receives the loop; written only on success.  Destroy it
 *   on the same thread with gcu_loop_destroy().
 * @param allocator Allocator for the loop and its timer queue, or `NULL` for
 *   the default.  It must outlive the loop.  It is also used for the
 *   descriptor of an accepted socket.
 * @return ::GCU_LOOP_OK; ::GCU_LOOP_ERR_INVALID for NULL;
 *   ::GCU_LOOP_ERR_OOM; ::GCU_LOOP_ERR_OS; or ::GCU_LOOP_ERR_UNSUPPORTED.
 */
GCU_API GCU_Loop_Result gcu_loop_create(
  GCU_Loop ** out_loop, const GCU_Allocator * allocator);

/**
 * Cancel everything in flight, deliver each completion, and free the loop.
 *
 * See the file comment.  Not callable from inside a callback.
 *
 * @param loop The loop, or `NULL`, which is ignored.
 * @return ::GCU_LOOP_OK; ::GCU_LOOP_ERR_THREAD from another thread, with
 *   the loop untouched; or ::GCU_LOOP_ERR_STATE while the loop is running,
 *   and on Windows when the drain gives up: an operation the OS will not
 *   let go of is still its, so after thirty one-second waits destroy frees
 *   nothing and returns ::GCU_LOOP_ERR_STATE.  The loop is then valid, what
 *   was delivered stays delivered, and destroy may be called again.
 */
GCU_API GCU_Loop_Result gcu_loop_destroy(GCU_Loop * loop);

/**
 * Run the loop until gcu_loop_stop() is called.
 *
 * @param loop The loop.
 * @return ::GCU_LOOP_OK once stopped; ::GCU_LOOP_ERR_THREAD;
 *   ::GCU_LOOP_ERR_STATE if it is already running (called from a callback);
 *   or ::GCU_LOOP_ERR_OS.
 */
GCU_API GCU_Loop_Result gcu_loop_run(GCU_Loop * loop);

/**
 * Wait for something to happen, deliver what has completed, and return.
 *
 * @param loop The loop.
 * @param timeout_ms The longest to wait, in milliseconds: 0 delivers only
 *   what is already ready, a negative value waits until something happens.
 *   A timer, a completion, a post or a wake ends the wait early.
 * @return As gcu_loop_run().
 */
GCU_API GCU_Loop_Result gcu_loop_run_once(GCU_Loop * loop, long timeout_ms);

/**
 * Ask a running loop to return from gcu_loop_run() after the current
 * iteration.  Called from a callback, or before run (which then returns at
 * once).  From another thread, post a record whose callback calls this.
 *
 * @param loop The loop.
 * @return ::GCU_LOOP_OK, ::GCU_LOOP_ERR_INVALID or ::GCU_LOOP_ERR_THREAD.
 */
GCU_API GCU_Loop_Result gcu_loop_stop(GCU_Loop * loop);

/**
 * How many operations are in flight: started, callback not yet entered.
 * Posted records count.
 *
 * @param loop The loop.
 * @return The count, or 0 for NULL.  Owner-thread only: from any other
 *   thread it returns 0 whatever is in flight, which is not an answer.
 */
GCU_API size_t gcu_loop_pending(const GCU_Loop * loop);

/**
 * Read from a stream socket.  Completes with whatever has arrived, at least
 * one byte, or with 0 bytes at end of stream.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param socket A connected stream socket, not yet used with another loop.
 * @param buffer Where to put the data.  Held until the completion.
 * @param length Its size.
 * @return ::GCU_LOOP_OK when started (the completion will come);
 *   ::GCU_LOOP_ERR_INVALID; ::GCU_LOOP_ERR_THREAD; ::GCU_LOOP_ERR_STATE for a
 *   record in flight, a socket of the wrong type, closed, blocking or bound
 *   to another loop, or a loop being destroyed; or ::GCU_LOOP_ERR_OS when
 *   the OS refused to watch the socket.
 */
GCU_API GCU_Loop_Result gcu_loop_read(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, void * buffer, size_t length);

/**
 * Write to a stream socket.  Completes when all of the buffer has been
 * accepted by the OS, or on an error with the bytes already moved.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param socket A connected stream socket.
 * @param buffer The data.  Held, and not modified by the loop, until the
 *   completion.
 * @param length Its size.
 * @return As gcu_loop_read().
 */
GCU_API GCU_Loop_Result gcu_loop_write(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const void * buffer, size_t length);

/**
 * Accept one connection on a listening socket.
 *
 * The new socket is allocated with the loop's allocator, is non-blocking,
 * and arrives in the completion's `accepted`; the caller owns it.  The memory
 * for it is taken before a connection is: a failure to allocate completes
 * with ::GCU_LOOP_ERR_OOM and leaves the connection in the listener's
 * backlog, for the next accept to take.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param listener A listening stream socket.
 * @return As gcu_loop_read().
 */
GCU_API GCU_Loop_Result gcu_loop_accept(
  GCU_Loop * loop, GCU_Loop_Op * op, GCU_Socket * listener);

/**
 * Connect a stream socket.  A socket is connected at most once.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param socket An unconnected stream socket.
 * @param address Where to connect; copied.
 * @return As gcu_loop_read(); ::GCU_LOOP_ERR_INVALID also for an address of
 *   the wrong family.  A refusal by the peer is a completion with
 *   ::GCU_LOOP_ERR_OS, not a return value.
 */
GCU_API GCU_Loop_Result gcu_loop_connect(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const GCU_Socket_Address * address);

/**
 * Send one datagram.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param socket A datagram socket.
 * @param buffer The datagram.  Held until the completion.
 * @param length Its size.
 * @param address The destination; copied.
 * @return As gcu_loop_connect().
 */
GCU_API GCU_Loop_Result gcu_loop_sendto(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const void * buffer, size_t length,
  const GCU_Socket_Address * address);

/**
 * Receive one datagram.  One that is longer than @p length is cut to it,
 * silently: the completion has no truncation flag, `bytes` is the buffer's
 * size, and the rest of the datagram is gone.  Size the buffer for the largest
 * datagram the protocol allows.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param socket A bound datagram socket.
 * @param buffer Where to put the datagram.  Held until the completion.
 * @param length Its size.
 * @return As gcu_loop_read().  The sender is in the completion's `peer`.
 */
GCU_API GCU_Loop_Result gcu_loop_recvfrom(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, void * buffer, size_t length);

/**
 * Start a one-shot timer.
 *
 * @param loop The loop.
 * @param op An initialised, idle record.
 * @param delay_ms Milliseconds from now, on a monotonic clock.  0 fires on
 *   the next iteration.
 * @return ::GCU_LOOP_OK; ::GCU_LOOP_ERR_INVALID; ::GCU_LOOP_ERR_THREAD;
 *   ::GCU_LOOP_ERR_STATE; or ::GCU_LOOP_ERR_OOM when the timer queue cannot
 *   grow.
 */
GCU_API GCU_Loop_Result gcu_loop_timer_start(
  GCU_Loop * loop, GCU_Loop_Op * op, unsigned long delay_ms);

/**
 * Cancel an operation in flight.
 *
 * Returns at once and changes nothing the caller can see except that the
 * completion, with `GCU_LOOP_CANCELLED`, is now certain to arrive from the
 * loop.  **The operation's buffer is still the loop's until that completion
 * arrives**; free it in the callback, not after this call returns.  An
 * operation whose result is already decided (it completed, and only its
 * callback has yet to run) is not cancelled and keeps its result; one that
 * never started, or whose callback has been entered, is not in flight.
 * Cancelling a connect that is in progress on Linux leaves the socket
 * half-connected (the OS goes on with it) and not usable for another connect
 * or for data; close it.  A cancelled accept, read, write or datagram call
 * leaves its socket as it was.  On
 * Windows an operation the OS finished before the cancel, whose packet the
 * loop has not collected yet, is treated the same way: `GCU_LOOP_ERR_STATE`,
 * and the completion carries its real result.  A second cancel of an
 * operation already being cancelled is also `GCU_LOOP_ERR_STATE`.
 *
 * @param loop The loop.
 * @param op The record.
 * @return ::GCU_LOOP_OK when the cancellation was taken;
 *   ::GCU_LOOP_ERR_STATE when the operation is not in flight or its result
 *   is already decided; ::GCU_LOOP_ERR_INVALID for NULL or a posted
 *   record (a post cannot be cancelled by anyone but destroy);
 *   ::GCU_LOOP_ERR_THREAD.
 */
GCU_API GCU_Loop_Result gcu_loop_cancel(GCU_Loop * loop, GCU_Loop_Op * op);

/**
 * Cancel a timer.  gcu_loop_cancel() for a record that is a timer, and
 * ::GCU_LOOP_ERR_INVALID for one that is not.
 *
 * @param loop The loop.
 * @param op The timer's record.
 * @return As gcu_loop_cancel().
 */
GCU_API GCU_Loop_Result gcu_loop_timer_cancel(
  GCU_Loop * loop, GCU_Loop_Op * op);

/**
 * Queue a record for the loop to run its callback.  **The one call, with
 * gcu_loop_wake(), that any thread may make.**
 *
 * The record is the caller's and is the loop's until its callback is
 * entered, as for any operation; its `result.status` is ::GCU_LOOP_OK, or
 * ::GCU_LOOP_CANCELLED when the loop was destroyed first.  A waiting loop
 * is woken.  Nothing is allocated, so this cannot fail for lack of memory,
 * and callbacks run in the order they were posted.
 *
 * @param loop The loop.
 * @param op An initialised, idle record with a callback.
 * @return ::GCU_LOOP_OK; ::GCU_LOOP_ERR_INVALID for NULL, an uninitialised
 *   record or no callback; ::GCU_LOOP_ERR_STATE when the record is in flight
 *   or the loop is being destroyed; or ::GCU_LOOP_ERR_OS when the wake
 *   failed, in which case the record was not posted: it is idle again and no
 *   callback will be entered for it.
 */
GCU_API GCU_Loop_Result gcu_loop_post(GCU_Loop * loop, GCU_Loop_Op * op);

/**
 * Make a waiting loop return from its wait.  Callable from any thread.  A
 * wake with nothing to deliver makes gcu_loop_run_once() return early and
 * gcu_loop_run() go round again.
 *
 * @param loop The loop.
 * @return ::GCU_LOOP_OK; ::GCU_LOOP_ERR_INVALID; ::GCU_LOOP_ERR_STATE when
 *   the loop is being destroyed; or ::GCU_LOOP_ERR_OS.
 */
GCU_API GCU_Loop_Result gcu_loop_wake(GCU_Loop * loop);

/**
 * Suspend the running fiber until an operation completes.
 *
 * Start the operation first, then call this on the fiber that wants the
 * answer; it returns when the completion has been delivered, and the answer
 * is in `op->result`.  The loop resumes the fiber from inside its iteration
 * (see the file comment).  A record's callback, if it has one, runs first.
 * One fiber waits on a record at a time.  **The record must stay valid until
 * this call returns**, not only until its callback is entered: the resumed
 * fiber reads `op->result` afterwards, so a callback that frees the record
 * leaves the waiter reading freed memory.
 *
 * @param op A record in flight.
 * @return ::GCU_LOOP_OK once completed; ::GCU_LOOP_ERR_INVALID for NULL;
 *   ::GCU_LOOP_ERR_STATE when not on a fiber, when the record is not in
 *   flight or is a posted record, or when another fiber is already waiting
 *   on it; ::GCU_LOOP_ERR_THREAD from a thread other than the loop's.
 */
GCU_API GCU_Loop_Result gcu_loop_fiber_wait(GCU_Loop_Op * op);

/**
 * A short name for a result, for messages.
 *
 * @param result Any value; one out of range gets a placeholder.
 * @return A string with static storage.
 */
GCU_API const char * gcu_loop_result_string(GCU_Loop_Result result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_LOOP_H
