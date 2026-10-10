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
 * A thread pool built on this library's own threads, mutexes and semaphores.
 *
 * The design, and the reasoning behind each decision here, are recorded in
 * `documentation/thread-pool.md`.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/pool.h>
#include <ghoti.io/cutil/safemath.h>

/// @cond HIDDEN_SYMBOLS
#define GCU_Pool_Item GHOTIIO_CUTIL(GCU_Pool_Item)
/// @endcond

/**
 * One queued task.  Stored by value in the queue array, so enqueueing costs
 * no allocation of its own once the array has room.
 */
typedef struct {
  GCU_Pool_Task task;             // The work.
  void * ctx;                     // Handed to the task.
  GCU_Pool_Complete on_complete;  // Called after it returns, or NULL.
  void * user_data;               // Handed to the callback.
} GCU_Pool_Item;

/**
 * The default prefix for worker thread names.
 */
static const char GCU_POOL_DEFAULT_PREFIX[] = "gcu-pool";

/**
 * Compact the queue once the dead prefix left by popping reaches half of it,
 * so that steady enqueue/pop traffic does not grow the array without bound.
 *
 * Must be called with the mutex held.
 */
static void gcu_pool_compact(GCU_Pool * pool) {
  size_t live = pool->queue.count - pool->queue_head;

  if (pool->queue_head == 0 || pool->queue_head < pool->queue.count / 2) {
    return;
  }

  if (live) {
    memmove(pool->queue.data,
      (GCU_Pool_Item *)pool->queue.data + pool->queue_head,
      live * sizeof(GCU_Pool_Item));
  }

  // Shrinking never reallocates, so this cannot fail in a way that matters;
  // if it somehow did, the queue would be left as it was and simply compact
  // again later.
  if (gcu_array_resize(&pool->queue, live)) {
    pool->queue_head = 0;
  }
}

/**
 * Release every thread blocked in gcu_pool_wait() if the pool has gone idle.
 *
 * Posting once per waiter, and zeroing the count in the same critical
 * section that posts, is what keeps a second waiter from hanging and keeps a
 * surplus post from leaking to a later wait.  Both are failure modes the
 * `compress` implementation has.
 *
 * Must be called with the mutex held.
 */
static void gcu_pool_release_waiters_if_idle(GCU_Pool * pool) {
  if (pool->queue_head != pool->queue.count || pool->active
      || !pool->waiters) {
    return;
  }

  for (size_t i = 0; i < pool->waiters; ++i) {
    gcu_semaphore_signal(&pool->idle);
  }
  pool->waiters = 0;
}

/**
 * Run one task and account for it.  Called with no lock held.
 */
static void gcu_pool_run(GCU_Pool * pool, GCU_Pool_Item * item) {
  int status = item->task(item->ctx);

  if (item->on_complete) {
    item->on_complete(item->ctx, status, item->user_data);
  }

  GCU_MUTEX_LOCK(pool->mutex);
  --pool->active;
  if (status && !pool->first_error) {
    pool->first_error = status;
  }
  gcu_pool_release_waiters_if_idle(pool);
  GCU_MUTEX_UNLOCK(pool->mutex);
}

/**
 * Whether the calling thread's id is in the published worker list.
 *
 * Must be called with the mutex held.  An id is published only after that
 * worker has been named, so being listed is the dequeue gate for both.
 */
static bool gcu_pool_lists_current(const GCU_Pool * pool) {
  GCU_Thread self = gcu_thread_get_current_id();

  for (size_t i = 0; i < pool->thread_count; ++i) {
    if (pool->threads[i] == self) {
      return true;
    }
  }

  return false;
}

/**
 * The worker loop.
 *
 * The order of the checks is the whole correctness argument: the queue is
 * examined *before* shutdown is honoured, and shutdown *before* `retire`, so
 * a worker woken by teardown still drains whatever is waiting and does not
 * take the resize exit once shutdown has been requested.  Reversing the first
 * two is what makes `compress` discard queued jobs at destroy.  `retire` is a
 * count of workers that should leave, decremented only by a worker that is
 * leaving.  It is never a comparison against a target count.
 */
static GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION
gcu_pool_worker(GCU_THREAD_FUNC_ARG_T arg) {
  GCU_Pool * pool = (GCU_Pool *)arg;

  while (true) {
    // One wakeup per enqueued task, plus one per worker at shutdown, plus one
    // per worker a resize wants to retire.  They are indistinguishable here,
    // which is why the decision below is made by looking at the queue rather
    // than by trusting the wakeup.
    int woke = gcu_semaphore_wait(&pool->work);

    GCU_MUTEX_LOCK(pool->mutex);

    // The wrapper posts started, and gcu_thread_create returns from that post,
    // before the wrapper calls this function.  This thread can still run
    // before its id is published.  A wakeup consumed in that window is put
    // back only when the wait took a token; the task stays queued for a
    // worker that is allowed to take it.
    if (!gcu_pool_lists_current(pool)) {
      GCU_MUTEX_UNLOCK(pool->mutex);
      if (woke == 0) {
        gcu_semaphore_signal(&pool->work);
      }
      continue;
    }

    if (pool->queue_head < pool->queue.count) {
      GCU_Pool_Item item =
        *(GCU_Pool_Item *)gcu_array_at(&pool->queue, pool->queue_head);
      ++pool->queue_head;
      ++pool->active;
      gcu_pool_compact(pool);

      // The slot frees as soon as the task leaves the queue, not when it
      // finishes, so a bounded queue measures what is waiting rather than
      // what is in flight.
      //
      // Posted while the mutex is still held, so that a task leaving the
      // queue and its slot becoming free are one indivisible step.  Posting
      // after the unlock left a window in which the pool reported the task as
      // running while the slot it vacated was still unavailable, so no
      // observation of the pool told a caller when it could enqueue again.
      // Posting a semaphore does not block, so holding the mutex across it
      // costs only the woken thread's wait for a lock this thread is about to
      // release.
      if (pool->max_queued) {
        gcu_semaphore_signal(&pool->slots);
      }

      GCU_MUTEX_UNLOCK(pool->mutex);

      gcu_pool_run(pool, &item);
      continue;
    }

    if (pool->shutting_down) {
      GCU_MUTEX_UNLOCK(pool->mutex);
      break;
    }

    // Empty queue and shutdown not requested.  Leave only when this pool has
    // asked someone to leave.  The id is published before the post, so the
    // resizer can join it as soon as the post is observed.
    if (pool->retire > 0) {
      --pool->retire;
      pool->retired_ids[pool->retired] = gcu_thread_get_current_id();
      ++pool->retired;
      GCU_MUTEX_UNLOCK(pool->mutex);
      gcu_semaphore_signal(&pool->retired_wake);
      break;
    }

    // A surplus wakeup with nothing to do and no shutdown: go back to sleep.
    GCU_MUTEX_UNLOCK(pool->mutex);
  }

  return (GCU_THREAD_FUNC_RETURN_T)0;
}

/**
 * Name one worker, best-effort.  A platform that refuses the name, or a
 * thread module that cannot find the record, is not a reason to fail.
 */
static void gcu_pool_name_worker(
  GCU_Thread thread, const char * prefix, size_t index) {
  // Prefix, separator, the widest size_t (20 digits) and the terminator.
  // The platform truncates further if it must; this only has to keep
  // snprintf from having to.
  char name[GCU_POOL_NAME_PREFIX_MAX + 22];

  snprintf(name, sizeof(name), "%.*s-%zu",
    (int)GCU_POOL_NAME_PREFIX_MAX, prefix, index);
  gcu_thread_set_name(thread, name);
}

/**
 * Tear a running pool down.  Shared by the drain and abandon paths, which
 * differ only in whether the queue survives long enough to be run.
 */
static void gcu_pool_shutdown(GCU_Pool * pool, bool discard_queue) {
  GCU_MUTEX_LOCK(pool->mutex);

  pool->shutting_down = true;

  // Snapshot the list under the same lock that publishes it.  Resize is not
  // interlocked with destroy; this is the list of workers still recorded, and
  // the wakeups below are one per entry of it.
  size_t live = pool->thread_count;
  GCU_Thread * workers = pool->threads;

  if (discard_queue) {
    // Emptying the queue here, before the workers are woken, is the whole of
    // the difference between abandoning and draining: the workers then find
    // nothing to do and stop.
    gcu_array_clear(&pool->queue);
    pool->queue_head = 0;
  }

  // Release anyone blocked for a queue slot, or teardown waits for a producer
  // that is itself waiting for a worker that is stopping.  They re-check
  // shutting_down on the way out and report failure.
  for (size_t i = 0; i < pool->slot_waiters; ++i) {
    gcu_semaphore_signal(&pool->slots);
  }

  GCU_MUTEX_UNLOCK(pool->mutex);

  // One guaranteed wakeup each, so that every worker reaches the shutdown
  // test even if the queue is empty.  Shutdown stays ahead of retire: a
  // worker that sees shutting_down set does not take the resize exit.
  for (size_t i = 0; i < live; ++i) {
    gcu_semaphore_signal(&pool->work);
  }

  for (size_t i = 0; i < live; ++i) {
    gcu_thread_join(workers[i]);
  }

  // Release the sleepers in gcu_pool_wait().  Unconditionally, not through
  // gcu_pool_release_waiters_if_idle(): that one posts only if the pool looks
  // idle, and teardown must not make releasing them conditional on anything.
  // A queue that came out non-empty -- a producer that appended in the window
  // before shutting_down was set, say -- would otherwise leave a sleeper
  // unwoken, and the drain below would wait for it forever.  Trading a rare
  // use-after-free for a rare hang is not a fix.
  //
  // Before the drain, necessarily: the drain waits for exactly the threads
  // this releases.
  GCU_MUTEX_LOCK(pool->mutex);
  for (size_t i = 0; i < pool->waiters; ++i) {
    gcu_semaphore_signal(&pool->idle);
  }
  pool->waiters = 0;
  GCU_MUTEX_UNLOCK(pool->mutex);

  // Now wait for everyone teardown has released to leave the pool's memory,
  // because the caller is about to free it.  Both kinds of released thread
  // re-take this mutex on their way out and read the pool after doing so, and
  // the mutex, the semaphore each just woke from and the pool itself are all
  // about to be destroyed.
  //
  // `in_flight` and not `slot_waiters`, which is what this waited on before
  // and which answers a different question.  `slot_waiters` counts threads
  // blocked on the `slots` semaphore, so it drops to zero the moment a
  // producer is woken -- including when a *worker* frees a slot, with no
  // shutdown in sight.  That producer then went on to take the mutex and
  // append to the queue while teardown, seeing nobody waiting, freed the pool
  // beneath it.  Reproduced as a heap-use-after-free at the producer's next
  // GCU_MUTEX_LOCK, against the free in gcu_pool_abandon().  It also counted
  // no gcu_pool_wait() sleepers at all, and teardown releases those too.
  //
  // A thread decrements `in_flight` under this mutex and touches nothing
  // afterwards, so a count of zero observed under it means they are all done.
  GCU_MUTEX_LOCK(pool->mutex);
  while (pool->in_flight) {
    GCU_MUTEX_UNLOCK(pool->mutex);
    gcu_thread_yield();
    GCU_MUTEX_LOCK(pool->mutex);
  }
  GCU_MUTEX_UNLOCK(pool->mutex);
}

/**
 * Release everything a successfully created pool owns.
 */
static void gcu_pool_free_parts(GCU_Pool * pool) {
  gcu_array_destroy_in_place(&pool->queue);

  if (!pool->is_inline) {
    if (pool->max_queued) {
      gcu_semaphore_destroy(&pool->slots);
    }
    gcu_semaphore_destroy(&pool->retired_wake);
    gcu_semaphore_destroy(&pool->idle);
    gcu_semaphore_destroy(&pool->work);
    gcu_allocator_free(pool->allocator, pool->retired_ids);
    gcu_allocator_free(pool->allocator, pool->threads);
  }

  GCU_MUTEX_DESTROY(pool->mutex);
}

bool gcu_pool_create_in_place(
  GCU_Pool * pool, const GCU_Pool_Config * config) {
  if (!pool) {
    return false;
  }

  memset(pool, 0, sizeof(GCU_Pool));

  const GCU_Allocator * allocator = config && config->allocator
    ? config->allocator
    : gcu_allocator_default();
  size_t requested = config ? config->thread_count : GCU_POOL_THREADS_AUTO;
  const char * prefix = config && config->name_prefix
    ? config->name_prefix
    : GCU_POOL_DEFAULT_PREFIX;

  if (requested == GCU_POOL_THREADS_AUTO) {
    // Documented as never less than 1, so this needs no floor of its own.
    requested = gcu_thread_get_num_processors();
  }

  pool->allocator = allocator;
  pool->is_inline = requested == 0;
  pool->thread_count = pool->is_inline ? 0 : requested;
  pool->max_queued = config && !pool->is_inline ? config->max_queued : 0;
  // Copied, so the caller's pointer need only be valid for this call, and a
  // later grow still has the prefix.
  snprintf(pool->name_prefix, sizeof(pool->name_prefix), "%.*s",
    (int)GCU_POOL_NAME_PREFIX_MAX, prefix);

  if (GCU_MUTEX_CREATE(pool->mutex) != 0) {
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  if (!gcu_array_create_in_place(
      &pool->queue, sizeof(GCU_Pool_Item), 0, allocator)) {
    GCU_MUTEX_DESTROY(pool->mutex);
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  if (pool->is_inline) {
    return true;
  }

  size_t bytes;
  if (!gcu_safe_mul_size(pool->thread_count, sizeof(GCU_Thread), &bytes)) {
    gcu_array_destroy_in_place(&pool->queue);
    GCU_MUTEX_DESTROY(pool->mutex);
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  pool->threads =
    gcu_allocator_calloc(allocator, pool->thread_count, sizeof(GCU_Thread));
  if (!pool->threads) {
    gcu_array_destroy_in_place(&pool->queue);
    GCU_MUTEX_DESTROY(pool->mutex);
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  if (gcu_semaphore_create(&pool->work, 0) != 0) {
    gcu_allocator_free(allocator, pool->threads);
    gcu_array_destroy_in_place(&pool->queue);
    GCU_MUTEX_DESTROY(pool->mutex);
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  if (gcu_semaphore_create(&pool->idle, 0) != 0) {
    gcu_semaphore_destroy(&pool->work);
    gcu_allocator_free(allocator, pool->threads);
    gcu_array_destroy_in_place(&pool->queue);
    GCU_MUTEX_DESTROY(pool->mutex);
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  if (gcu_semaphore_create(&pool->retired_wake, 0) != 0) {
    gcu_semaphore_destroy(&pool->idle);
    gcu_semaphore_destroy(&pool->work);
    gcu_allocator_free(allocator, pool->threads);
    gcu_array_destroy_in_place(&pool->queue);
    GCU_MUTEX_DESTROY(pool->mutex);
    memset(pool, 0, sizeof(GCU_Pool));
    return false;
  }

  if (pool->max_queued) {
    // A semaphore counts with an int, so a limit that will not fit cannot be
    // honoured and is refused rather than silently narrowed.
    if (pool->max_queued > (size_t)__INT_MAX__
        || gcu_semaphore_create(&pool->slots, (int)pool->max_queued) != 0) {
      gcu_semaphore_destroy(&pool->retired_wake);
      gcu_semaphore_destroy(&pool->idle);
      gcu_semaphore_destroy(&pool->work);
      gcu_allocator_free(allocator, pool->threads);
      gcu_array_destroy_in_place(&pool->queue);
      GCU_MUTEX_DESTROY(pool->mutex);
      memset(pool, 0, sizeof(GCU_Pool));
      return false;
    }
  }

  for (size_t i = 0; i < pool->thread_count; ++i) {
    // Into a local, not threads[i].  The new thread has entered
    // gcu_pool_worker before this returns, and it must not dequeue until the
    // id is listed and the name has been applied.  thread_count is already
    // the full count, so an unpublished slot stays 0 and does not match.
    GCU_Thread id;
    if (gcu_thread_create(&id, gcu_pool_worker, pool) != 0) {
      // Stop the workers that did start before unwinding, so that none of
      // them outlives the memory it is reading.
      size_t started = i;

      GCU_MUTEX_LOCK(pool->mutex);
      pool->shutting_down = true;
      GCU_MUTEX_UNLOCK(pool->mutex);

      for (size_t j = 0; j < started; ++j) {
        gcu_semaphore_signal(&pool->work);
      }
      for (size_t j = 0; j < started; ++j) {
        gcu_thread_join(pool->threads[j]);
      }

      gcu_pool_free_parts(pool);
      memset(pool, 0, sizeof(GCU_Pool));
      return false;
    }

    gcu_pool_name_worker(id, pool->name_prefix, i);

    GCU_MUTEX_LOCK(pool->mutex);
    pool->threads[i] = id;
    GCU_MUTEX_UNLOCK(pool->mutex);
  }

  return true;
}

GCU_Pool * gcu_pool_create(const GCU_Pool_Config * config) {
  const GCU_Allocator * allocator = config && config->allocator
    ? config->allocator
    : gcu_allocator_default();

  GCU_Pool * pool = gcu_allocator_malloc(allocator, sizeof(GCU_Pool));
  if (!pool) {
    return NULL;
  }

  if (!gcu_pool_create_in_place(pool, config)) {
    gcu_allocator_free(allocator, pool);
    return NULL;
  }

  return pool;
}

/**
 * Drop published leavers out of the worker list.
 *
 * Must be called with the mutex held, after every leaver in `retired_ids`
 * has been joined.
 */
static void gcu_pool_remove_retired(GCU_Pool * pool) {
  size_t kept = 0;

  for (size_t i = 0; i < pool->thread_count; ++i) {
    bool gone = false;

    for (size_t j = 0; j < pool->retired; ++j) {
      if (pool->threads[i] == pool->retired_ids[j]) {
        gone = true;
        break;
      }
    }

    if (!gone) {
      pool->threads[kept++] = pool->threads[i];
    }
  }

  pool->thread_count = kept;
}

/**
 * Wait until one leaving worker has posted.
 *
 * A POSIX wait is restarted only when it fails with EINTR.  Any other
 * failure, including a Windows wait that does not set errno, is final: the
 * caller returns false instead of spinning.
 */
static bool gcu_pool_wait_retired(GCU_Pool * pool) {
#ifdef _WIN32
  return gcu_semaphore_wait(&pool->retired_wake) == 0;
#else
  for (;;) {
    if (gcu_semaphore_wait(&pool->retired_wake) == 0) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
#endif
}

/**
 * Join any published leaver the caller has not joined yet, drop those ids
 * from the worker list, and forget the retire request.
 *
 * Workers already joined stay joined.  Workers that have not published stay
 * listed, so destroy still joins them.  `joined` is how many posts the caller
 * has already consumed.
 */
static void gcu_pool_shrink_settle(GCU_Pool * pool, size_t joined) {
  GCU_MUTEX_LOCK(pool->mutex);
  size_t published = pool->retired;
  // No further worker takes the resize exit.  Anyone already inside the
  // retire section has published; that write happens in the same critical
  // section as the decrement.
  pool->retire = 0;
  GCU_MUTEX_UNLOCK(pool->mutex);

  for (size_t n = joined; n < published; ++n) {
    GCU_Thread id;

    GCU_MUTEX_LOCK(pool->mutex);
    id = pool->retired_ids[n];
    GCU_MUTEX_UNLOCK(pool->mutex);
    // Outside the mutex.  Joining here, before waiting on anyone else, is
    // what keeps a reused thread id from meeting an unjoined record.
    gcu_thread_join(id);
  }

  // Each of those joins returned only after its post.  Consume the posts the
  // wait loop did not, so a later shrink does not observe a stale wakeup.
  // A POSIX trywait that fails with EINTR did not take the permit; retry it.
  // A Windows trywait does not set errno, so one attempt is the whole try.
  for (size_t n = joined; n < published; ++n) {
#ifdef _WIN32
    gcu_semaphore_trywait(&pool->retired_wake);
#else
    for (;;) {
      if (gcu_semaphore_trywait(&pool->retired_wake) == 0) {
        break;
      }
      if (errno != EINTR) {
        break;
      }
    }
#endif
  }

  GCU_MUTEX_LOCK(pool->mutex);
  gcu_pool_remove_retired(pool);
  GCU_Thread * ids = pool->retired_ids;
  pool->retired_ids = NULL;
  pool->retired = 0;
  pool->retire = 0;
  GCU_MUTEX_UNLOCK(pool->mutex);
  gcu_allocator_free(pool->allocator, ids);
}

/**
 * Start workers until the pool has `target` of them.
 *
 * A thread is named and then published before it may dequeue.  If creating
 * one fails, the workers already published stay published: the count reached
 * is what gcu_pool_count_threads() reports, and this returns false.
 */
static bool gcu_pool_grow(GCU_Pool * pool, size_t target) {
  size_t bytes;
  if (!gcu_safe_mul_size(target, sizeof(GCU_Thread), &bytes)) {
    return false;
  }

  GCU_Thread * bigger =
    gcu_allocator_calloc(pool->allocator, target, sizeof(GCU_Thread));
  if (!bigger) {
    return false;
  }

  GCU_MUTEX_LOCK(pool->mutex);
  size_t have = pool->thread_count;
  memcpy(bigger, pool->threads, have * sizeof(GCU_Thread));
  GCU_Thread * old = pool->threads;
  pool->threads = bigger;
  GCU_MUTEX_UNLOCK(pool->mutex);
  gcu_allocator_free(pool->allocator, old);

  for (size_t i = have; i < target; ++i) {
    GCU_Thread id;

    if (gcu_thread_create(&id, gcu_pool_worker, pool) != 0) {
      return false;
    }

    gcu_pool_name_worker(id, pool->name_prefix, i);

    GCU_MUTEX_LOCK(pool->mutex);
    pool->threads[i] = id;
    pool->thread_count = i + 1;
    GCU_MUTEX_UNLOCK(pool->mutex);
  }

  return true;
}

/**
 * Retire workers until `target` remain.
 *
 * The retired-id array is allocated before anything is asked to leave, so a
 * failure there leaves the count unchanged.  Each leaver is joined as soon
 * as its publication is observed, and only then does this wait for the next.
 */
static bool gcu_pool_shrink(GCU_Pool * pool, size_t target) {
  size_t current = gcu_pool_count_threads(pool);
  size_t surplus = current - target;
  size_t bytes;

  if (!gcu_safe_mul_size(surplus, sizeof(GCU_Thread), &bytes)) {
    return false;
  }

  GCU_Thread * ids =
    gcu_allocator_calloc(pool->allocator, surplus, sizeof(GCU_Thread));
  if (!ids) {
    return false;
  }

  GCU_MUTEX_LOCK(pool->mutex);
  pool->retired_ids = ids;
  pool->retired = 0;
  pool->retire = surplus;
  GCU_MUTEX_UNLOCK(pool->mutex);

  // One wakeup per worker that should leave.  A wakeup spent on a queued
  // task does not decrement retire; the extra post stays until the queue is
  // empty, which is what holds this call open while work remains.
  for (size_t i = 0; i < surplus; ++i) {
    gcu_semaphore_signal(&pool->work);
  }

  for (size_t n = 0; n < surplus; ++n) {
    if (!gcu_pool_wait_retired(pool)) {
      gcu_pool_shrink_settle(pool, n);
      return false;
    }

    GCU_Thread id;
    GCU_MUTEX_LOCK(pool->mutex);
    id = pool->retired_ids[n];
    GCU_MUTEX_UNLOCK(pool->mutex);
    gcu_thread_join(id);
  }

  gcu_pool_shrink_settle(pool, surplus);

  // The last surplus worker committed to leave only after it saw an empty
  // queue.  Another thread can enqueue after that.  Wait until the queue is
  // empty again before returning success.  Do not wait for active to reach
  // zero: a worker that remains may still be inside a task.
  for (;;) {
    GCU_MUTEX_LOCK(pool->mutex);
    bool empty = pool->queue_head == pool->queue.count;
    GCU_MUTEX_UNLOCK(pool->mutex);
    if (empty) {
      break;
    }
    gcu_thread_yield();
  }

  return true;
}

bool gcu_pool_set_thread_count(GCU_Pool * pool, size_t thread_count) {
  if (!pool || !pool->allocator || pool->is_inline || thread_count == 0) {
    return false;
  }

  size_t target = thread_count == GCU_POOL_THREADS_AUTO
    ? gcu_thread_get_num_processors()
    : thread_count;

  GCU_MUTEX_LOCK(pool->mutex);
  // A task on this pool must not join its own worker, or rewrite the list
  // the grow below is filling in.
  if (gcu_pool_lists_current(pool)) {
    GCU_MUTEX_UNLOCK(pool->mutex);
    return false;
  }
  size_t current = pool->thread_count;
  GCU_MUTEX_UNLOCK(pool->mutex);

  if (target == current) {
    return true;
  }

  return target > current
    ? gcu_pool_grow(pool, target)
    : gcu_pool_shrink(pool, target);
}

void gcu_pool_destroy_in_place(GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return;
  }

  if (!pool->is_inline) {
    gcu_pool_shutdown(pool, false);
  }

  gcu_pool_free_parts(pool);
  memset(pool, 0, sizeof(GCU_Pool));
}

void gcu_pool_abandon_in_place(GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return;
  }

  if (!pool->is_inline) {
    gcu_pool_shutdown(pool, true);
  }

  gcu_pool_free_parts(pool);
  memset(pool, 0, sizeof(GCU_Pool));
}

void gcu_pool_destroy(GCU_Pool * pool) {
  if (!pool) {
    return;
  }

  const GCU_Allocator * allocator = pool->allocator;
  gcu_pool_destroy_in_place(pool);
  gcu_allocator_free(allocator, pool);
}

void gcu_pool_abandon(GCU_Pool * pool) {
  if (!pool) {
    return;
  }

  const GCU_Allocator * allocator = pool->allocator;
  gcu_pool_abandon_in_place(pool);
  gcu_allocator_free(allocator, pool);
}

/**
 * The body behind all four enqueue entry points.
 */
static bool gcu_pool_enqueue_common(GCU_Pool * pool, GCU_Pool_Task task,
  void * ctx, GCU_Pool_Complete on_complete, void * user_data, bool blocking) {
  if (!pool || !task || !pool->allocator) {
    return false;
  }

  GCU_Pool_Item item = {
    .task = task,
    .ctx = ctx,
    .on_complete = on_complete,
    .user_data = user_data,
  };

  // Whether this call is one of the threads teardown counts; see the tail.
  bool counted = false;

  if (pool->is_inline) {
    int status = item.task(item.ctx);

    if (item.on_complete) {
      item.on_complete(item.ctx, status, item.user_data);
    }

    GCU_MUTEX_LOCK(pool->mutex);
    if (status && !pool->first_error) {
      pool->first_error = status;
    }
    GCU_MUTEX_UNLOCK(pool->mutex);
    return true;
  }

  if (pool->max_queued) {
    if (blocking) {
      GCU_MUTEX_LOCK(pool->mutex);
      if (pool->shutting_down) {
        GCU_MUTEX_UNLOCK(pool->mutex);
        return false;
      }
      ++pool->slot_waiters;
      // Counted from here because teardown will release this thread whether
      // or not a slot ever frees, which makes its exit teardown's business.
      ++pool->in_flight;
      counted = true;
      GCU_MUTEX_UNLOCK(pool->mutex);

      gcu_semaphore_wait(&pool->slots);

      GCU_MUTEX_LOCK(pool->mutex);
      --pool->slot_waiters;
      if (pool->shutting_down) {
        --pool->in_flight;
        GCU_MUTEX_UNLOCK(pool->mutex);
        return false;
      }
      GCU_MUTEX_UNLOCK(pool->mutex);
    }
    else if (gcu_semaphore_trywait(&pool->slots) != 0) {
      return false;
    }
  }

  GCU_MUTEX_LOCK(pool->mutex);
  const bool queued =
    !pool->shutting_down && gcu_array_append(&pool->queue, &item);
  GCU_MUTEX_UNLOCK(pool->mutex);

  if (queued) {
    gcu_semaphore_signal(&pool->work);
  }
  else if (pool->max_queued) {
    // Hand the slot back; this producer is not going to use it.
    gcu_semaphore_signal(&pool->slots);
  }

  // Last, because everything above touches the pool and teardown reads this
  // count to decide that nothing is left inside it.  This is the whole reason
  // the three tails above were folded into one: each of them used to return
  // straight out, so a producer that got past the shutting_down check went on
  // touching a pool that no longer counted it -- and teardown, seeing nobody
  // waiting for a slot, freed the pool underneath it.
  if (counted) {
    GCU_MUTEX_LOCK(pool->mutex);
    --pool->in_flight;
    GCU_MUTEX_UNLOCK(pool->mutex);
  }

  return queued;
}

bool gcu_pool_enqueue(GCU_Pool * pool, GCU_Pool_Task task, void * ctx) {
  return gcu_pool_enqueue_common(pool, task, ctx, NULL, NULL, false);
}

bool gcu_pool_enqueue_cb(GCU_Pool * pool, GCU_Pool_Task task, void * ctx,
  GCU_Pool_Complete on_complete, void * user_data) {
  return gcu_pool_enqueue_common(
    pool, task, ctx, on_complete, user_data, false);
}

bool gcu_pool_enqueue_wait(GCU_Pool * pool, GCU_Pool_Task task, void * ctx) {
  return gcu_pool_enqueue_common(pool, task, ctx, NULL, NULL, true);
}

bool gcu_pool_enqueue_wait_cb(GCU_Pool * pool, GCU_Pool_Task task, void * ctx,
  GCU_Pool_Complete on_complete, void * user_data) {
  return gcu_pool_enqueue_common(
    pool, task, ctx, on_complete, user_data, true);
}

int gcu_pool_wait(GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return 0;
  }

  GCU_MUTEX_LOCK(pool->mutex);

  // `shutting_down` belongs in this test, and not only because there is
  // nothing left to wait for.  Teardown releases the sleepers it finds and
  // then waits for them to leave; a thread that registers *after* that
  // release is never posted, so without this it would park on a semaphore
  // nobody will signal again.  Unfixed, that was a lost wakeup and a hung
  // caller; with the drain below it in place it would have been a hung
  // teardown.  Registering requires this mutex and teardown sets the flag
  // under it, so the two cannot interleave.
  if (pool->is_inline || pool->shutting_down
      || (pool->queue_head == pool->queue.count && !pool->active)) {
    int result = pool->first_error;
    GCU_MUTEX_UNLOCK(pool->mutex);
    return result;
  }

  ++pool->waiters;
  // As in the blocking enqueue: teardown releases these sleepers on its way
  // out, so it owns their exit and has to wait for it.
  ++pool->in_flight;
  GCU_MUTEX_UNLOCK(pool->mutex);

  gcu_semaphore_wait(&pool->idle);

  GCU_MUTEX_LOCK(pool->mutex);
  int result = pool->first_error;
  --pool->in_flight;
  GCU_MUTEX_UNLOCK(pool->mutex);

  return result;
}

void gcu_pool_clear_error(GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return;
  }

  GCU_MUTEX_LOCK(pool->mutex);
  pool->first_error = 0;
  GCU_MUTEX_UNLOCK(pool->mutex);
}

size_t gcu_pool_count_queued(const GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return 0;
  }

  GCU_Pool * mutable_pool = (GCU_Pool *)pool;
  GCU_MUTEX_LOCK(mutable_pool->mutex);
  size_t result = pool->queue.count - pool->queue_head;
  GCU_MUTEX_UNLOCK(mutable_pool->mutex);

  return result;
}

size_t gcu_pool_count_active(const GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return 0;
  }

  GCU_Pool * mutable_pool = (GCU_Pool *)pool;
  GCU_MUTEX_LOCK(mutable_pool->mutex);
  size_t result = pool->active;
  GCU_MUTEX_UNLOCK(mutable_pool->mutex);

  return result;
}

size_t gcu_pool_count_threads(const GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return 0;
  }

  // thread_count is written under the mutex by a resize.  Sampling it the
  // same way the other counts are sampled keeps the reader correct across
  // that update.
  GCU_Pool * mutable_pool = (GCU_Pool *)pool;
  GCU_MUTEX_LOCK(mutable_pool->mutex);
  size_t result = pool->thread_count;
  GCU_MUTEX_UNLOCK(mutable_pool->mutex);

  return result;
}

bool gcu_pool_is_inline(const GCU_Pool * pool) {
  return pool ? pool->is_inline : true;
}

bool gcu_pool_is_shutting_down(const GCU_Pool * pool) {
  if (!pool || !pool->allocator) {
    return true;
  }

  GCU_Pool * mutable_pool = (GCU_Pool *)pool;
  GCU_MUTEX_LOCK(mutable_pool->mutex);
  bool result = pool->shutting_down;
  GCU_MUTEX_UNLOCK(mutable_pool->mutex);

  return result;
}
