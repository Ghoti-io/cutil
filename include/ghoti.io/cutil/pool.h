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
 * A thread pool: worker threads drawing tasks from a shared FIFO queue.  The
 * worker count is chosen at creation and changed with
 * gcu_pool_set_thread_count().  GCU_Managed_Pool adds a manager thread so any
 * caller, including a worker, can request a new count without joining.
 *
 * The design and the reasoning behind each decision are recorded in
 * `documentation/thread-pool.md`.
 */

#ifndef GHOTI_IO_GCU_POOL_H
#define GHOTI_IO_GCU_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/array.h>
#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/cutil/semaphore.h>
#include <ghoti.io/cutil/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Passed as `thread_count` to request one worker per logical processor.
 */
#define GCU_POOL_THREADS_AUTO ((size_t)-1)

/**
 * The longest `name_prefix` that is used in full.  A longer prefix is
 * truncated to this many characters.
 *
 * POSIX caps a thread name at 15 characters plus the terminator, and the
 * worker index is appended to the prefix, so the prefix cannot have all of
 * that budget.  See gcu_thread_set_name().
 */
#define GCU_POOL_NAME_PREFIX_MAX 8

/**
 * A thread pool.
 *
 * The layout is published only so that a caller may embed one in a structure
 * of its own and use the `_in_place` calls.  **Every field is private.**
 * Read the counts through gcu_pool_count_queued() and its siblings, which
 * take the lock; reading a field directly races the workers.
 */
typedef struct GCU_Pool GCU_Pool;

/// @cond HIDDEN_SYMBOLS
struct GCU_Pool {
  const GCU_Allocator * allocator; ///< Private.  Allocator for all memory.
  bool is_inline;                  ///< Private.  Tasks run at enqueue time.
  size_t max_queued;               ///< Private.  Queue limit; 0 = unbounded.
  /// Private.  Worker-name prefix copied at create.  New workers keep using
  /// it across a resize.
  char name_prefix[GCU_POOL_NAME_PREFIX_MAX + 1];

  GCU_MUTEX_T mutex;               ///< Private.  Covers thread_count, threads,
                                   ///<   and every field below.
  size_t thread_count;             ///< Private.  Worker count; 0 when inline.
  GCU_Thread * threads;            ///< Private.  Worker ids, or NULL.
  size_t retire;                   ///< Private.  Workers that should leave.
  GCU_Thread * retired_ids;        ///< Private.  Ids published by leavers.
  size_t retired;                  ///< Private.  How many leavers published.

  GCU_Array queue;                 ///< Private.  FIFO of queued tasks.
  size_t queue_head;               ///< Private.  Index of the next task.
  size_t active;                   ///< Private.  Tasks currently running.
  int first_error;                 ///< Private.  First non-zero status seen.
  bool shutting_down;              ///< Private.  No new work accepted.
  size_t waiters;                  ///< Private.  Threads in gcu_pool_wait().
  size_t slot_waiters;             ///< Private.  Threads awaiting a slot.
  size_t in_flight;                ///< Private.  External threads that have
                                   ///<   entered this pool and not yet left
                                   ///<   it.  Teardown waits for zero.

  GCU_Semaphore work;              ///< Private.  Outstanding worker wakeups.
  GCU_Semaphore idle;              ///< Private.  Releases waiters when idle.
  GCU_Semaphore slots;             ///< Private.  Free slots when bounded.
  GCU_Semaphore retired_wake;      ///< Private.  Posted when a leaver
                                   ///<   publishes its id.
};
/// @endcond

/**
 * A unit of work.
 *
 * @param ctx The context pointer supplied when the task was enqueued.
 * @return `0` on success, or any non-zero status on failure.  A non-zero
 *   status is recorded by the pool and reported by gcu_pool_wait(); it does
 *   not stop the pool or affect any other task.
 */
typedef int (*GCU_Pool_Task)(void * ctx);

/**
 * Called after a task returns.
 *
 * Runs on the worker thread that ran the task, outside every pool lock, after
 * the task returns and before the task is counted as complete.  It must not
 * call back into the pool that invoked it.
 *
 * @param ctx The context pointer the task was enqueued with.
 * @param status The value the task returned.
 * @param user_data The data supplied alongside the callback.
 */
typedef void (*GCU_Pool_Complete)(void * ctx, int status, void * user_data);

/**
 * Pool configuration.  A `NULL` config selects every default.
 */
typedef struct GCU_Pool_Config {
  /**
   * Worker threads to create.
   *
   * `0` selects inline mode, in which no threads are created and each task
   * runs on the calling thread at enqueue time.  `GCU_POOL_THREADS_AUTO`
   * selects one worker per logical processor.  Any other value is taken
   * literally: `1` means one worker thread, not inline.
   */
  size_t thread_count;

  /**
   * The most tasks that may be queued at once, or `0` for no limit.
   *
   * When a limit is set, gcu_pool_enqueue() fails once the queue is full and
   * gcu_pool_enqueue_wait() blocks until a slot frees.  Ignored in inline
   * mode, which never queues.
   */
  size_t max_queued;

  /**
   * Prefix for worker thread names, or `NULL` for `"gcu-pool"`.
   *
   * Workers are named `<prefix>-<index>`.  Truncated to
   * #GCU_POOL_NAME_PREFIX_MAX characters.  Naming is best-effort: a platform
   * that refuses the name does not fail the create.
   */
  const char * name_prefix;

  /**
   * The allocator, or `NULL` for gcu_allocator_default().  It must outlive
   * the pool.
   */
  const GCU_Allocator * allocator;
} GCU_Pool_Config;

/**
 * Create a pool on the heap and start its workers.
 *
 * @param config The configuration, or `NULL` for every default.
 * @return The pool, or `NULL` on failure.  Destroy it with
 *   gcu_pool_destroy().
 */
GCU_API GCU_Pool * gcu_pool_create(const GCU_Pool_Config * config);

/**
 * Create a pool in memory the caller owns, and start its workers.
 *
 * @param pool Storage for the pool.  Must not be `NULL`.
 * @param config The configuration, or `NULL` for every default.
 * @return `true` on success.  On failure the pool is left zeroed and safe to
 *   pass to gcu_pool_destroy_in_place().
 */
GCU_API bool gcu_pool_create_in_place(
  GCU_Pool * pool, const GCU_Pool_Config * config);

/**
 * Run every queued task, stop the workers, and free the pool.
 *
 * Blocks until the queue is empty and no task is running.  Nothing that was
 * successfully enqueued is discarded.  To discard the queue instead, use
 * gcu_pool_abandon().
 *
 * Must not be called while another thread is calling into the pool, with one
 * exception: threads blocked in gcu_pool_enqueue_wait() are released, return
 * `false`, and are waited for before anything is freed.  Passing `NULL` does
 * nothing.
 *
 * @param pool The pool to drain and destroy.
 */
GCU_API void gcu_pool_destroy(GCU_Pool * pool);

/**
 * As gcu_pool_destroy(), for a pool created with gcu_pool_create_in_place().
 *
 * @param pool The pool to drain and tear down.
 */
GCU_API void gcu_pool_destroy_in_place(GCU_Pool * pool);

/**
 * Discard the queued tasks, stop the workers, and free the pool.
 *
 * Tasks that have already started run to completion; tasks still queued are
 * discarded without running and without being reported. Blocks until the
 * workers have stopped.
 *
 * This loses work by design.  gcu_pool_destroy() is the ordinary teardown.
 *
 * Must not be called while another thread is calling into the pool, with the
 * same exception gcu_pool_destroy() makes for gcu_pool_enqueue_wait().
 * Passing `NULL` does nothing.
 *
 * @param pool The pool to abandon and destroy.
 */
GCU_API void gcu_pool_abandon(GCU_Pool * pool);

/**
 * As gcu_pool_abandon(), for a pool created with gcu_pool_create_in_place().
 *
 * @param pool The pool to abandon and tear down.
 */
GCU_API void gcu_pool_abandon_in_place(GCU_Pool * pool);

/**
 * Change the number of worker threads.
 *
 * Growing starts workers on the same queue.  Shrinking retires surplus
 * workers: each finishes its current task, exits on an empty queue, and this
 * call joins those exits.  Queued tasks stay queued.  A shrink does not
 * return while every worker is busy; it waits until each surplus worker has
 * finished the task it is in and exited; queued tasks are taken before a
 * worker exits, so a non-empty queue holds the call until that queue is
 * empty; workers that remain may still be inside a task when the call
 * returns.
 *
 * A worker exits for resize only after its current task returns, and only
 * when it wakes to an empty queue.  It does not exit while a task is queued,
 * and it does not take the resize exit once shutting down has been requested.
 * Enqueue may proceed during the call.  A call from a worker of this pool
 * returns `false` and leaves the count unchanged.  The call must not run
 * concurrently with destroy, abandon, or another resize.  Inline mode is
 * create-time only.
 *
 * @param pool The pool.
 * @param thread_count The worker count to reach.  `0` is refused.
 *   `GCU_POOL_THREADS_AUTO` is one worker per logical processor, never 0, as
 *   at create.
 * @return `true` when the count equals the requested count.
 *   `GCU_POOL_THREADS_AUTO` succeeds at the processor count.  `false` if
 *   @p pool is `NULL`, the pool is inline, @p thread_count is `0`, the caller
 *   is a worker of this pool, or the count could not be reached.  A failed
 *   allocation leaves the count unchanged.  A failed retired-semaphore wait
 *   may already have joined some workers and reduced the count.  A failed
 *   grow keeps the workers that started, and gcu_pool_count_threads() is the
 *   count reached.
 */
GCU_API bool gcu_pool_set_thread_count(GCU_Pool * pool, size_t thread_count);

/**
 * Enqueue a task without blocking.
 *
 * In inline mode the task runs on the calling thread before this returns.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @return `true` if the task was enqueued (or, inline, run).  `false` if the
 *   pool or task is `NULL`, the pool is shutting down, the queue is bounded
 *   and full, or memory could not be obtained.
 */
GCU_API bool gcu_pool_enqueue(
  GCU_Pool * pool, GCU_Pool_Task task, void * ctx);

/**
 * Enqueue a task with a completion callback, without blocking.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @param on_complete Called after the task returns, or `NULL`.
 * @param user_data Passed to @p on_complete.
 * @return As gcu_pool_enqueue().
 */
GCU_API bool gcu_pool_enqueue_cb(GCU_Pool * pool, GCU_Pool_Task task,
  void * ctx, GCU_Pool_Complete on_complete, void * user_data);

/**
 * Enqueue a task, waiting for room if the queue is bounded and full.
 *
 * On an unbounded queue this is exactly gcu_pool_enqueue(), since a slot is
 * always available.
 *
 * **Never call this from a task running on the same pool.**  The slot it
 * waits for can only be freed by a worker, and the caller is occupying one,
 * so the pool can deadlock against itself.  Use gcu_pool_enqueue() there.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @return `true` if the task was enqueued.  `false` on the same conditions as
 *   gcu_pool_enqueue(), and if the pool began shutting down while waiting.
 */
GCU_API bool gcu_pool_enqueue_wait(
  GCU_Pool * pool, GCU_Pool_Task task, void * ctx);

/**
 * As gcu_pool_enqueue_wait(), with a completion callback.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @param on_complete Called after the task returns, or `NULL`.
 * @param user_data Passed to @p on_complete.
 * @return As gcu_pool_enqueue_wait().
 */
GCU_API bool gcu_pool_enqueue_wait_cb(GCU_Pool * pool, GCU_Pool_Task task,
  void * ctx, GCU_Pool_Complete on_complete, void * user_data);

/**
 * Block until the queue is empty and no task is running.
 *
 * May be called from any number of threads at once; all waiters are released
 * together.
 *
 * Returning means the pool was idle at the instant the condition was
 * observed.  If other threads are still enqueueing, more work may exist by
 * the time this returns.  Stopping the producers is the caller's job.
 *
 * A pool that is being torn down is not waited on:  teardown releases the
 * threads parked here and then waits for them to leave, so one that arrived
 * after that release would park on a semaphore nobody will post again.  From
 * the moment teardown begins, this returns at once with the recorded status.
 *
 * @param pool The pool.
 * @return The first non-zero status any task has returned since the last
 *   gcu_pool_clear_error(), or `0`.  Reading it does not clear it, so every
 *   waiter sees the same answer.
 */
GCU_API int gcu_pool_wait(GCU_Pool * pool);

/**
 * Forget the recorded first error, so that gcu_pool_wait() reports `0` again.
 *
 * @param pool The pool.
 */
GCU_API void gcu_pool_clear_error(GCU_Pool * pool);

/**
 * The number of tasks waiting to start.
 *
 * An observation, not a guarantee: it may be stale before it is returned.
 *
 * @param pool The pool.
 * @return The queued task count, or `0` if @p pool is `NULL`.
 */
GCU_API size_t gcu_pool_count_queued(const GCU_Pool * pool);

/**
 * The number of tasks currently running.
 *
 * An observation, not a guarantee.
 *
 * @param pool The pool.
 * @return The running task count, or `0` if @p pool is `NULL`.
 */
GCU_API size_t gcu_pool_count_active(const GCU_Pool * pool);

/**
 * The number of worker threads.
 *
 * @param pool The pool.
 * @return The worker count, which is `0` in inline mode and for a `NULL`
 *   pool.
 */
GCU_API size_t gcu_pool_count_threads(const GCU_Pool * pool);

/**
 * Whether the pool runs tasks on the calling thread.
 *
 * @param pool The pool.
 * @return `true` in inline mode, and for a `NULL` pool.
 */
GCU_API bool gcu_pool_is_inline(const GCU_Pool * pool);

/**
 * Whether teardown has begun and the pool has stopped accepting tasks.
 *
 * @param pool The pool.
 * @return `true` once shutdown has been requested, and for a `NULL` pool.
 */
GCU_API bool gcu_pool_is_shutting_down(const GCU_Pool * pool);

/**
 * A pool plus the thread that resizes and tears it down.
 *
 * The layout is published only so that a caller may embed one and use the
 * `_in_place` calls.  **Every field is private.**  The inner pool is not
 * exposed, and its fields are not readable from here.
 */
typedef struct GCU_Managed_Pool GCU_Managed_Pool;

/// @cond HIDDEN_SYMBOLS
struct GCU_Managed_Pool {
  const GCU_Allocator * allocator; ///< Private.  Allocator for this object.
  GCU_Pool * inner;                ///< Private.  The pool the manager owns.
  GCU_Thread manager;              ///< Private.  Not a worker of `inner`.
  GCU_Semaphore wake;              ///< Private.  Counting wakeup.  A post
                                   ///<   during an apply stays counted.
  GCU_MUTEX_T state;               ///< Private.  Covers desired and stop.
  size_t desired;                  ///< Private.  Last stored request.
  int stop;                        ///< Private.  Non-zero once stop is
                                   ///<   requested.  Destroy and abandon
                                   ///<   share this word with desired.
};
/// @endcond

/**
 * Create a managed pool on the heap and start its workers and its manager.
 *
 * Does not return until the inner pool is up at the requested count, or
 * until the call has failed and left no thread running.  A `NULL` config
 * selects the same default as gcu_pool_create(), and the stored desired
 * count is #GCU_POOL_THREADS_AUTO.  `thread_count == 0` is refused: inline
 * mode is not a managed pool.
 *
 * @param config The configuration, or `NULL` for every default.
 * @return The pool, or `NULL` on failure.  Destroy it with
 *   gcu_managed_pool_destroy().
 */
GCU_API GCU_Managed_Pool * gcu_managed_pool_create(
  const GCU_Pool_Config * config);

/**
 * Create a managed pool in memory the caller owns.
 *
 * @param pool Storage for the pool.  Must not be `NULL`.
 * @param config The configuration, or `NULL` for every default.
 * @return `true` on success.  On failure the storage is left zeroed and
 *   safe to pass to gcu_managed_pool_destroy_in_place().
 */
GCU_API bool gcu_managed_pool_create_in_place(
  GCU_Managed_Pool * pool, const GCU_Pool_Config * config);

/**
 * Run every queued task, stop the workers, and free the pool.
 *
 * Sets stop, wakes the manager, and joins it.  The manager finishes the
 * resize it is already inside, then destroys the inner pool.  A shrink in
 * progress therefore holds this call until that shrink's queue is empty.
 *
 * **A worker of this pool must not call this.**  It joins the manager, and
 * the manager joins the workers.  Passing `NULL` does nothing.
 *
 * @param pool The pool to drain and destroy.
 */
GCU_API void gcu_managed_pool_destroy(GCU_Managed_Pool * pool);

/**
 * As gcu_managed_pool_destroy(), for a pool created in place.
 *
 * @param pool The pool to drain and tear down.
 */
GCU_API void gcu_managed_pool_destroy_in_place(GCU_Managed_Pool * pool);

/**
 * Discard the queued tasks, stop the workers, and free the pool.
 *
 * Sets stop, wakes the manager, and joins it.  The manager finishes the
 * resize it is already inside, then abandons the inner pool.  A shrink in
 * progress runs until its queue is empty; abandon then discards what is
 * queued after that.  A task already running still runs.
 *
 * **A worker of this pool must not call this.**  Passing `NULL` does
 * nothing.
 *
 * @param pool The pool to abandon and destroy.
 */
GCU_API void gcu_managed_pool_abandon(GCU_Managed_Pool * pool);

/**
 * As gcu_managed_pool_abandon(), for a pool created in place.
 *
 * @param pool The pool to abandon and tear down.
 */
GCU_API void gcu_managed_pool_abandon_in_place(GCU_Managed_Pool * pool);

/**
 * Record a new worker count and wake the manager.
 *
 * Stores the count, then posts the manager's semaphore, and returns.  `true`
 * means the request was stored and the post succeeded.  It does not mean the
 * live count has changed.  A later request replaces one that has not been
 * applied.  #GCU_POOL_THREADS_AUTO is stored as itself and resolved when the
 * manager applies it.
 *
 * A shrink the manager is already inside holds the manager until that
 * shrink's queue is empty.  This call does not wait for either.
 *
 * @param pool The pool.
 * @param thread_count The count to store.  `0` is refused and does not
 *   replace the stored count.
 * @return `false` if @p pool is `NULL`, @p thread_count is `0`, the post
 *   fails, or stop has already been requested.  `true` when the request is
 *   recorded.
 */
GCU_API bool gcu_managed_pool_set_thread_count(
  GCU_Managed_Pool * pool, size_t thread_count);

/**
 * The count last stored by create or gcu_managed_pool_set_thread_count().
 *
 * An observation.  #GCU_POOL_THREADS_AUTO stays that value until a later
 * request replaces it.  A failed apply does not change it.
 *
 * @param pool The pool.
 * @return The stored request, or `0` if @p pool is `NULL`.
 */
GCU_API size_t gcu_managed_pool_desired_thread_count(
  const GCU_Managed_Pool * pool);

/**
 * Enqueue a task without blocking.
 *
 * Forwards to the inner pool and keeps that pool's contract.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @return As gcu_pool_enqueue().
 */
GCU_API bool gcu_managed_pool_enqueue(
  GCU_Managed_Pool * pool, GCU_Pool_Task task, void * ctx);

/**
 * Enqueue a task with a completion callback, without blocking.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @param on_complete Called after the task returns, or `NULL`.
 * @param user_data Passed to @p on_complete.
 * @return As gcu_pool_enqueue_cb().
 */
GCU_API bool gcu_managed_pool_enqueue_cb(GCU_Managed_Pool * pool,
  GCU_Pool_Task task, void * ctx, GCU_Pool_Complete on_complete,
  void * user_data);

/**
 * Enqueue a task, waiting for room if the queue is bounded and full.
 *
 * Forwards to the inner pool.  **Never call this from a task running on
 * this pool.**
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @return As gcu_pool_enqueue_wait().
 */
GCU_API bool gcu_managed_pool_enqueue_wait(
  GCU_Managed_Pool * pool, GCU_Pool_Task task, void * ctx);

/**
 * As gcu_managed_pool_enqueue_wait(), with a completion callback.
 *
 * @param pool The pool.
 * @param task The task.  Must not be `NULL`.
 * @param ctx The context pointer to hand the task.
 * @param on_complete Called after the task returns, or `NULL`.
 * @param user_data Passed to @p on_complete.
 * @return As gcu_pool_enqueue_wait_cb().
 */
GCU_API bool gcu_managed_pool_enqueue_wait_cb(GCU_Managed_Pool * pool,
  GCU_Pool_Task task, void * ctx, GCU_Pool_Complete on_complete,
  void * user_data);

/**
 * Block until the queue is empty and no task is running.
 *
 * @param pool The pool.
 * @return As gcu_pool_wait().
 */
GCU_API int gcu_managed_pool_wait(GCU_Managed_Pool * pool);

/**
 * Forget the recorded first error.
 *
 * @param pool The pool.
 */
GCU_API void gcu_managed_pool_clear_error(GCU_Managed_Pool * pool);

/**
 * The number of tasks waiting to start.
 *
 * @param pool The pool.
 * @return As gcu_pool_count_queued().
 */
GCU_API size_t gcu_managed_pool_count_queued(const GCU_Managed_Pool * pool);

/**
 * The number of tasks currently running.
 *
 * @param pool The pool.
 * @return As gcu_pool_count_active().
 */
GCU_API size_t gcu_managed_pool_count_active(const GCU_Managed_Pool * pool);

/**
 * The number of worker threads.
 *
 * The manager is not a worker and is not included.  An observation.
 *
 * @param pool The pool.
 * @return As gcu_pool_count_threads().
 */
GCU_API size_t gcu_managed_pool_count_threads(const GCU_Managed_Pool * pool);

/**
 * Whether teardown of the inner pool has begun.
 *
 * @param pool The pool.
 * @return As gcu_pool_is_shutting_down().
 */
GCU_API bool gcu_managed_pool_is_shutting_down(const GCU_Managed_Pool * pool);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_POOL_H
