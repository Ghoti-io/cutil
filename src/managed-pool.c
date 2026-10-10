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
 * A thread pool whose resize, destroy, and abandon run on a manager thread.
 *
 * gcu_pool_set_thread_count() joins the workers that leave, so a worker of
 * that pool cannot call it.  The manager is the only caller of that function
 * and of gcu_pool_destroy() and gcu_pool_abandon() once it is running.  Any
 * other thread, including a worker, stores a desired count and posts.  The
 * reasoning is recorded in `documentation/thread-pool.md`, Decision 4.
 */

#include <ghoti.io/cutil/macros.h>

#include <errno.h>
#include <string.h>

#include <ghoti.io/cutil/pool.h>

/**
 * The manager is not a worker.  The name is best-effort, as worker names are.
 */
static const char GCU_MANAGED_POOL_THREAD_NAME[] = "gcu-mgr";

/**
 * `stop` while the manager is still serving requests.
 */
static const int GCU_MANAGED_STOP_NONE = 0;

/**
 * The manager's next shutdown of the inner pool drains it.
 */
static const int GCU_MANAGED_STOP_DESTROY = 1;

/**
 * The manager's next shutdown of the inner pool discards the queue.
 */
static const int GCU_MANAGED_STOP_ABANDON = 2;

/**
 * Drop a pool that never started its manager.
 *
 * The manager is the only caller of destroy once it is running.  This runs
 * only when create could not start that thread, so the creating thread is
 * the one that joins the workers it just started.
 */
static void gcu_managed_pool_reset(GCU_Managed_Pool * pool, bool semaphore,
  bool mutex) {
  if (pool->inner) {
    gcu_pool_destroy(pool->inner);
  }
  if (semaphore) {
    gcu_semaphore_destroy(&pool->wake);
  }
  if (mutex) {
    GCU_MUTEX_DESTROY(pool->state);
  }
  memset(pool, 0, sizeof(*pool));
}

/**
 * Wait for one post.
 *
 * A POSIX wait is restarted only when it fails with EINTR.  Any other
 * failure is reported to the caller, which must not treat it as a wakeup
 * and must not spin on it.
 */
static bool gcu_managed_pool_wait_wake(GCU_Managed_Pool * pool) {
#ifdef _WIN32
  return gcu_semaphore_wait(&pool->wake) == 0;
#else
  for (;;) {
    if (gcu_semaphore_wait(&pool->wake) == 0) {
      return true;
    }
    if (errno != EINTR) {
      return false;
    }
  }
#endif
}

/**
 * Copy the request.  The store in set_thread_count happens before its post,
 * and this load happens after the wait that post released, with the mutex
 * held on both sides.
 */
static void gcu_managed_pool_read(GCU_Managed_Pool * pool, size_t * desired,
  int * stop) {
  GCU_MUTEX_LOCK(pool->state);
  *desired = pool->desired;
  *stop = pool->stop;
  GCU_MUTEX_UNLOCK(pool->state);
}

/**
 * Shut the inner pool down the way stop asked, then return so the caller
 * can join this thread.
 */
static void gcu_managed_pool_shutdown(GCU_Managed_Pool * pool, int stop) {
  if (stop == GCU_MANAGED_STOP_ABANDON) {
    gcu_pool_abandon(pool->inner);
  }
  else {
    gcu_pool_destroy(pool->inner);
  }
  pool->inner = NULL;
}

/**
 * Apply the stored count until it matches, a failed apply must wait, or
 * stop is set.
 *
 * A post that arrives during gcu_pool_set_thread_count() is left counted.
 * After that call this reads the stored count again and waits only when the
 * read already matches the live count, or when the apply failed and the
 * stored count is unchanged.  It does not drain the semaphore first, and it
 * does not spin on a failure.
 *
 * @return `true` when the inner pool has been shut down.
 */
static bool gcu_managed_pool_drive(GCU_Managed_Pool * pool) {
  for (;;) {
    size_t desired;
    int stop;

    gcu_managed_pool_read(pool, &desired, &stop);
    if (stop != GCU_MANAGED_STOP_NONE) {
      gcu_managed_pool_shutdown(pool, stop);
      return true;
    }

    // AUTO is resolved at this read, not at the time it was stored.
    size_t resolved = desired == GCU_POOL_THREADS_AUTO
      ? (size_t)gcu_thread_get_num_processors()
      : desired;
    if (gcu_pool_count_threads(pool->inner) == resolved) {
      return false;
    }

    size_t applied = desired;
    bool ok = gcu_pool_set_thread_count(pool->inner, applied);

    gcu_managed_pool_read(pool, &desired, &stop);
    if (!ok && desired == applied && stop == GCU_MANAGED_STOP_NONE) {
      return false;
    }
  }
}

/**
 * The manager loop.
 *
 * The first action is the wait, so create can publish a pool that is already
 * at its count and this thread blocks until a request or a stop arrives.
 */
static GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION
gcu_managed_pool_manager(GCU_THREAD_FUNC_ARG_T arg) {
  GCU_Managed_Pool * pool = (GCU_Managed_Pool *)arg;

  for (;;) {
    if (!gcu_managed_pool_wait_wake(pool)) {
      size_t desired;
      int stop;

      gcu_managed_pool_read(pool, &desired, &stop);
      (void)desired;
      if (stop == GCU_MANAGED_STOP_NONE) {
        gcu_thread_yield();
        continue;
      }
    }

    if (gcu_managed_pool_drive(pool)) {
      break;
    }
  }

  return (GCU_THREAD_FUNC_RETURN_T)0;
}

/**
 * Record stop if it is not already recorded, then wake the manager with the
 * same semaphore a resize uses.
 */
static void gcu_managed_pool_request_stop(GCU_Managed_Pool * pool, int stop) {
  GCU_MUTEX_LOCK(pool->state);
  if (pool->stop == GCU_MANAGED_STOP_NONE) {
    pool->stop = stop;
  }
  GCU_MUTEX_UNLOCK(pool->state);
  gcu_semaphore_signal(&pool->wake);
}

/**
 * Join the manager and zero the caller's storage.
 */
static void gcu_managed_pool_join_manager(GCU_Managed_Pool * pool, int stop) {
  gcu_managed_pool_request_stop(pool, stop);
  gcu_thread_join(pool->manager);
  gcu_semaphore_destroy(&pool->wake);
  GCU_MUTEX_DESTROY(pool->state);
  memset(pool, 0, sizeof(*pool));
}

static const GCU_Allocator * gcu_managed_pool_allocator(
  const GCU_Pool_Config * config) {
  if (config && config->allocator) {
    return config->allocator;
  }
  return gcu_allocator_default();
}

bool gcu_managed_pool_create_in_place(GCU_Managed_Pool * pool,
  const GCU_Pool_Config * config) {
  if (!pool) {
    return false;
  }

  memset(pool, 0, sizeof(*pool));

  // Inline mode is create-time on the inner pool, and a managed pool has no
  // use for it: there is no worker count for the manager to change.
  if (config && config->thread_count == 0) {
    return false;
  }

  pool->allocator = gcu_managed_pool_allocator(config);
  pool->desired = config ? config->thread_count : GCU_POOL_THREADS_AUTO;
  pool->stop = GCU_MANAGED_STOP_NONE;

  if (GCU_MUTEX_CREATE(pool->state) != 0) {
    memset(pool, 0, sizeof(*pool));
    return false;
  }

  if (gcu_semaphore_create(&pool->wake, 0) != 0) {
    GCU_MUTEX_DESTROY(pool->state);
    memset(pool, 0, sizeof(*pool));
    return false;
  }

  pool->inner = gcu_pool_create(config);
  if (!pool->inner) {
    gcu_managed_pool_reset(pool, true, true);
    return false;
  }

  if (gcu_thread_create(&pool->manager, gcu_managed_pool_manager, pool) != 0) {
    gcu_managed_pool_reset(pool, true, true);
    return false;
  }

  // After the thread exists, so a refusal here does not fail the create.
  gcu_thread_set_name(pool->manager, GCU_MANAGED_POOL_THREAD_NAME);
  return true;
}

GCU_Managed_Pool * gcu_managed_pool_create(const GCU_Pool_Config * config) {
  if (config && config->thread_count == 0) {
    return NULL;
  }

  const GCU_Allocator * allocator = gcu_managed_pool_allocator(config);
  GCU_Managed_Pool * pool =
    gcu_allocator_malloc(allocator, sizeof(GCU_Managed_Pool));
  if (!pool) {
    return NULL;
  }

  if (!gcu_managed_pool_create_in_place(pool, config)) {
    gcu_allocator_free(allocator, pool);
    return NULL;
  }

  return pool;
}

void gcu_managed_pool_destroy_in_place(GCU_Managed_Pool * pool) {
  if (!pool || !pool->allocator) {
    return;
  }

  gcu_managed_pool_join_manager(pool, GCU_MANAGED_STOP_DESTROY);
}

void gcu_managed_pool_abandon_in_place(GCU_Managed_Pool * pool) {
  if (!pool || !pool->allocator) {
    return;
  }

  gcu_managed_pool_join_manager(pool, GCU_MANAGED_STOP_ABANDON);
}

void gcu_managed_pool_destroy(GCU_Managed_Pool * pool) {
  if (!pool) {
    return;
  }

  const GCU_Allocator * allocator = pool->allocator;
  gcu_managed_pool_destroy_in_place(pool);
  gcu_allocator_free(allocator, pool);
}

void gcu_managed_pool_abandon(GCU_Managed_Pool * pool) {
  if (!pool) {
    return;
  }

  const GCU_Allocator * allocator = pool->allocator;
  gcu_managed_pool_abandon_in_place(pool);
  gcu_allocator_free(allocator, pool);
}

bool gcu_managed_pool_set_thread_count(GCU_Managed_Pool * pool,
  size_t thread_count) {
  if (!pool || !pool->allocator || thread_count == 0) {
    return false;
  }

  GCU_MUTEX_LOCK(pool->state);
  if (pool->stop != GCU_MANAGED_STOP_NONE) {
    GCU_MUTEX_UNLOCK(pool->state);
    return false;
  }
  pool->desired = thread_count;
  GCU_MUTEX_UNLOCK(pool->state);

  // The store is published before the post.  A post that lands while the
  // manager is inside gcu_pool_set_thread_count stays counted.
  return gcu_semaphore_signal(&pool->wake) == 0;
}

size_t gcu_managed_pool_desired_thread_count(const GCU_Managed_Pool * pool) {
  if (!pool || !pool->allocator) {
    return 0;
  }

  GCU_Managed_Pool * mutable_pool = (GCU_Managed_Pool *)pool;
  GCU_MUTEX_LOCK(mutable_pool->state);
  size_t desired = pool->desired;
  GCU_MUTEX_UNLOCK(mutable_pool->state);
  return desired;
}

static GCU_Pool * gcu_managed_pool_inner(const GCU_Managed_Pool * pool) {
  return pool ? pool->inner : NULL;
}

bool gcu_managed_pool_enqueue(GCU_Managed_Pool * pool, GCU_Pool_Task task,
  void * ctx) {
  return gcu_pool_enqueue(gcu_managed_pool_inner(pool), task, ctx);
}

bool gcu_managed_pool_enqueue_cb(GCU_Managed_Pool * pool, GCU_Pool_Task task,
  void * ctx, GCU_Pool_Complete on_complete, void * user_data) {
  return gcu_pool_enqueue_cb(gcu_managed_pool_inner(pool), task, ctx,
    on_complete, user_data);
}

bool gcu_managed_pool_enqueue_wait(GCU_Managed_Pool * pool, GCU_Pool_Task task,
  void * ctx) {
  return gcu_pool_enqueue_wait(gcu_managed_pool_inner(pool), task, ctx);
}

bool gcu_managed_pool_enqueue_wait_cb(GCU_Managed_Pool * pool,
  GCU_Pool_Task task, void * ctx, GCU_Pool_Complete on_complete,
  void * user_data) {
  return gcu_pool_enqueue_wait_cb(gcu_managed_pool_inner(pool), task, ctx,
    on_complete, user_data);
}

int gcu_managed_pool_wait(GCU_Managed_Pool * pool) {
  return gcu_pool_wait(gcu_managed_pool_inner(pool));
}

void gcu_managed_pool_clear_error(GCU_Managed_Pool * pool) {
  gcu_pool_clear_error(gcu_managed_pool_inner(pool));
}

size_t gcu_managed_pool_count_queued(const GCU_Managed_Pool * pool) {
  return gcu_pool_count_queued(gcu_managed_pool_inner(pool));
}

size_t gcu_managed_pool_count_active(const GCU_Managed_Pool * pool) {
  return gcu_pool_count_active(gcu_managed_pool_inner(pool));
}

size_t gcu_managed_pool_count_threads(const GCU_Managed_Pool * pool) {
  return gcu_pool_count_threads(gcu_managed_pool_inner(pool));
}

bool gcu_managed_pool_is_shutting_down(const GCU_Managed_Pool * pool) {
  return gcu_pool_is_shutting_down(gcu_managed_pool_inner(pool));
}
