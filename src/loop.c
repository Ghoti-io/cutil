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

// accept4(), eventfd() and CLOCK_MONOTONIC are not in strict ISO C.
#ifndef _WIN32
#define _GNU_SOURCE
#endif

/**
 * @file
 *
 * The event loop.  See loop.h for the contract and `documentation/loop.md`
 * for the reasoning.
 *
 * Three implementations share this file, chosen at compile time.  The
 * bookkeeping that does not depend on the OS (the lists of operations, the
 * ready queue, the timer heap, posting, fiber waits, teardown) is written
 * once; `epoll` on Linux and an I/O completion port on Windows supply the
 * calls that start an operation, cancel it and wait for the OS.  On any other
 * target every call that would create a loop reports
 * ::GCU_LOOP_ERR_UNSUPPORTED.
 *
 * ## The life of an operation
 *
 * `IDLE` -> `PENDING` when a start call returns OK -> `READY` when its result
 * is decided -> `IDLE` when its callback is entered.  A cancel is accepted
 * only in `PENDING`.  An operation is `READY` only inside the loop, between
 * the call that decided its result and the end of that iteration's delivery,
 * which is why a callback is never entered from inside a start or a cancel.
 *
 * ## Planted defects
 *
 * The `GCU_LOOP_PLANT_*` macros each compile one deliberate defect into the
 * loop.  They exist for `make check-loop-defects` (and the Windows and arm64
 * runs in `tools/`), which build the library with one of them and require the
 * tests to fail.  No ordinary build defines one.
 *
 *   - `GCU_LOOP_PLANT_NO_WAKE`: gcu_loop_post() queues the record and does
 *     not wake the loop, so a loop waiting in the OS never learns of it.
 *   - `GCU_LOOP_PLANT_CANCEL_EARLY_RELEASE`: gcu_loop_cancel() delivers the
 *     `CANCELLED` completion at once and leaves the OS still reading into
 *     the buffer, which the caller is now free to release.
 *   - `GCU_LOOP_PLANT_TIMER_ORDER`: the timer queue is ordered by when a
 *     timer was started, not by when it is due.
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/macros.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#endif

#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/cutil/loop.h>
#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/cutil/socket.h>

#include "socket_internal.h"

#if !defined(_WIN32) && GCU_LOOP_SUPPORTED
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

static const char * const gcu_loop_result_names[GCU_LOOP_RESULT_COUNT] = {
  [GCU_LOOP_OK] = "ok",
  [GCU_LOOP_ERR_INVALID] = "invalid argument",
  [GCU_LOOP_ERR_OOM] = "out of memory",
  [GCU_LOOP_ERR_UNSUPPORTED] = "no event loop on this platform",
  [GCU_LOOP_ERR_THREAD] = "not the thread that owns the loop",
  [GCU_LOOP_ERR_STATE] = "not valid now",
  [GCU_LOOP_ERR_OS] = "the operating system refused",
  [GCU_LOOP_CANCELLED] = "cancelled",
};

const char * gcu_loop_result_string(GCU_Loop_Result result) {
  if ((unsigned)result >= (unsigned)GCU_LOOP_RESULT_COUNT) {
    return "unknown loop result";
  }
  return gcu_loop_result_names[result];
}

/**
 * What a record is doing; the values of `priv_kind`.
 */
enum {
  GCU_LOOP_KIND_NONE = 0,
  GCU_LOOP_KIND_READ,
  GCU_LOOP_KIND_WRITE,
  GCU_LOOP_KIND_ACCEPT,
  GCU_LOOP_KIND_CONNECT,
  GCU_LOOP_KIND_SENDTO,
  GCU_LOOP_KIND_RECVFROM,
  GCU_LOOP_KIND_TIMER,
  GCU_LOOP_KIND_POST,
};

/**
 * Where a record is in its life; the values of `priv_state`.
 */
enum {
  GCU_LOOP_STATE_IDLE = 0,
  GCU_LOOP_STATE_PENDING,
  GCU_LOOP_STATE_READY,
};

/**
 * The bits of `priv_flags`.
 */
enum {
  GCU_LOOP_FLAG_CANCEL_REQUESTED = 1, ///< Windows: a cancel was issued.
  GCU_LOOP_FLAG_STARTED = 2,          ///< A connect has been handed to the OS.
  GCU_LOOP_FLAG_QUEUED = 4,           ///< On its socket's wait queue.
};

void gcu_loop_op_init(
  GCU_Loop_Op * op, GCU_Loop_Callback callback, void * user_data) {
  if (!op) {
    return;
  }
  memset(op, 0, sizeof(*op));
  op->size = sizeof(*op);
  op->callback = callback;
  op->user_data = user_data;
  op->result.size = sizeof(op->result);
  op->result.peer.size = sizeof(op->result.peer);
}

bool gcu_loop_op_is_pending(const GCU_Loop_Op * op) {
  return op && op->priv_state != GCU_LOOP_STATE_IDLE;
}

#if GCU_LOOP_SUPPORTED

#ifdef _WIN32
#define GCU_LOOP_NATIVE(s) ((SOCKET)(s)->handle)
#else
#define GCU_LOOP_NATIVE(s) ((s)->fd)
#endif

/**
 * The loop.  Everything is the owner thread's except the post list and the
 * `closing` flag, which `mutex` covers.
 */
struct GCU_Loop {
  const GCU_Allocator * allocator;
#ifdef _WIN32
  DWORD owner;
  HANDLE port;
  unsigned drain_waits;           ///< Waits a destroy has spent on packets.
#else
  pthread_t owner;
  int epfd;
  int wakefd;
  GCU_Socket * zombies;           ///< Closed during an iteration.
#endif
  bool running;                   ///< Inside run_once or the destroy drain.
  bool stop;                      ///< gcu_loop_stop() was called.
  bool destroying;

  GCU_Socket * sockets;           ///< Every socket used with this loop.

  GCU_Loop_Op * all_head;         ///< Every PENDING or READY operation.
  size_t all_count;
  GCU_Loop_Op * ready_head;       ///< READY, awaiting delivery.
  GCU_Loop_Op * ready_tail;

  GCU_Loop_Op ** heap;            ///< The timers, earliest first.
  size_t heap_count;
  size_t heap_capacity;
  uint64_t seq;                   ///< Start order, to break ties.

  GCU_MUTEX_T mutex;              ///< Covers the three fields below.
  bool closing;
  GCU_Loop_Op * post_head;
  GCU_Loop_Op * post_tail;
  size_t post_count;
};

static bool gcu_loop_is_owner(const GCU_Loop * loop) {
#ifdef _WIN32
  return loop->owner == GetCurrentThreadId();
#else
  return pthread_equal(loop->owner, pthread_self()) != 0;
#endif
}

//
// The clock.  Nanoseconds, so that a timer started a fraction of a
// millisecond into one never fires a fraction of a millisecond early.
//
static int64_t gcu_loop_now(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  if (frequency.QuadPart == 0) {
    QueryPerformanceFrequency(&frequency);
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  int64_t seconds = counter.QuadPart / frequency.QuadPart;
  int64_t rest = counter.QuadPart % frequency.QuadPart;
  return seconds * 1000000000 + rest * 1000000000 / frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

//
// The lists.
//
static void gcu_loop_all_link(GCU_Loop * loop, GCU_Loop_Op * op) {
  op->priv_all_prev = NULL;
  op->priv_all_next = loop->all_head;
  if (loop->all_head) {
    loop->all_head->priv_all_prev = op;
  }
  loop->all_head = op;
  ++loop->all_count;
}

static void gcu_loop_all_unlink(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (op->priv_all_prev) {
    op->priv_all_prev->priv_all_next = op->priv_all_next;
  }
  else {
    loop->all_head = op->priv_all_next;
  }
  if (op->priv_all_next) {
    op->priv_all_next->priv_all_prev = op->priv_all_prev;
  }
  op->priv_all_next = op->priv_all_prev = NULL;
  --loop->all_count;
}

/**
 * Decide an operation's result: it is READY and will be delivered by this
 * iteration (or the next).  Never calls the callback.
 */
static void gcu_loop_ready_push(GCU_Loop * loop, GCU_Loop_Op * op) {
  op->priv_state = GCU_LOOP_STATE_READY;
  op->priv_ready_next = NULL;
  if (loop->ready_tail) {
    loop->ready_tail->priv_ready_next = op;
  }
  else {
    loop->ready_head = op;
  }
  loop->ready_tail = op;
}

static void gcu_loop_reset_result(GCU_Loop_Op * op) {
  memset(&op->result, 0, sizeof(op->result));
  op->result.size = sizeof(op->result);
  op->result.peer.size = sizeof(op->result.peer);
}

/**
 * Enter an operation's callback and resume its waiter, if any.
 *
 * The record is idle first, so the callback may start it again, and may
 * free it: nothing here touches it afterwards, which is why the waiter is
 * read before the call.
 */
static void gcu_loop_deliver(GCU_Loop * loop, GCU_Loop_Op * op) {
  GCU_Socket * socket = op->priv_socket;
  GCU_Fiber * waiter = op->priv_waiter;

  if (op->priv_kind != GCU_LOOP_KIND_POST) {
    gcu_loop_all_unlink(loop, op);
  }
  if (socket) {
    --socket->pending;
  }
  op->priv_waiter = NULL;
  op->priv_state = GCU_LOOP_STATE_IDLE;
  ++op->priv_generation;
  GCU_Loop_Callback callback = op->callback;
  void * user_data = op->user_data;
  if (callback) {
    callback(op, user_data);
  }
  if (waiter) {
    gcu_fiber_switch_to(waiter);
  }
}

/**
 * Deliver every READY operation, in the order their results were decided.
 * Operations that become ready while this runs wait for the next call.
 */
static void gcu_loop_deliver_ready(GCU_Loop * loop) {
  GCU_Loop_Op * op = loop->ready_head;
  loop->ready_head = loop->ready_tail = NULL;
  while (op) {
    GCU_Loop_Op * next = op->priv_ready_next;
    gcu_loop_deliver(loop, op);
    op = next;
  }
}

//
// The timers: a binary heap on (deadline, start order).
//
static bool gcu_loop_timer_before(const GCU_Loop_Op * a, const GCU_Loop_Op * b) {
#ifdef GCU_LOOP_PLANT_TIMER_ORDER
  return a->priv_seq < b->priv_seq;
#else
  if (a->priv_deadline != b->priv_deadline) {
    return a->priv_deadline < b->priv_deadline;
  }
  return a->priv_seq < b->priv_seq;
#endif
}

static void gcu_loop_heap_place(GCU_Loop * loop, size_t index, GCU_Loop_Op * op) {
  loop->heap[index] = op;
  op->priv_heap_index = index;
}

static void gcu_loop_heap_up(GCU_Loop * loop, size_t index) {
  GCU_Loop_Op * op = loop->heap[index];
  while (index > 0) {
    size_t parent = (index - 1) / 2;
    if (!gcu_loop_timer_before(op, loop->heap[parent])) {
      break;
    }
    gcu_loop_heap_place(loop, index, loop->heap[parent]);
    index = parent;
  }
  gcu_loop_heap_place(loop, index, op);
}

static void gcu_loop_heap_down(GCU_Loop * loop, size_t index) {
  GCU_Loop_Op * op = loop->heap[index];
  for (;;) {
    size_t child = 2 * index + 1;
    if (child >= loop->heap_count) {
      break;
    }
    if (child + 1 < loop->heap_count
        && gcu_loop_timer_before(loop->heap[child + 1], loop->heap[child])) {
      ++child;
    }
    if (!gcu_loop_timer_before(loop->heap[child], op)) {
      break;
    }
    gcu_loop_heap_place(loop, index, loop->heap[child]);
    index = child;
  }
  gcu_loop_heap_place(loop, index, op);
}

static bool gcu_loop_heap_insert(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (loop->heap_count == loop->heap_capacity) {
    size_t capacity = loop->heap_capacity ? loop->heap_capacity * 2 : 8;
    GCU_Loop_Op ** grown = gcu_allocator_realloc(
      loop->allocator, loop->heap, capacity * sizeof(*grown));
    if (!grown) {
      return false;
    }
    loop->heap = grown;
    loop->heap_capacity = capacity;
  }
  gcu_loop_heap_place(loop, loop->heap_count, op);
  ++loop->heap_count;
  gcu_loop_heap_up(loop, loop->heap_count - 1);
  return true;
}

static void gcu_loop_heap_remove(GCU_Loop * loop, GCU_Loop_Op * op) {
  size_t index = op->priv_heap_index;
  --loop->heap_count;
  if (index != loop->heap_count) {
    GCU_Loop_Op * last = loop->heap[loop->heap_count];
    gcu_loop_heap_place(loop, index, last);
    gcu_loop_heap_up(loop, index);
    gcu_loop_heap_down(loop, last->priv_heap_index);
  }
}

/**
 * Move every timer that is due to the ready queue, earliest first.
 */
static void gcu_loop_timers_fire(GCU_Loop * loop) {
  if (!loop->heap_count) {
    return;
  }
  int64_t now = gcu_loop_now();
  while (loop->heap_count && loop->heap[0]->priv_deadline <= now) {
    GCU_Loop_Op * op = loop->heap[0];
    gcu_loop_heap_remove(loop, op);
    op->result.status = GCU_LOOP_OK;
    gcu_loop_ready_push(loop, op);
  }
}

/**
 * How long the OS wait may last, in milliseconds, given the caller's limit
 * (negative for none).  Zero when something is ready; otherwise the time to
 * the earliest timer, rounded up so the timer is due when the wait returns.
 */
static long gcu_loop_wait_ms(const GCU_Loop * loop, long timeout_ms) {
  if (loop->ready_head) {
    return 0;
  }
  if (loop->heap_count) {
    int64_t delta = loop->heap[0]->priv_deadline - gcu_loop_now();
    long until = 0;
    if (delta > 0) {
      int64_t ms = (delta + 999999) / 1000000;
      until = ms > 2000000000 ? 2000000000 : (long)ms;
    }
    if (timeout_ms < 0 || until < timeout_ms) {
      return until;
    }
  }
  return timeout_ms;
}

/**
 * Post records that other threads queued join the ready queue.
 */
static void gcu_loop_posts_collect(GCU_Loop * loop, GCU_Loop_Result status) {
  GCU_MUTEX_LOCK(loop->mutex);
  GCU_Loop_Op * op = loop->post_head;
  loop->post_head = loop->post_tail = NULL;
  loop->post_count = 0;
  GCU_MUTEX_UNLOCK(loop->mutex);
  while (op) {
    GCU_Loop_Op * next = op->priv_q_next;
    op->priv_q_next = NULL;
    op->result.status = status;
    gcu_loop_ready_push(loop, op);
    op = next;
  }
}

//
// What each OS supplies.  Declared here, defined in the arm below.
//
static GCU_Loop_Result gcu_loop_plat_create(GCU_Loop * loop);
static void gcu_loop_plat_destroy(GCU_Loop * loop);
static GCU_Loop_Result gcu_loop_plat_wake(GCU_Loop * loop);
static GCU_Loop_Result gcu_loop_plat_wait(GCU_Loop * loop, long wait_ms);
static GCU_Loop_Result gcu_loop_plat_attach(GCU_Loop * loop, GCU_Socket * socket);
static GCU_Loop_Result gcu_loop_plat_start(GCU_Loop * loop, GCU_Loop_Op * op);
static bool gcu_loop_plat_cancel(GCU_Loop * loop, GCU_Loop_Op * op);
static void gcu_loop_plat_detach(GCU_Loop * loop, GCU_Socket * socket);
static void gcu_loop_plat_end_iteration(GCU_Loop * loop);
static bool gcu_loop_plat_drain(GCU_Loop * loop);

//
// The calls every platform shares.
//

GCU_Loop_Result gcu_loop_create(
  GCU_Loop ** out_loop, const GCU_Allocator * allocator) {
  if (!out_loop) {
    return GCU_LOOP_ERR_INVALID;
  }
  GCU_Loop * loop = gcu_allocator_calloc(allocator, 1, sizeof(*loop));
  if (!loop) {
    return GCU_LOOP_ERR_OOM;
  }
  loop->allocator = allocator ? allocator : gcu_allocator_default();
#ifdef _WIN32
  loop->owner = GetCurrentThreadId();
#else
  loop->owner = pthread_self();
#endif
  if (GCU_MUTEX_CREATE(loop->mutex)) {
    gcu_allocator_free(loop->allocator, loop);
    return GCU_LOOP_ERR_OS;
  }
  GCU_Loop_Result result = gcu_loop_plat_create(loop);
  if (result != GCU_LOOP_OK) {
    GCU_MUTEX_DESTROY(loop->mutex);
    gcu_allocator_free(loop->allocator, loop);
    return result;
  }
  *out_loop = loop;
  return GCU_LOOP_OK;
}

/**
 * Ask for an operation in flight to end.  Timers end here; everything else
 * is the platform's, since only it knows how the OS is told to let go.
 * Either way the result is delivered by the loop, never from this call.
 */
static bool gcu_loop_cancel_pending(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (op->priv_kind == GCU_LOOP_KIND_TIMER) {
    gcu_loop_heap_remove(loop, op);
    op->result.status = GCU_LOOP_CANCELLED;
    gcu_loop_ready_push(loop, op);
    return true;
  }
  return gcu_loop_plat_cancel(loop, op);
}

GCU_Loop_Result gcu_loop_destroy(GCU_Loop * loop) {
  if (!loop) {
    return GCU_LOOP_OK;
  }
  if (!gcu_loop_is_owner(loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  if (loop->running || loop->destroying) {
    return GCU_LOOP_ERR_STATE;
  }
  loop->destroying = true;
  loop->running = true;

  // From here a post is refused, so nothing new reaches the list; what is on
  // it was accepted and is owed a completion.
  GCU_MUTEX_LOCK(loop->mutex);
  loop->closing = true;
  GCU_MUTEX_UNLOCK(loop->mutex);
  gcu_loop_posts_collect(loop, GCU_LOOP_CANCELLED);

  for (GCU_Loop_Op * op = loop->all_head; op;) {
    GCU_Loop_Op * next = op->priv_all_next;
    if (op->priv_state == GCU_LOOP_STATE_PENDING) {
      // A cancel that loses to the OS completing the operation leaves it
      // pending for the packet that carries the real result.
      (void)gcu_loop_cancel_pending(loop, op);
    }
    op = next;
  }

  // Deliver until nothing is left.  A callback cannot start anything new, so
  // this ends; on Windows the cancelled operations are still the OS's until
  // their packets arrive, and the platform waits for them.
  for (;;) {
    gcu_loop_deliver_ready(loop);
    if (loop->ready_head) {
      continue;
    }
    if (!loop->all_count || !gcu_loop_plat_drain(loop)) {
      break;
    }
  }

  if (loop->all_count) {
    // The drain gave up (Windows only: an operation the OS would not let go
    // of).  Its buffer may still be written, so nothing is freed.  The loop
    // is as it was, minus what was delivered, and destroy can be tried again.
    GCU_MUTEX_LOCK(loop->mutex);
    loop->closing = false;
    GCU_MUTEX_UNLOCK(loop->mutex);
    loop->destroying = false;
    loop->running = false;
    return GCU_LOOP_ERR_STATE;
  }

  while (loop->sockets) {
    GCU_Socket * socket = loop->sockets;
    loop->sockets = socket->loop_next;
    gcu_loop_plat_detach(loop, socket);
    socket->loop = NULL;
    socket->detached = true;
    socket->loop_next = socket->loop_prev = NULL;
    socket->rq_head = socket->rq_tail = socket->wq_head = socket->wq_tail = NULL;
  }
  gcu_loop_plat_end_iteration(loop);
  gcu_loop_plat_destroy(loop);
  GCU_MUTEX_DESTROY(loop->mutex);
  gcu_allocator_free(loop->allocator, loop->heap);
  gcu_allocator_free(loop->allocator, loop);
  return GCU_LOOP_OK;
}

GCU_Loop_Result gcu_loop_run_once(GCU_Loop * loop, long timeout_ms) {
  if (!loop) {
    return GCU_LOOP_ERR_INVALID;
  }
  if (!gcu_loop_is_owner(loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  if (loop->running || loop->destroying) {
    return GCU_LOOP_ERR_STATE;
  }
  loop->running = true;
  GCU_Loop_Result result =
    gcu_loop_plat_wait(loop, gcu_loop_wait_ms(loop, timeout_ms));
  gcu_loop_posts_collect(loop, GCU_LOOP_OK);
  gcu_loop_timers_fire(loop);
  gcu_loop_deliver_ready(loop);
  gcu_loop_plat_end_iteration(loop);
  loop->running = false;
  return result;
}

GCU_Loop_Result gcu_loop_run(GCU_Loop * loop) {
  if (!loop) {
    return GCU_LOOP_ERR_INVALID;
  }
  if (!gcu_loop_is_owner(loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  if (loop->running || loop->destroying) {
    return GCU_LOOP_ERR_STATE;
  }
  for (;;) {
    if (loop->stop) {
      loop->stop = false;
      return GCU_LOOP_OK;
    }
    GCU_Loop_Result result = gcu_loop_run_once(loop, -1);
    if (result != GCU_LOOP_OK) {
      return result;
    }
  }
}

GCU_Loop_Result gcu_loop_stop(GCU_Loop * loop) {
  if (!loop) {
    return GCU_LOOP_ERR_INVALID;
  }
  if (!gcu_loop_is_owner(loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  loop->stop = true;
  return GCU_LOOP_OK;
}

size_t gcu_loop_pending(const GCU_Loop * loop) {
  if (!loop || !gcu_loop_is_owner(loop)) {
    return 0;
  }
  GCU_Loop * mutable_loop = (GCU_Loop *)loop;
  GCU_MUTEX_LOCK(mutable_loop->mutex);
  size_t posted = loop->post_count;
  GCU_MUTEX_UNLOCK(mutable_loop->mutex);
  return loop->all_count + posted;
}

//
// Starting an operation.
//

static GCU_Loop_Result gcu_loop_check(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (!loop || !op || op->size < sizeof(*op)) {
    return GCU_LOOP_ERR_INVALID;
  }
  if (!gcu_loop_is_owner(loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  if (loop->destroying) {
    return GCU_LOOP_ERR_STATE;
  }
  if (op->priv_state != GCU_LOOP_STATE_IDLE) {
    return GCU_LOOP_ERR_STATE;
  }
  return GCU_LOOP_OK;
}

/**
 * Make a socket this loop's, or refuse it.  A socket belongs to the first
 * loop that uses it.
 */
static GCU_Loop_Result gcu_loop_socket_prepare(
  GCU_Loop * loop, GCU_Socket * socket, GCU_Socket_Type type) {
  if (socket->closed || socket->detached || !socket->nonblocking
      || socket->type != type) {
    return GCU_LOOP_ERR_STATE;
  }
  if (socket->loop && socket->loop != loop) {
    return GCU_LOOP_ERR_STATE;
  }
  if (!socket->loop) {
    GCU_Loop_Result result = gcu_loop_plat_attach(loop, socket);
    if (result != GCU_LOOP_OK) {
      return result;
    }
    socket->loop = loop;
    socket->loop_prev = NULL;
    socket->loop_next = loop->sockets;
    if (loop->sockets) {
      loop->sockets->loop_prev = socket;
    }
    loop->sockets = socket;
  }
  return GCU_LOOP_OK;
}

static void gcu_loop_arm(GCU_Loop * loop, GCU_Loop_Op * op, unsigned kind,
  GCU_Socket * socket, void * buffer, size_t length) {
  gcu_loop_reset_result(op);
  op->priv_loop = loop;
  op->priv_socket = socket;
  op->priv_buffer = buffer;
  op->priv_length = length;
  op->priv_done = 0;
  op->priv_kind = (uint8_t)kind;
  op->priv_flags = 0;
  op->priv_aux_length = 0;
  op->priv_waiter = NULL;
  op->priv_pending_socket = NULL;
  op->priv_q_next = op->priv_q_prev = NULL;
  op->priv_state = GCU_LOOP_STATE_PENDING;
  gcu_loop_all_link(loop, op);
  if (socket) {
    ++socket->pending;
  }
}

/**
 * Undo gcu_loop_arm() for a start that failed.
 */
static void gcu_loop_disarm(GCU_Loop * loop, GCU_Loop_Op * op) {
  gcu_loop_all_unlink(loop, op);
  if (op->priv_socket) {
    --op->priv_socket->pending;
  }
  op->priv_state = GCU_LOOP_STATE_IDLE;
}

static GCU_Loop_Result gcu_loop_start_io(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, GCU_Socket_Type type, unsigned kind, void * buffer,
  size_t length, const GCU_Socket_Address * address) {
  GCU_Loop_Result result = gcu_loop_check(loop, op);
  if (result != GCU_LOOP_OK) {
    return result;
  }
  if (!socket || (length && !buffer)) {
    return GCU_LOOP_ERR_INVALID;
  }
  if (address && (address->size < sizeof(GCU_Socket_Address)
        || address->family != socket->family)) {
    return GCU_LOOP_ERR_INVALID;
  }
  result = gcu_loop_socket_prepare(loop, socket, type);
  if (result != GCU_LOOP_OK) {
    return result;
  }
  gcu_loop_arm(loop, op, kind, socket, buffer, length);
  if (address) {
    op->priv_address = *address;
  }
  result = gcu_loop_plat_start(loop, op);
  if (result != GCU_LOOP_OK) {
    gcu_loop_disarm(loop, op);
  }
  return result;
}

GCU_Loop_Result gcu_loop_read(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, void * buffer, size_t length) {
  return gcu_loop_start_io(loop, op, socket, GCU_SOCKET_STREAM,
    GCU_LOOP_KIND_READ, buffer, length, NULL);
}

GCU_Loop_Result gcu_loop_write(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const void * buffer, size_t length) {
  // The loop never writes through this pointer for a write.
  return gcu_loop_start_io(loop, op, socket, GCU_SOCKET_STREAM,
    GCU_LOOP_KIND_WRITE, (void *)(uintptr_t)buffer, length, NULL);
}

GCU_Loop_Result gcu_loop_accept(
  GCU_Loop * loop, GCU_Loop_Op * op, GCU_Socket * listener) {
  return gcu_loop_start_io(loop, op, listener, GCU_SOCKET_STREAM,
    GCU_LOOP_KIND_ACCEPT, NULL, 0, NULL);
}

GCU_Loop_Result gcu_loop_connect(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const GCU_Socket_Address * address) {
  if (!address) {
    return GCU_LOOP_ERR_INVALID;
  }
  return gcu_loop_start_io(loop, op, socket, GCU_SOCKET_STREAM,
    GCU_LOOP_KIND_CONNECT, NULL, 0, address);
}

GCU_Loop_Result gcu_loop_sendto(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const void * buffer, size_t length,
  const GCU_Socket_Address * address) {
  if (!address) {
    return GCU_LOOP_ERR_INVALID;
  }
  return gcu_loop_start_io(loop, op, socket, GCU_SOCKET_DATAGRAM,
    GCU_LOOP_KIND_SENDTO, (void *)(uintptr_t)buffer, length, address);
}

GCU_Loop_Result gcu_loop_recvfrom(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, void * buffer, size_t length) {
  return gcu_loop_start_io(loop, op, socket, GCU_SOCKET_DATAGRAM,
    GCU_LOOP_KIND_RECVFROM, buffer, length, NULL);
}

GCU_Loop_Result gcu_loop_timer_start(
  GCU_Loop * loop, GCU_Loop_Op * op, unsigned long delay_ms) {
  GCU_Loop_Result result = gcu_loop_check(loop, op);
  if (result != GCU_LOOP_OK) {
    return result;
  }
  // A delay too long to add to the clock is a delay nobody will wait out.
  uint64_t delay = delay_ms;
  const uint64_t longest = (uint64_t)INT64_MAX / 4 / 1000000;
  if (delay > longest) {
    delay = longest;
  }
  gcu_loop_arm(loop, op, GCU_LOOP_KIND_TIMER, NULL, NULL, 0);
  op->priv_deadline = gcu_loop_now() + (int64_t)delay * 1000000;
  op->priv_seq = ++loop->seq;
  if (!gcu_loop_heap_insert(loop, op)) {
    gcu_loop_disarm(loop, op);
    return GCU_LOOP_ERR_OOM;
  }
  return GCU_LOOP_OK;
}

GCU_Loop_Result gcu_loop_cancel(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (!loop || !op || op->size < sizeof(*op)) {
    return GCU_LOOP_ERR_INVALID;
  }
  if (!gcu_loop_is_owner(loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  if (op->priv_state != GCU_LOOP_STATE_PENDING) {
    return GCU_LOOP_ERR_STATE;
  }
  if (op->priv_kind == GCU_LOOP_KIND_POST || op->priv_loop != loop) {
    return GCU_LOOP_ERR_INVALID;
  }
#ifdef GCU_LOOP_PLANT_CANCEL_EARLY_RELEASE
  if (op->priv_kind != GCU_LOOP_KIND_TIMER) {
    // The defect: report the cancellation now and leave the OS reading into
    // a buffer the caller may now free.
    op->result.status = GCU_LOOP_CANCELLED;
    gcu_loop_deliver(loop, op);
    return GCU_LOOP_OK;
  }
#endif
  // Once a cancel has been taken there is nothing more to cancel, even where
  // the operation stays with the OS until its packet arrives.
  if ((op->priv_flags & GCU_LOOP_FLAG_CANCEL_REQUESTED)
      || !gcu_loop_cancel_pending(loop, op)) {
    return GCU_LOOP_ERR_STATE;
  }
  return GCU_LOOP_OK;
}

GCU_Loop_Result gcu_loop_timer_cancel(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (op && op->size >= sizeof(*op) && op->priv_state == GCU_LOOP_STATE_PENDING
      && op->priv_kind != GCU_LOOP_KIND_TIMER) {
    return GCU_LOOP_ERR_INVALID;
  }
  return gcu_loop_cancel(loop, op);
}

//
// Reaching the loop from another thread.
//

GCU_Loop_Result gcu_loop_post(GCU_Loop * loop, GCU_Loop_Op * op) {
  if (!loop || !op || op->size < sizeof(*op) || !op->callback) {
    return GCU_LOOP_ERR_INVALID;
  }
  GCU_MUTEX_LOCK(loop->mutex);
  if (loop->closing || op->priv_state != GCU_LOOP_STATE_IDLE) {
    GCU_MUTEX_UNLOCK(loop->mutex);
    return GCU_LOOP_ERR_STATE;
  }
  gcu_loop_reset_result(op);
  op->priv_loop = loop;
  op->priv_socket = NULL;
  op->priv_kind = GCU_LOOP_KIND_POST;
  op->priv_flags = 0;
  op->priv_waiter = NULL;
  op->priv_state = GCU_LOOP_STATE_PENDING;
  op->priv_q_next = NULL;
  if (loop->post_tail) {
    loop->post_tail->priv_q_next = op;
  }
  else {
    loop->post_head = op;
  }
  loop->post_tail = op;
  ++loop->post_count;
  // The wake is inside the lock so that destroy, which takes it to close the
  // list, cannot free the loop between the append and the wake.
  GCU_Loop_Result result = GCU_LOOP_OK;
#ifndef GCU_LOOP_PLANT_NO_WAKE
  result = gcu_loop_plat_wake(loop);
#endif
  if (result != GCU_LOOP_OK) {
    // Not posted: take the record back off the list (it is the tail), so the
    // caller who sees an error has an idle record and no callback to expect.
    if (loop->post_head == op) {
      loop->post_head = loop->post_tail = NULL;
    }
    else {
      GCU_Loop_Op * before = loop->post_head;
      while (before->priv_q_next != op) {
        before = before->priv_q_next;
      }
      before->priv_q_next = NULL;
      loop->post_tail = before;
    }
    --loop->post_count;
    op->priv_state = GCU_LOOP_STATE_IDLE;
  }
  GCU_MUTEX_UNLOCK(loop->mutex);
  return result;
}

GCU_Loop_Result gcu_loop_wake(GCU_Loop * loop) {
  if (!loop) {
    return GCU_LOOP_ERR_INVALID;
  }
  GCU_MUTEX_LOCK(loop->mutex);
  GCU_Loop_Result result =
    loop->closing ? GCU_LOOP_ERR_STATE : gcu_loop_plat_wake(loop);
  GCU_MUTEX_UNLOCK(loop->mutex);
  return result;
}

GCU_Loop_Result gcu_loop_fiber_wait(GCU_Loop_Op * op) {
  if (!op || op->size < sizeof(*op)) {
    return GCU_LOOP_ERR_INVALID;
  }
  GCU_Fiber * fiber = gcu_fiber_current();
  if (!fiber) {
    return GCU_LOOP_ERR_STATE;
  }
  // The state is read before the loop pointer is trusted: an idle record's
  // pointer may name a loop that has since been destroyed.
  if (op->priv_state == GCU_LOOP_STATE_IDLE
      || op->priv_kind == GCU_LOOP_KIND_POST || op->priv_waiter) {
    return GCU_LOOP_ERR_STATE;
  }
  if (!gcu_loop_is_owner(op->priv_loop)) {
    return GCU_LOOP_ERR_THREAD;
  }
  uint32_t generation = op->priv_generation;
  op->priv_waiter = fiber;
  while (op->priv_generation == generation) {
    if (gcu_fiber_yield() != GCU_FIBER_OK) {
      op->priv_waiter = NULL;
      return GCU_LOOP_ERR_STATE;
    }
  }
  return GCU_LOOP_OK;
}

#ifndef _WIN32

// ===========================================================================
// The epoll arm.
//
// Every operation is attempted at once.  Only when the OS says it would block
// is the operation put on its socket's wait queue and the socket's epoll
// interest raised.  Registration is level-triggered and is dropped whenever
// both queues are empty: a registration with no interest would still report
// hang-up and error, and a loop that is told about a closed connection it has
// nothing to do with spins.
// ===========================================================================

// This arm keeps addresses on the stack and uses neither priv_ov nor priv_aux;
// what it does share with the other arms is that a native address fits the
// private block.  (The Windows arm asserts its own needs below.)
_Static_assert(sizeof(struct sockaddr_in6) <= sizeof(((GCU_Loop_Op *)0)->priv_aux),
  "GCU_Loop_Op::priv_aux must hold a native IPv6 address");

static bool gcu_loop_kind_reads(unsigned kind) {
  return kind == GCU_LOOP_KIND_READ || kind == GCU_LOOP_KIND_ACCEPT
    || kind == GCU_LOOP_KIND_RECVFROM;
}

static GCU_Loop_Op ** gcu_loop_queue_head(GCU_Socket * socket, unsigned kind) {
  return gcu_loop_kind_reads(kind) ? &socket->rq_head : &socket->wq_head;
}

static GCU_Loop_Op ** gcu_loop_queue_tail(GCU_Socket * socket, unsigned kind) {
  return gcu_loop_kind_reads(kind) ? &socket->rq_tail : &socket->wq_tail;
}

static void gcu_loop_queue_append(GCU_Socket * socket, GCU_Loop_Op * op) {
  GCU_Loop_Op ** head = gcu_loop_queue_head(socket, op->priv_kind);
  GCU_Loop_Op ** tail = gcu_loop_queue_tail(socket, op->priv_kind);
  op->priv_q_next = NULL;
  op->priv_q_prev = *tail;
  if (*tail) {
    (*tail)->priv_q_next = op;
  }
  else {
    *head = op;
  }
  *tail = op;
  op->priv_flags |= GCU_LOOP_FLAG_QUEUED;
}

static void gcu_loop_queue_remove(GCU_Socket * socket, GCU_Loop_Op * op) {
  GCU_Loop_Op ** head = gcu_loop_queue_head(socket, op->priv_kind);
  GCU_Loop_Op ** tail = gcu_loop_queue_tail(socket, op->priv_kind);
  if (op->priv_q_prev) {
    op->priv_q_prev->priv_q_next = op->priv_q_next;
  }
  else {
    *head = op->priv_q_next;
  }
  if (op->priv_q_next) {
    op->priv_q_next->priv_q_prev = op->priv_q_prev;
  }
  else {
    *tail = op->priv_q_prev;
  }
  op->priv_q_next = op->priv_q_prev = NULL;
  op->priv_flags &= (uint8_t)~GCU_LOOP_FLAG_QUEUED;
}

/**
 * Make the epoll registration say what the two queues need.
 */
static int gcu_loop_update_interest(GCU_Loop * loop, GCU_Socket * socket) {
  uint32_t want = (socket->rq_head ? EPOLLIN : 0u)
    | (socket->wq_head ? EPOLLOUT : 0u);
  if (!want) {
    if (socket->registered) {
      epoll_ctl(loop->epfd, EPOLL_CTL_DEL, socket->fd, NULL);
      socket->registered = false;
      socket->interest = 0;
    }
    return 0;
  }
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = want;
  event.data.ptr = socket;
  if (!socket->registered) {
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, socket->fd, &event) < 0) {
      return -1;
    }
    socket->registered = true;
  }
  else if (want != socket->interest) {
    if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, socket->fd, &event) < 0) {
      return -1;
    }
  }
  socket->interest = want;
  return 0;
}

/**
 * Decide an operation's result.
 */
static void gcu_loop_finish(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Loop_Result status, size_t bytes, int os_error) {
  if (op->priv_flags & GCU_LOOP_FLAG_QUEUED) {
    gcu_loop_queue_remove(op->priv_socket, op);
  }
#ifdef GCU_LOOP_PLANT_CANCEL_EARLY_RELEASE
  if (op->priv_state != GCU_LOOP_STATE_PENDING) {
    // The planted cancel already delivered this record.  Dropping the second
    // completion keeps the defect to the one that matters: the OS was still
    // reading into the caller's buffer.
    return;
  }
#endif
  if (op->priv_pending_socket) {
    // An accept that did not take a connection gives its memory back.
    gcu_socket_free_(op->priv_pending_socket);
    op->priv_pending_socket = NULL;
  }
  op->result.status = status;
  op->result.bytes = bytes;
  op->result.os_error = os_error;
  gcu_loop_ready_push(loop, op);
}

static bool gcu_loop_would_block(int error) {
  return error == EAGAIN || error == EWOULDBLOCK;
}

/**
 * Try an operation.  Returns `false` when the OS says to wait, with the
 * operation untouched but for progress; `true` when its result is decided.
 *
 * @param events What epoll reported, or 0 when called outside an event.
 */
static bool gcu_loop_attempt(
  GCU_Loop * loop, GCU_Loop_Op * op, uint32_t events) {
  GCU_Socket * socket = op->priv_socket;
  int fd = socket->fd;

  switch (op->priv_kind) {
    case GCU_LOOP_KIND_READ:
      if (op->priv_length == 0) {
        // Nothing to read into: done, without asking the OS, which would
        // answer "would block" on a stream with nothing to say.
        gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
        return true;
      }
      for (;;) {
        ssize_t n = recv(fd, op->priv_buffer, op->priv_length, 0);
        if (n >= 0) {
          gcu_loop_finish(loop, op, GCU_LOOP_OK, (size_t)n, 0);
          return true;
        }
        int error = errno;
        if (error == EINTR) {
          continue;
        }
        if (gcu_loop_would_block(error)) {
          return false;
        }
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return true;
      }

    case GCU_LOOP_KIND_WRITE:
      while (op->priv_done < op->priv_length) {
        ssize_t n = send(fd, (const char *)op->priv_buffer + op->priv_done,
          op->priv_length - op->priv_done, MSG_NOSIGNAL);
        if (n > 0) {
          op->priv_done += (size_t)n;
          continue;
        }
        int error = n < 0 ? errno : EAGAIN;
        if (error == EINTR) {
          continue;
        }
        if (gcu_loop_would_block(error)) {
          return false;
        }
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, op->priv_done, error);
        return true;
      }
      gcu_loop_finish(loop, op, GCU_LOOP_OK, op->priv_done, 0);
      return true;

    case GCU_LOOP_KIND_ACCEPT:
      // The memory for the new socket comes first, and is kept across waits,
      // so that running out of it leaves the connection in the listener's
      // backlog (as on Windows, where AcceptEx needs its socket beforehand)
      // for the next accept to take, not accepted and then dropped.
      if (!op->priv_pending_socket
          && gcu_socket_allocate_(&op->priv_pending_socket, loop->allocator,
               socket->family, GCU_SOCKET_STREAM) != GCU_SOCKET_OK) {
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OOM, 0, 0);
        return true;
      }
      for (;;) {
        struct sockaddr_storage storage;
        socklen_t length = sizeof(storage);
        int accepted = accept4(fd, (struct sockaddr *)&storage, &length,
          SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted >= 0) {
          op->priv_pending_socket->fd = accepted;
          op->result.accepted = op->priv_pending_socket;
          op->priv_pending_socket = NULL;
          gcu_socket_address_from_native_(
            &storage, (int)length, &op->result.peer);
          gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
          return true;
        }
        int error = errno;
        // A connection that was reset before it was accepted is not this
        // caller's failure: the next one in the queue is the answer.
        if (error == EINTR || error == ECONNABORTED || error == EPROTO) {
          continue;
        }
        if (gcu_loop_would_block(error)) {
          return false;
        }
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return true;
      }

    case GCU_LOOP_KIND_CONNECT:
      if (!(op->priv_flags & GCU_LOOP_FLAG_STARTED)) {
        struct sockaddr_storage storage;
        int length = 0;
        gcu_socket_address_to_native_(&op->priv_address, &storage, &length);
        int status = connect(fd, (const struct sockaddr *)&storage,
          (socklen_t)length);
        if (status == 0) {
          socket->bound = true;
          gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
          return true;
        }
        int error = errno;
        // After EINTR the connection carries on without the caller, as it
        // does after EINPROGRESS (POSIX connect).
        if (error == EINPROGRESS || error == EINTR) {
          op->priv_flags |= GCU_LOOP_FLAG_STARTED;
          socket->bound = true;
          return false;
        }
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return true;
      }
      else {
        // SO_ERROR reads 0 while the connection is still in progress, so only
        // an event that says the connection ended either way can be believed.
        if (!(events & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
          return false;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
          error = errno;
        }
        if (error == EINPROGRESS) {
          return false;
        }
        if (error == 0) {
          gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
        }
        else {
          gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        }
        return true;
      }

    case GCU_LOOP_KIND_SENDTO:
      for (;;) {
        struct sockaddr_storage storage;
        int length = 0;
        gcu_socket_address_to_native_(&op->priv_address, &storage, &length);
        ssize_t n = sendto(fd, op->priv_buffer, op->priv_length, MSG_NOSIGNAL,
          (const struct sockaddr *)&storage, (socklen_t)length);
        if (n >= 0) {
          gcu_loop_finish(loop, op, GCU_LOOP_OK, (size_t)n, 0);
          return true;
        }
        int error = errno;
        if (error == EINTR) {
          continue;
        }
        if (gcu_loop_would_block(error)) {
          return false;
        }
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return true;
      }

    case GCU_LOOP_KIND_RECVFROM:
      for (;;) {
        struct sockaddr_storage storage;
        socklen_t length = sizeof(storage);
        ssize_t n = recvfrom(fd, op->priv_buffer, op->priv_length, 0,
          (struct sockaddr *)&storage, &length);
        if (n >= 0) {
          gcu_socket_address_from_native_(
            &storage, (int)length, &op->result.peer);
          gcu_loop_finish(loop, op, GCU_LOOP_OK, (size_t)n, 0);
          return true;
        }
        int error = errno;
        if (error == EINTR) {
          continue;
        }
        if (gcu_loop_would_block(error)) {
          return false;
        }
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return true;
      }

    default:
      gcu_loop_finish(loop, op, GCU_LOOP_ERR_INVALID, 0, 0);
      return true;
  }
}

/**
 * Work down one of a socket's queues until the OS says to wait.  Only the
 * head is tried, so operations on one socket finish in the order started.
 */
static void gcu_loop_service_queue(
  GCU_Loop * loop, GCU_Loop_Op ** head, uint32_t events) {
  while (*head) {
    if (!gcu_loop_attempt(loop, *head, events)) {
      break;
    }
  }
}

/**
 * The OS would not take the registration the queues need, so nothing will
 * ever wake them: fail every queued operation with the OS error rather than
 * leave it pending for good.
 */
static void gcu_loop_fail_queues(
  GCU_Loop * loop, GCU_Socket * socket, int error) {
  while (socket->rq_head) {
    gcu_loop_finish(loop, socket->rq_head, GCU_LOOP_ERR_OS,
      socket->rq_head->priv_done, error);
  }
  while (socket->wq_head) {
    gcu_loop_finish(loop, socket->wq_head, GCU_LOOP_ERR_OS,
      socket->wq_head->priv_done, error);
  }
  gcu_loop_update_interest(loop, socket);
}

static void gcu_loop_service_socket(
  GCU_Loop * loop, GCU_Socket * socket, uint32_t events) {
  if (socket->closed) {
    return;
  }
  if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
    gcu_loop_service_queue(loop, &socket->rq_head, events);
  }
  if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
    gcu_loop_service_queue(loop, &socket->wq_head, events);
  }
  if (gcu_loop_update_interest(loop, socket) < 0) {
    gcu_loop_fail_queues(loop, socket, errno);
  }
}

static GCU_Loop_Result gcu_loop_plat_create(GCU_Loop * loop) {
  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->epfd < 0) {
    return GCU_LOOP_ERR_OS;
  }
  loop->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (loop->wakefd < 0) {
    int saved = errno;
    close(loop->epfd);
    errno = saved;
    return GCU_LOOP_ERR_OS;
  }
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN;
  event.data.ptr = loop;
  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->wakefd, &event) < 0) {
    int saved = errno;
    close(loop->wakefd);
    close(loop->epfd);
    errno = saved;
    return GCU_LOOP_ERR_OS;
  }
  return GCU_LOOP_OK;
}

static void gcu_loop_plat_destroy(GCU_Loop * loop) {
  close(loop->wakefd);
  close(loop->epfd);
}

static GCU_Loop_Result gcu_loop_plat_wake(GCU_Loop * loop) {
  uint64_t one = 1;
  ssize_t n;
  do {
    n = write(loop->wakefd, &one, sizeof(one));
  } while (n < 0 && errno == EINTR);
  // A full counter (EAGAIN) means a wake is already waiting to be read.
  if (n < 0 && !gcu_loop_would_block(errno)) {
    return GCU_LOOP_ERR_OS;
  }
  return GCU_LOOP_OK;
}

static GCU_Loop_Result gcu_loop_plat_wait(GCU_Loop * loop, long wait_ms) {
  struct epoll_event events[64];
  int n = epoll_wait(loop->epfd, events, 64,
    wait_ms > INT_MAX ? INT_MAX : (int)wait_ms);
  if (n < 0) {
    return errno == EINTR ? GCU_LOOP_OK : GCU_LOOP_ERR_OS;
  }
  for (int i = 0; i < n; ++i) {
    if (events[i].data.ptr == loop) {
      uint64_t count;
      ssize_t ignored = read(loop->wakefd, &count, sizeof(count));
      (void)ignored;
      continue;
    }
    gcu_loop_service_socket(
      loop, (GCU_Socket *)events[i].data.ptr, events[i].events);
  }
  return GCU_LOOP_OK;
}

static GCU_Loop_Result gcu_loop_plat_attach(
  GCU_Loop * loop, GCU_Socket * socket) {
  (void)loop;
  (void)socket;
  return GCU_LOOP_OK;
}

static GCU_Loop_Result gcu_loop_plat_start(GCU_Loop * loop, GCU_Loop_Op * op) {
  GCU_Socket * socket = op->priv_socket;
  GCU_Loop_Op ** head = gcu_loop_queue_head(socket, op->priv_kind);

  if (!*head && gcu_loop_attempt(loop, op, 0)) {
    // Decided at once.  It is delivered by the loop, not by this call.
    return GCU_LOOP_OK;
  }
  gcu_loop_queue_append(socket, op);
  if (gcu_loop_update_interest(loop, socket) < 0) {
    int saved = errno;
    gcu_loop_queue_remove(socket, op);
    gcu_loop_update_interest(loop, socket);
    if (op->priv_pending_socket) {
      // An accept that never began gives back the memory it set aside.
      gcu_socket_free_(op->priv_pending_socket);
      op->priv_pending_socket = NULL;
    }
    errno = saved;
    return GCU_LOOP_ERR_OS;
  }
  return GCU_LOOP_OK;
}

static bool gcu_loop_plat_cancel(GCU_Loop * loop, GCU_Loop_Op * op) {
  GCU_Socket * socket = op->priv_socket;
  if (op->priv_flags & GCU_LOOP_FLAG_QUEUED) {
    gcu_loop_queue_remove(socket, op);
  }
  // The buffer is no longer anyone's but the caller's, but the caller learns
  // that from the completion, which this decides and the loop delivers.
  gcu_loop_finish(loop, op, GCU_LOOP_CANCELLED, op->priv_done, 0);
  if (gcu_loop_update_interest(loop, socket) < 0) {
    gcu_loop_fail_queues(loop, socket, errno);
  }
  return true;
}

static void gcu_loop_plat_detach(GCU_Loop * loop, GCU_Socket * socket) {
  if (socket->registered) {
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, socket->fd, NULL);
    socket->registered = false;
    socket->interest = 0;
  }
}

static void gcu_loop_plat_end_iteration(GCU_Loop * loop) {
  while (loop->zombies) {
    GCU_Socket * zombie = loop->zombies;
    loop->zombies = zombie->zombie_next;
    gcu_socket_free_(zombie);
  }
}

static bool gcu_loop_plat_drain(GCU_Loop * loop) {
  // Every cancelled operation was decided by the cancel; nothing is still
  // with the OS.
  (void)loop;
  return false;
}

void gcu_loop_socket_closed_(GCU_Loop * loop, GCU_Socket * socket) {
  gcu_loop_plat_detach(loop, socket);
  if (socket->loop_prev) {
    socket->loop_prev->loop_next = socket->loop_next;
  }
  else {
    loop->sockets = socket->loop_next;
  }
  if (socket->loop_next) {
    socket->loop_next->loop_prev = socket->loop_prev;
  }
  gcu_socket_native_close_(socket);
  socket->loop = NULL;
  if (loop->running) {
    // An event for this socket may already be in the batch being delivered,
    // so its memory goes at the end of the iteration.
    socket->closed = true;
    socket->zombie_next = loop->zombies;
    loop->zombies = socket;
  }
  else {
    gcu_socket_free_(socket);
  }
}

#else // _WIN32

// ===========================================================================
// The I/O completion port arm.
//
// An operation is handed to the OS with an OVERLAPPED that lives in the
// record, and a packet comes back when it has finished, successfully or not,
// including when it finished at once.  Nothing here asks for success to skip
// the port: one path for every completion is worth more than the wakeups.
// ===========================================================================

_Static_assert(sizeof(OVERLAPPED) <= sizeof(((GCU_Loop_Op *)0)->priv_ov),
  "GCU_Loop_Op::priv_ov is too small for an OVERLAPPED");
_Static_assert(2 * (sizeof(SOCKADDR_IN6) + 16) <=
  sizeof(((GCU_Loop_Op *)0)->priv_aux),
  "GCU_Loop_Op::priv_aux is too small for AcceptEx's address block");

// What AcceptEx wants appended to each address in its buffer.
#define GCU_LOOP_ACCEPT_ADDRESS_LENGTH ((DWORD)(sizeof(SOCKADDR_IN6) + 16))

// The most one WSABUF can describe; a longer buffer is done in pieces.
#define GCU_LOOP_IOCP_CHUNK ((size_t)0x40000000)

static OVERLAPPED * gcu_loop_overlapped(GCU_Loop_Op * op) {
  return (OVERLAPPED *)op->priv_ov;
}

static GCU_Loop_Op * gcu_loop_op_of(OVERLAPPED * overlapped) {
  return (GCU_Loop_Op *)((char *)overlapped - offsetof(GCU_Loop_Op, priv_ov));
}

static void gcu_loop_finish(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Loop_Result status, size_t bytes, int os_error) {
#ifdef GCU_LOOP_PLANT_CANCEL_EARLY_RELEASE
  if (op->priv_state != GCU_LOOP_STATE_PENDING) {
    // The planted cancel already delivered this record; the packet that now
    // arrives for it is dropped, while the OS has written into the buffer.
    return;
  }
#endif
  op->result.status = status;
  op->result.bytes = bytes;
  op->result.os_error = os_error;
  gcu_loop_ready_push(loop, op);
}

/**
 * Hand the operation, or the rest of it, to the OS.  `false` means the OS
 * refused at once and the result is decided; `true` means a packet will come.
 */
static bool gcu_loop_issue(GCU_Loop * loop, GCU_Loop_Op * op) {
  GCU_Socket * socket = op->priv_socket;
  SOCKET handle = GCU_LOOP_NATIVE(socket);
  OVERLAPPED * overlapped = gcu_loop_overlapped(op);
  memset(overlapped, 0, sizeof(*overlapped));

  WSABUF buffer;
  size_t remaining = op->priv_length - op->priv_done;
  buffer.len = (ULONG)(remaining > GCU_LOOP_IOCP_CHUNK
    ? GCU_LOOP_IOCP_CHUNK : remaining);
  buffer.buf = (CHAR *)op->priv_buffer + op->priv_done;
  DWORD flags = 0;
  int status = 0;

  switch (op->priv_kind) {
    case GCU_LOOP_KIND_READ:
      if (op->priv_length == 0) {
        // As on Linux: nothing to read into is done at once.
        gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
        return false;
      }
      status = WSARecv(handle, &buffer, 1, NULL, &flags, overlapped, NULL);
      break;

    case GCU_LOOP_KIND_WRITE:
      status = WSASend(handle, &buffer, 1, NULL, 0, overlapped, NULL);
      break;

    case GCU_LOOP_KIND_ACCEPT: {
      GCU_Socket * made = NULL;
      GCU_Socket_Result created = gcu_socket_create(
        &made, socket->family, GCU_SOCKET_STREAM, loop->allocator);
      if (created != GCU_SOCKET_OK) {
        int error = WSAGetLastError();
        gcu_loop_finish(loop, op,
          created == GCU_SOCKET_ERR_OOM ? GCU_LOOP_ERR_OOM : GCU_LOOP_ERR_OS,
          0, error);
        return false;
      }
      op->priv_pending_socket = made;
      DWORD got = 0;
      if (!AcceptEx(handle, GCU_LOOP_NATIVE(made), op->priv_aux, 0,
            GCU_LOOP_ACCEPT_ADDRESS_LENGTH, GCU_LOOP_ACCEPT_ADDRESS_LENGTH,
            &got, overlapped)) {
        status = SOCKET_ERROR;
      }
      break;
    }

    case GCU_LOOP_KIND_CONNECT: {
      // ConnectEx wants the socket bound first.
      if (!socket->bound) {
        GCU_Socket_Address any;
        gcu_socket_address_any(&any, socket->family, 0);
        if (gcu_socket_bind(socket, &any) != GCU_SOCKET_OK) {
          gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, WSAGetLastError());
          return false;
        }
      }
      GUID guid = WSAID_CONNECTEX;
      LPFN_CONNECTEX connect_ex = NULL;
      DWORD got = 0;
      if (WSAIoctl(handle, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid,
            sizeof(guid), &connect_ex, sizeof(connect_ex), &got, NULL, NULL)
          == SOCKET_ERROR) {
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, WSAGetLastError());
        return false;
      }
      int length = 0;
      gcu_socket_address_to_native_(&op->priv_address, op->priv_aux, &length);
      if (!connect_ex(handle, (const struct sockaddr *)op->priv_aux, length,
            NULL, 0, NULL, overlapped)) {
        status = SOCKET_ERROR;
      }
      break;
    }

    case GCU_LOOP_KIND_SENDTO: {
      int length = 0;
      gcu_socket_address_to_native_(&op->priv_address, op->priv_aux, &length);
      status = WSASendTo(handle, &buffer, 1, NULL, 0,
        (const struct sockaddr *)op->priv_aux, length, overlapped, NULL);
      break;
    }

    case GCU_LOOP_KIND_RECVFROM:
      op->priv_aux_length = socket->family == GCU_SOCKET_IPV4
        ? (int)sizeof(SOCKADDR_IN) : (int)sizeof(SOCKADDR_IN6);
      status = WSARecvFrom(handle, &buffer, 1, NULL, &flags,
        (struct sockaddr *)op->priv_aux, &op->priv_aux_length, overlapped,
        NULL);
      break;

    default:
      gcu_loop_finish(loop, op, GCU_LOOP_ERR_INVALID, 0, 0);
      return false;
  }

  // A datagram longer than the buffer fails with WSAEMSGSIZE at once and is
  // still completed through the port: the NT status behind it is a warning,
  // which counts as a finished operation.  Treat it as pending, or the packet
  // that follows is a second completion of the same record.
  if (status != 0 && WSAGetLastError() != WSA_IO_PENDING
      && !(op->priv_kind == GCU_LOOP_KIND_RECVFROM
           && WSAGetLastError() == WSAEMSGSIZE)) {
    int error = WSAGetLastError();
    if (op->priv_pending_socket) {
      gcu_socket_close(op->priv_pending_socket);
      op->priv_pending_socket = NULL;
    }
    gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, op->priv_done, error);
    return false;
  }
  return true;
}

/**
 * A packet came back for an operation: read how it went and decide.
 */
static void gcu_loop_iocp_completed(GCU_Loop * loop, GCU_Loop_Op * op) {
  // A packet for a record that is not waiting for one is not an operation's
  // answer, and delivering it again would run a callback twice.  Nothing here
  // should produce one; the planted early cancel does, on purpose.
  if (op->priv_state != GCU_LOOP_STATE_PENDING) {
    return;
  }
  GCU_Socket * socket = op->priv_socket;
  DWORD got = 0;
  DWORD flags = 0;
  BOOL ok = WSAGetOverlappedResult(GCU_LOOP_NATIVE(socket),
    gcu_loop_overlapped(op), &got, FALSE, &flags);
  int error = ok ? 0 : WSAGetLastError();

  if (!ok && error == WSA_OPERATION_ABORTED
      && (op->priv_flags & GCU_LOOP_FLAG_CANCEL_REQUESTED)) {
    if (op->priv_pending_socket) {
      gcu_socket_close(op->priv_pending_socket);
      op->priv_pending_socket = NULL;
    }
    gcu_loop_finish(loop, op, GCU_LOOP_CANCELLED, op->priv_done, 0);
    return;
  }

  switch (op->priv_kind) {
    case GCU_LOOP_KIND_READ:
    case GCU_LOOP_KIND_SENDTO:
      if (ok) {
        gcu_loop_finish(loop, op, GCU_LOOP_OK, (size_t)got, 0);
      }
      else {
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
      }
      return;

    case GCU_LOOP_KIND_RECVFROM:
      // A datagram longer than the buffer is cut to it, as on Linux.
      if (ok || error == WSAEMSGSIZE) {
        gcu_socket_address_from_native_(
          op->priv_aux, op->priv_aux_length, &op->result.peer);
        gcu_loop_finish(loop, op, GCU_LOOP_OK, (size_t)got, 0);
      }
      else {
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
      }
      return;

    case GCU_LOOP_KIND_WRITE:
      if (!ok) {
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, op->priv_done, error);
        return;
      }
      op->priv_done += got;
      if (op->priv_done < op->priv_length
          && (op->priv_flags & GCU_LOOP_FLAG_CANCEL_REQUESTED)) {
        // A cancel was accepted while this write was partly done: honour it
        // instead of reissuing the rest.
        gcu_loop_finish(loop, op, GCU_LOOP_CANCELLED, op->priv_done, 0);
        return;
      }
      if (op->priv_done < op->priv_length && got > 0) {
        // The OS took part of it; the rest is a new request.
        gcu_loop_issue(loop, op);
        return;
      }
      gcu_loop_finish(loop, op, GCU_LOOP_OK, op->priv_done, 0);
      return;

    case GCU_LOOP_KIND_ACCEPT: {
      GCU_Socket * made = op->priv_pending_socket;
      op->priv_pending_socket = NULL;
      if (!ok) {
        gcu_socket_close(made);
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return;
      }
      // Without this the accepted socket is not yet a connected socket as far
      // as getpeername, shutdown and the rest are concerned.
      SOCKET listener = GCU_LOOP_NATIVE(socket);
      setsockopt(GCU_LOOP_NATIVE(made), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (const char *)&listener, sizeof(listener));
      GCU_Socket_Address peer;
      if (gcu_socket_peer_address(made, &peer) == GCU_SOCKET_OK) {
        op->result.peer = peer;
      }
      op->result.accepted = made;
      gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
      return;
    }

    case GCU_LOOP_KIND_CONNECT:
      if (!ok) {
        gcu_loop_finish(loop, op, GCU_LOOP_ERR_OS, 0, error);
        return;
      }
      setsockopt(GCU_LOOP_NATIVE(socket), SOL_SOCKET,
        SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
      gcu_loop_finish(loop, op, GCU_LOOP_OK, 0, 0);
      return;

    default:
      gcu_loop_finish(loop, op, GCU_LOOP_ERR_INVALID, 0, 0);
      return;
  }
}

static GCU_Loop_Result gcu_loop_plat_create(GCU_Loop * loop) {
  gcu_socket_startup_();
  loop->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  return loop->port ? GCU_LOOP_OK : GCU_LOOP_ERR_OS;
}

static void gcu_loop_plat_destroy(GCU_Loop * loop) {
  CloseHandle(loop->port);
}

static GCU_Loop_Result gcu_loop_plat_wake(GCU_Loop * loop) {
  return PostQueuedCompletionStatus(loop->port, 0, 0, NULL)
    ? GCU_LOOP_OK : GCU_LOOP_ERR_OS;
}

static GCU_Loop_Result gcu_loop_plat_wait(GCU_Loop * loop, long wait_ms) {
  OVERLAPPED_ENTRY entries[64];
  ULONG count = 0;
  DWORD timeout = wait_ms < 0 ? INFINITE
    : (wait_ms > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)wait_ms);
  if (!GetQueuedCompletionStatusEx(
        loop->port, entries, 64, &count, timeout, FALSE)) {
    return GetLastError() == WAIT_TIMEOUT ? GCU_LOOP_OK : GCU_LOOP_ERR_OS;
  }
  for (ULONG i = 0; i < count; ++i) {
    // A packet with no OVERLAPPED is a wake.
    if (entries[i].lpOverlapped) {
      gcu_loop_iocp_completed(loop, gcu_loop_op_of(entries[i].lpOverlapped));
    }
  }
  return GCU_LOOP_OK;
}

static GCU_Loop_Result gcu_loop_plat_attach(
  GCU_Loop * loop, GCU_Socket * socket) {
  if (!CreateIoCompletionPort(
        (HANDLE)GCU_LOOP_NATIVE(socket), loop->port, (ULONG_PTR)socket, 0)) {
    return GCU_LOOP_ERR_OS;
  }
  return GCU_LOOP_OK;
}

static GCU_Loop_Result gcu_loop_plat_start(GCU_Loop * loop, GCU_Loop_Op * op) {
  // Whether the OS took it or refused it, a result is on its way: a packet in
  // the first case and the ready queue in the second.
  gcu_loop_issue(loop, op);
  return GCU_LOOP_OK;
}

static bool gcu_loop_plat_cancel(GCU_Loop * loop, GCU_Loop_Op * op) {
  (void)loop;
  op->priv_flags |= GCU_LOOP_FLAG_CANCEL_REQUESTED;
  if (!CancelIoEx((HANDLE)GCU_LOOP_NATIVE(op->priv_socket),
        gcu_loop_overlapped(op))) {
    // ERROR_NOT_FOUND: the OS has already finished the operation and its
    // packet is queued.  That packet carries the real result, so the cancel
    // lost, which is the same answer as for an operation whose result the
    // loop had decided.  Any other failure means no abort was requested, so
    // no aborted packet will come: the cancel is not taken either.
    op->priv_flags &= (uint8_t)~GCU_LOOP_FLAG_CANCEL_REQUESTED;
    return false;
  }
  return true;
}

static void gcu_loop_plat_detach(GCU_Loop * loop, GCU_Socket * socket) {
  (void)loop;
  (void)socket;
}

static void gcu_loop_plat_end_iteration(GCU_Loop * loop) {
  (void)loop;
}

static bool gcu_loop_plat_drain(GCU_Loop * loop) {
  // The cancelled operations are the OS's until their packets arrive.  Bound
  // the wait: an operation whose cancel the OS refused would otherwise hold
  // the destroy forever.
  if (loop->drain_waits >= 30) {
    loop->drain_waits = 0;   // a retried destroy gets its own thirty
    return false;
  }
  ++loop->drain_waits;
  gcu_loop_plat_wait(loop, 1000);
  gcu_loop_posts_collect(loop, GCU_LOOP_OK);
  return true;
}

void gcu_loop_socket_closed_(GCU_Loop * loop, GCU_Socket * socket) {
  if (socket->loop_prev) {
    socket->loop_prev->loop_next = socket->loop_next;
  }
  else {
    loop->sockets = socket->loop_next;
  }
  if (socket->loop_next) {
    socket->loop_next->loop_prev = socket->loop_prev;
  }
  gcu_socket_native_close_(socket);
  socket->loop = NULL;
  gcu_socket_free_(socket);
}

#endif // _WIN32

#else // GCU_LOOP_SUPPORTED

// ===========================================================================
// The unsupported arm: kqueue is intended and not written.  Every call that
// would create or drive a loop says so, and the library still links.
// ===========================================================================

GCU_Loop_Result gcu_loop_create(
  GCU_Loop ** out_loop, const GCU_Allocator * allocator) {
  (void)allocator;
  return out_loop ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_destroy(GCU_Loop * loop) {
  return loop ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_OK;
}

GCU_Loop_Result gcu_loop_run(GCU_Loop * loop) {
  return loop ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_run_once(GCU_Loop * loop, long timeout_ms) {
  (void)timeout_ms;
  return loop ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_stop(GCU_Loop * loop) {
  return loop ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

size_t gcu_loop_pending(const GCU_Loop * loop) {
  (void)loop;
  return 0;
}

GCU_Loop_Result gcu_loop_read(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, void * buffer, size_t length) {
  (void)buffer; (void)length;
  return loop && op && socket ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_write(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const void * buffer, size_t length) {
  (void)buffer; (void)length;
  return loop && op && socket ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_accept(
  GCU_Loop * loop, GCU_Loop_Op * op, GCU_Socket * listener) {
  return loop && op && listener ? GCU_LOOP_ERR_UNSUPPORTED
                                : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_connect(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const GCU_Socket_Address * address) {
  return loop && op && socket && address ? GCU_LOOP_ERR_UNSUPPORTED
                                         : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_sendto(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, const void * buffer, size_t length,
  const GCU_Socket_Address * address) {
  (void)buffer; (void)length;
  return loop && op && socket && address ? GCU_LOOP_ERR_UNSUPPORTED
                                         : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_recvfrom(GCU_Loop * loop, GCU_Loop_Op * op,
  GCU_Socket * socket, void * buffer, size_t length) {
  (void)buffer; (void)length;
  return loop && op && socket ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_timer_start(
  GCU_Loop * loop, GCU_Loop_Op * op, unsigned long delay_ms) {
  (void)delay_ms;
  return loop && op ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_cancel(GCU_Loop * loop, GCU_Loop_Op * op) {
  return loop && op ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_timer_cancel(GCU_Loop * loop, GCU_Loop_Op * op) {
  return loop && op ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_post(GCU_Loop * loop, GCU_Loop_Op * op) {
  return loop && op ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_wake(GCU_Loop * loop) {
  return loop ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

GCU_Loop_Result gcu_loop_fiber_wait(GCU_Loop_Op * op) {
  return op ? GCU_LOOP_ERR_UNSUPPORTED : GCU_LOOP_ERR_INVALID;
}

void gcu_loop_socket_closed_(GCU_Loop * loop, GCU_Socket * socket) {
  // No socket is ever used with a loop here, so none can reach this.
  (void)loop;
  (void)socket;
}

#endif // GCU_LOOP_SUPPORTED
