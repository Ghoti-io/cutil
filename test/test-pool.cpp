#include <gtest/gtest.h>
#include <ghoti.io/cutil/pool.h>
#include <ghoti.io/cutil/thread.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <cstdio>
#include <pthread.h>
#endif

using namespace std;

namespace {

/// Shared counters for the simple cases.
std::atomic<int> g_ran{0};

int count_task(void *) {
  g_ran.fetch_add(1);
  return 0;
}

/// Records that a particular task index ran, so that a task running twice is
/// caught rather than averaging out against one that never ran.
struct Marks {
  std::vector<std::atomic<int>> hits;
  explicit Marks(size_t n) : hits(n) {
    for (auto & h : hits) {
      h.store(0);
    }
  }
};

struct MarkArg {
  Marks * marks;
  size_t index;
  unsigned sleep_ms;
};

int mark_task(void * ctx) {
  MarkArg * arg = (MarkArg *)ctx;
  if (arg->sleep_ms) {
    gcu_thread_sleep(arg->sleep_ms);
  }
  arg->marks->hits[arg->index].fetch_add(1);
  return 0;
}

int fail_task(void * ctx) {
  return (int)(intptr_t)ctx;
}

int noop_task(void *) {
  return 0;
}

/**
 * Tears a pool down however the test ends.
 *
 * A pool that outlives its test is not merely a leak.  Its workers block in
 * the pool's semaphore forever, and the thread module's exit destructor joins
 * every thread it still knows about, so the process hangs on the way out
 * instead of reporting the failure.  A failed ASSERT returns from the test
 * body immediately, which is exactly when that happens, so teardown cannot be
 * the last statement of the test.
 */
struct PoolGuard {
  GCU_Pool * pool;

  explicit PoolGuard(GCU_Pool * p) : pool(p) {}
  PoolGuard(const PoolGuard &) = delete;
  PoolGuard & operator=(const PoolGuard &) = delete;

  /// Ordinary teardown: runs whatever is still queued.
  void destroy() {
    gcu_pool_destroy(pool);
    pool = nullptr;
  }

  /// Teardown that discards the queue.
  void abandon() {
    gcu_pool_abandon(pool);
    pool = nullptr;
  }

  /// Hand ownership back, for a test that tears down on another thread.
  void release() {
    pool = nullptr;
  }

  ~PoolGuard() {
    if (pool) {
      gcu_pool_abandon(pool);
    }
  }
};

/// Blocks until released, so that a worker can be held busy on purpose.
struct Gate {
  std::atomic<bool> open{false};
};

int gate_task(void * ctx) {
  Gate * gate = (Gate *)ctx;
  while (!gate->open.load()) {
    gcu_thread_yield();
  }
  return 0;
}

/// Gate task that also counts, so a resize test can tell the tasks apart from
/// the ones it enqueues afterwards.
struct GateCount {
  Gate * gate;
  std::atomic<int> * ran;
};

int gate_count_task(void * ctx) {
  GateCount * arg = (GateCount *)ctx;
  while (!arg->gate->open.load()) {
    gcu_thread_yield();
  }
  arg->ran->fetch_add(1);
  return 0;
}

/// Records the worker's id, then waits.  Four of these on four workers name
/// every worker without reading the pool's private list.
struct IdGate {
  Gate * gate;
  GCU_Thread * ids;
  std::atomic<size_t> * filled;
};

int id_gate_task(void * ctx) {
  IdGate * arg = (IdGate *)ctx;
  size_t slot = arg->filled->fetch_add(1);
  arg->ids[slot] = gcu_thread_get_current_id();
  while (!arg->gate->open.load()) {
    gcu_thread_yield();
  }
  return 0;
}

/// Spin until `pred` is true, or ten seconds pass.
template <typename Pred>
bool until(Pred pred) {
  auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
  while (chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    this_thread::yield();
  }
  return pred();
}

/// Fails unless this worker's id is already in the pool's thread list.
struct ListedOnEntry {
  GCU_Pool * pool;
  std::atomic<int> ran{0};
  std::atomic<int> missing{0};
};

int listed_on_entry(void * ctx) {
  ListedOnEntry * arg = (ListedOnEntry *)ctx;
  GCU_Thread self = gcu_thread_get_current_id();
  bool found = false;

  GCU_MUTEX_LOCK(arg->pool->mutex);
  for (size_t i = 0; i < arg->pool->thread_count; ++i) {
    if (arg->pool->threads[i] == self) {
      found = true;
      break;
    }
  }
  GCU_MUTEX_UNLOCK(arg->pool->mutex);

  if (!found) {
    arg->missing.fetch_add(1);
  }
  arg->ran.fetch_add(1);
  return found ? 0 : 1;
}

/// One worker, one gate, so a shrink test can release them one at a time.
struct OneGate {
  std::atomic<bool> open{false};
  GCU_Thread id{0};
  std::atomic<bool> entered{false};
};

int one_gate_task(void * ctx) {
  OneGate * slot = (OneGate *)ctx;
  slot->id = gcu_thread_get_current_id();
  slot->entered.store(true);
  while (!slot->open.load()) {
    gcu_thread_yield();
  }
  return 0;
}

/// Keeps a pool busy for long enough to park a thread in gcu_pool_wait().
int slow_task(void * ctx) {
  (void)ctx;
  gcu_thread_sleep(30);
  return 0;
}

}

//
// Drain.  Written first, and the reason this module exists in the shape it
// does:  the sibling compress implementation documents draining and discards
// the queue instead, racily, because its worker tests the shutdown flag
// before it looks at the queue.
//

TEST(Pool, DestroyRunsEveryQueuedTask) {
  const size_t kTasks = 64;
  Marks marks(kTasks);
  std::vector<MarkArg> args(kTasks);

  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  // Far more tasks than workers, each slow enough that the queue is certainly
  // non-empty when the pool is destroyed.
  for (size_t i = 0; i < kTasks; ++i) {
    args[i] = {&marks, i, 1};
    ASSERT_TRUE(gcu_pool_enqueue(pool, mark_task, &args[i]));
  }
  ASSERT_GT(gcu_pool_count_queued(pool), 0u);

  guard.destroy();

  for (size_t i = 0; i < kTasks; ++i) {
    EXPECT_EQ(1, marks.hits[i].load()) << "task " << i << " did not run once";
  }
}

TEST(Pool, AbandonDiscardsQueuedTasks) {
  const size_t kTasks = 64;
  Marks marks(kTasks);
  std::vector<MarkArg> args(kTasks);

  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (size_t i = 0; i < kTasks; ++i) {
    args[i] = {&marks, i, 1};
    ASSERT_TRUE(gcu_pool_enqueue(pool, mark_task, &args[i]));
  }
  ASSERT_GT(gcu_pool_count_queued(pool), 0u);

  guard.abandon();

  // Some ran, but not all: the point of abandoning is that the queue is
  // dropped.  Under ASan this also proves the dropped entries were freed.
  int total = 0;
  for (size_t i = 0; i < kTasks; ++i) {
    EXPECT_LE(marks.hits[i].load(), 1);
    total += marks.hits[i].load();
  }
  EXPECT_LT(total, (int)kTasks);
}

//
// Basics.
//

TEST(Pool, CreateAndDestroyAtEveryThreadCount) {
  for (size_t n : {(size_t)0, (size_t)1, (size_t)2, (size_t)8}) {
    GCU_Pool_Config config = {};
    config.thread_count = n;

    GCU_Pool * pool = gcu_pool_create(&config);
    ASSERT_NE(nullptr, pool) << "thread_count " << n;
    PoolGuard guard(pool);
    EXPECT_EQ(n == 0, gcu_pool_is_inline(pool));
    EXPECT_EQ(n == 0 ? 0u : n, gcu_pool_count_threads(pool));
    guard.destroy();
  }
}

TEST(Pool, AutoThreadCountIsUsable) {
  GCU_Pool_Config config = {};
  config.thread_count = GCU_POOL_THREADS_AUTO;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_FALSE(gcu_pool_is_inline(pool));
  EXPECT_GE(gcu_pool_count_threads(pool), 1u);
  guard.destroy();
}

TEST(Pool, NullConfigIsAllDefaults) {
  GCU_Pool * pool = gcu_pool_create(NULL);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_FALSE(gcu_pool_is_inline(pool));
  EXPECT_GE(gcu_pool_count_threads(pool), 1u);
  guard.destroy();
}

TEST(Pool, OneThreadIsNotInline) {
  // compress treats a request for one worker as a request for synchronous
  // execution.  A request for one worker here gets one worker.
  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_FALSE(gcu_pool_is_inline(pool));
  EXPECT_EQ(1u, gcu_pool_count_threads(pool));

  Gate gate;
  ASSERT_TRUE(gcu_pool_enqueue(pool, gate_task, &gate));

  // The task cannot have completed:  it is waiting on a gate this thread has
  // not opened, so enqueue must not have run it here.
  EXPECT_FALSE(gcu_pool_is_inline(pool));
  gate.open.store(true);

  EXPECT_EQ(0, gcu_pool_wait(pool));
  guard.destroy();
}

TEST(Pool, EveryTaskRunsExactlyOnce) {
  const size_t kTasks = 500;
  Marks marks(kTasks);
  std::vector<MarkArg> args(kTasks);

  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (size_t i = 0; i < kTasks; ++i) {
    args[i] = {&marks, i, 0};
    ASSERT_TRUE(gcu_pool_enqueue(pool, mark_task, &args[i]));
  }

  EXPECT_EQ(0, gcu_pool_wait(pool));
  for (size_t i = 0; i < kTasks; ++i) {
    EXPECT_EQ(1, marks.hits[i].load()) << "task " << i;
  }

  EXPECT_EQ(0u, gcu_pool_count_queued(pool));
  EXPECT_EQ(0u, gcu_pool_count_active(pool));
  guard.destroy();
}

//
// Inline mode.
//

TEST(Pool, InlineRunsOnTheCallingThread) {
  GCU_Pool_Config config = {};
  config.thread_count = 0;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_TRUE(gcu_pool_is_inline(pool));
  EXPECT_EQ(0u, gcu_pool_count_threads(pool));

  g_ran.store(0);
  EXPECT_TRUE(gcu_pool_enqueue(pool, count_task, NULL));

  // Already done, with nothing waited on.
  EXPECT_EQ(1, g_ran.load());
  EXPECT_EQ(0u, gcu_pool_count_queued(pool));
  EXPECT_EQ(0, gcu_pool_wait(pool));

  guard.destroy();
}

TEST(Pool, InlineRecordsErrors) {
  GCU_Pool_Config config = {};
  config.thread_count = 0;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  EXPECT_TRUE(gcu_pool_enqueue(pool, fail_task, (void *)(intptr_t)7));
  EXPECT_EQ(7, gcu_pool_wait(pool));

  gcu_pool_clear_error(pool);
  EXPECT_EQ(0, gcu_pool_wait(pool));

  guard.destroy();
}

//
// Waiting.
//

TEST(Pool, WaitOnIdlePoolReturnsImmediately) {
  GCU_Pool * pool = gcu_pool_create(NULL);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_EQ(0, gcu_pool_wait(pool));
  guard.destroy();
}

TEST(Pool, ConcurrentWaitersAllReturn) {
  // compress signals its completion semaphore once regardless of how many
  // threads are waiting, so a second waiter there never wakes.
  const size_t kWaiters = 8;
  const size_t kTasks = 200;
  Marks marks(kTasks);
  std::vector<MarkArg> args(kTasks);

  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (size_t i = 0; i < kTasks; ++i) {
    args[i] = {&marks, i, 1};
    ASSERT_TRUE(gcu_pool_enqueue(pool, mark_task, &args[i]));
  }

  std::atomic<int> returned{0};
  std::vector<std::thread> waiters;
  for (size_t i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      gcu_pool_wait(pool);
      returned.fetch_add(1);
    });
  }
  for (auto & t : waiters) {
    t.join();
  }

  EXPECT_EQ((int)kWaiters, returned.load());
  guard.destroy();
}

TEST(Pool, SecondWaitDoesNotReturnEarly) {
  // compress can leave a surplus count on its completion semaphore, so a
  // later wait returns at once while work is still outstanding.
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (int round = 0; round < 20; ++round) {
    const size_t kTasks = 32;
    Marks marks(kTasks);
    std::vector<MarkArg> args(kTasks);

    for (size_t i = 0; i < kTasks; ++i) {
      args[i] = {&marks, i, 0};
      ASSERT_TRUE(gcu_pool_enqueue(pool, mark_task, &args[i]));
    }

    EXPECT_EQ(0, gcu_pool_wait(pool));

    // If the wait returned early, some task has not run yet.
    for (size_t i = 0; i < kTasks; ++i) {
      ASSERT_EQ(1, marks.hits[i].load())
        << "round " << round << ", task " << i;
    }
    EXPECT_EQ(0u, gcu_pool_count_queued(pool));
    EXPECT_EQ(0u, gcu_pool_count_active(pool));
  }

  guard.destroy();
}

//
// Errors.
//

TEST(Pool, FirstErrorIsKeptAndSurvivesReading) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  // One worker, so the order is deterministic and the *first* error is known.
  ASSERT_TRUE(gcu_pool_enqueue(pool, fail_task, (void *)(intptr_t)11));
  ASSERT_TRUE(gcu_pool_enqueue(pool, fail_task, (void *)(intptr_t)22));
  EXPECT_EQ(11, gcu_pool_wait(pool));

  // Reading does not consume it:  compress resets its error inside wait, so
  // the second caller there is told everything succeeded.
  EXPECT_EQ(11, gcu_pool_wait(pool));

  gcu_pool_clear_error(pool);
  EXPECT_EQ(0, gcu_pool_wait(pool));

  guard.destroy();
}

TEST(Pool, SuccessfulTasksLeaveNoError) {
  GCU_Pool_Config config = {};
  config.thread_count = 3;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue(pool, noop_task, NULL));
  }
  EXPECT_EQ(0, gcu_pool_wait(pool));

  guard.destroy();
}

//
// Completion callbacks.
//

namespace {

struct CbRecord {
  std::atomic<int> calls{0};
  std::atomic<int> last_status{0};
  std::atomic<void *> last_user{nullptr};
};

void on_complete(void *, int status, void * user_data) {
  CbRecord * rec = (CbRecord *)user_data;
  rec->calls.fetch_add(1);
  rec->last_status.store(status);
  rec->last_user.store(user_data);
}

}

TEST(Pool, CallbackFiresOncePerTaskWithItsStatus) {
  CbRecord rec;

  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  ASSERT_TRUE(gcu_pool_enqueue_cb(
    pool, fail_task, (void *)(intptr_t)5, on_complete, &rec));
  EXPECT_EQ(5, gcu_pool_wait(pool));

  EXPECT_EQ(1, rec.calls.load());
  EXPECT_EQ(5, rec.last_status.load());
  EXPECT_EQ(&rec, rec.last_user.load());

  guard.destroy();
}

TEST(Pool, CallbackRunsBeforeTheTaskCountsComplete) {
  // wait() must not return until the callback has run, or a caller that uses
  // the callback to publish a result can observe the pool idle before the
  // result exists.
  CbRecord rec;

  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  const int kTasks = 100;
  for (int i = 0; i < kTasks; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue_cb(pool, noop_task, NULL, on_complete, &rec));
  }

  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_EQ(kTasks, rec.calls.load());

  guard.destroy();
}

//
// Bounded queue.
//

TEST(Pool, BoundedEnqueueFailsWhenFull) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;
  config.max_queued = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  // Hold the single worker so that nothing drains while the queue fills.
  Gate gate;
  ASSERT_TRUE(gcu_pool_enqueue(pool, gate_task, &gate));
  while (gcu_pool_count_active(pool) == 0) {
    gcu_thread_yield();
  }

  size_t accepted = 0;
  for (size_t i = 0; i < config.max_queued + 8; ++i) {
    if (gcu_pool_enqueue(pool, noop_task, NULL)) {
      ++accepted;
    }
  }

  // The bound is honoured, and the excess was refused rather than queued.
  EXPECT_EQ(config.max_queued, accepted);

  gate.open.store(true);
  EXPECT_EQ(0, gcu_pool_wait(pool));
  guard.destroy();
}

TEST(Pool, UnboundedIgnoresTheWaitingForm) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.max_queued = 0;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue_wait(pool, noop_task, NULL));
  }
  EXPECT_EQ(0, gcu_pool_wait(pool));

  guard.destroy();
}

TEST(Pool, BoundedWaitingEnqueueProceedsWhenASlotFrees) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;
  config.max_queued = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  Gate gate;
  ASSERT_TRUE(gcu_pool_enqueue(pool, gate_task, &gate));
  while (gcu_pool_count_active(pool) == 0) {
    gcu_thread_yield();
  }

  ASSERT_TRUE(gcu_pool_enqueue(pool, noop_task, NULL));
  ASSERT_TRUE(gcu_pool_enqueue(pool, noop_task, NULL));
  ASSERT_FALSE(gcu_pool_enqueue(pool, noop_task, NULL));

  // This one has to wait for the worker to take something off the queue.
  std::atomic<bool> done{false};
  std::thread producer([&] {
    EXPECT_TRUE(gcu_pool_enqueue_wait(pool, noop_task, NULL));
    done.store(true);
  });

  EXPECT_FALSE(done.load());
  gate.open.store(true);

  producer.join();
  EXPECT_TRUE(done.load());

  EXPECT_EQ(0, gcu_pool_wait(pool));
  guard.destroy();
}

TEST(Pool, ShutdownReleasesAProducerWaitingForASlot) {
  // The failure this guards against is a hang, not a wrong value, so it runs
  // the teardown on another thread and gives up after a deadline rather than
  // blocking the suite forever.
  GCU_Pool_Config config = {};
  config.thread_count = 1;
  config.max_queued = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  Gate gate;
  ASSERT_TRUE(gcu_pool_enqueue(pool, gate_task, &gate));
  while (gcu_pool_count_active(pool) == 0) {
    gcu_thread_yield();
  }
  ASSERT_TRUE(gcu_pool_enqueue(pool, noop_task, NULL));

  // The queue is full and the only worker is held, so this blocks.
  std::atomic<bool> refused{false};
  std::atomic<bool> producer_waiting{false};
  std::thread producer([&] {
    producer_waiting.store(true);
    if (!gcu_pool_enqueue_wait(pool, noop_task, NULL)) {
      refused.store(true);
    }
  });

  while (!producer_waiting.load()) {
    gcu_thread_yield();
  }
  gcu_thread_sleep(20);

  std::atomic<bool> torn_down{false};
  std::thread teardown([&] {
    gate.open.store(true);
    guard.abandon();
    torn_down.store(true);
  });

  auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
  while (!torn_down.load() && chrono::steady_clock::now() < deadline) {
    this_thread::sleep_for(chrono::milliseconds(5));
  }

  EXPECT_TRUE(torn_down.load())
    << "teardown hung: a producer waiting for a slot was never released";

  producer.join();
  teardown.join();
}

//
// Teardown owes an exit to every thread it wakes.
//

TEST(Pool, TeardownWaitsForASleeperInPoolWait) {
  // A thread parked in gcu_pool_wait() is released either by the last worker
  // going idle or by teardown itself, and in both cases it then re-takes the
  // pool's mutex and reads first_error.  Teardown counted neither, so it
  // destroyed that mutex and freed the pool while they were walking into it.
  //
  // Measured against the defect, ten runs of this test: 10/10 failed, every
  // one on the first round with all sixteen threads stranded -- they had been
  // released by the worker a moment before the free, and then blocked forever
  // on a mutex that was no longer there.  So this reports by value and needs
  // no sanitizer, though a ThreadSanitizer build additionally names it: the
  // same twelve runs under TSan gave six heap-use-after-free reports and four
  // hangs.  Rounds and sixteen sleepers because one of each catches nothing.
  //
  // The sibling case is a producer parked on the `slots` semaphore of a
  // bounded queue, which shares the counter and the drain this exercises.  Its
  // window is far narrower -- one report in 16,500 runs of the existing
  // Pool.ShutdownReleasesAProducerWaitingForASlot -- so it is not gated here.
  const size_t kRounds = 10;
  const size_t kSleepers = 16;

  for (size_t round = 0; round < kRounds; ++round) {
    GCU_Pool_Config config = {};
    config.thread_count = 1;

    GCU_Pool * pool = gcu_pool_create(&config);
    ASSERT_NE(nullptr, pool);

    // Keeps the pool non-idle, so that a thread reaching gcu_pool_wait()
    // parks instead of returning at once and testing nothing.
    ASSERT_TRUE(gcu_pool_enqueue(pool, slow_task, NULL));

    std::atomic<size_t> entered{0};
    std::atomic<size_t> returned{0};
    std::vector<std::thread> sleepers;
    for (size_t i = 0; i < kSleepers; ++i) {
      sleepers.emplace_back([&] {
        entered.fetch_add(1);
        gcu_pool_wait(pool);
        returned.fetch_add(1);
      });
    }
    while (entered.load() != kSleepers) {
      gcu_thread_yield();
    }
    // Covers the few instructions between that count and the call itself.
    // Destroying a pool a thread has not entered yet is the caller's mistake
    // and not the library's, and this test is not about that one.
    gcu_thread_sleep(20);

    gcu_pool_destroy(pool);

    // The defect strands sleepers as often as it corrupts them: one that
    // registers just after teardown's release is never posted at all.  Wait
    // on a deadline and report, rather than joining into a hang -- a gate
    // that hangs a suite is read as infrastructure trouble, not as a bug.
    auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
    while (returned.load() != kSleepers
        && chrono::steady_clock::now() < deadline) {
      this_thread::sleep_for(chrono::milliseconds(1));
    }
    if (returned.load() != kSleepers) {
      for (auto & t : sleepers) {
        t.detach();
      }
      FAIL() << "round " << round << ": " << (kSleepers - returned.load())
             << " of " << kSleepers
             << " threads released by teardown never came back";
    }
    for (auto & t : sleepers) {
      t.join();
    }
  }
}

//
// Worker names.
//

TEST(Pool, WorkersCarryTheConfiguredPrefix) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.name_prefix = "gtestpl";

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  // Names are best-effort, so this asserts the name only where the platform
  // supplied one at all.
  std::atomic<int> checked{0};
  auto probe = [](void * ctx) -> int {
    char name[64] = {0};
    if (gcu_thread_get_name(gcu_thread_get_current_id(), name, sizeof(name))
        == 0 && name[0]) {
      if (strncmp(name, "gtestpl-", 8) == 0) {
        ((std::atomic<int> *)ctx)->fetch_add(1);
      }
    }
    return 0;
  };

  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue(pool, probe, &checked));
  }
  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_GT(checked.load(), 0);

  guard.destroy();
}

TEST(Pool, OverlongPrefixIsTruncatedNotRejected) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;
  config.name_prefix = "a-very-long-prefix-indeed";

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_EQ(1u, gcu_pool_count_threads(pool));
  guard.destroy();
}

//
// Contention.
//

TEST(Pool, ManyProducersAndWorkers) {
  const size_t kProducers = 8;
  const size_t kPerProducer = 500;
  const size_t kTasks = kProducers * kPerProducer;

  Marks marks(kTasks);
  std::vector<MarkArg> args(kTasks);

  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  std::vector<std::thread> producers;
  for (size_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (size_t i = 0; i < kPerProducer; ++i) {
        size_t index = p * kPerProducer + i;
        args[index] = {&marks, index, 0};
        EXPECT_TRUE(gcu_pool_enqueue(pool, mark_task, &args[index]));
      }
    });
  }
  for (auto & t : producers) {
    t.join();
  }

  EXPECT_EQ(0, gcu_pool_wait(pool));

  size_t total = 0;
  for (size_t i = 0; i < kTasks; ++i) {
    ASSERT_EQ(1, marks.hits[i].load()) << "task " << i;
    total += (size_t)marks.hits[i].load();
  }
  EXPECT_EQ(kTasks, total);

  guard.destroy();
}

//
// Lifecycle and argument handling.
//

TEST(Pool, InPlaceLifecycle) {
  GCU_Pool pool;
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  ASSERT_TRUE(gcu_pool_create_in_place(&pool, &config));
  g_ran.store(0);
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(gcu_pool_enqueue(&pool, count_task, NULL));
  }
  EXPECT_EQ(0, gcu_pool_wait(&pool));
  EXPECT_EQ(20, g_ran.load());
  gcu_pool_destroy_in_place(&pool);

  // Tearing the same storage down twice must be safe.
  gcu_pool_destroy_in_place(&pool);
}

TEST(Pool, DestroyAnUnusedPool) {
  GCU_Pool * pool = gcu_pool_create(NULL);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  guard.destroy();
}

TEST(Pool, NullArgumentsAreRefusedNotFatal) {
  EXPECT_FALSE(gcu_pool_enqueue(NULL, noop_task, NULL));
  EXPECT_FALSE(gcu_pool_enqueue_wait(NULL, noop_task, NULL));
  EXPECT_EQ(0, gcu_pool_wait(NULL));
  EXPECT_EQ(0u, gcu_pool_count_queued(NULL));
  EXPECT_EQ(0u, gcu_pool_count_active(NULL));
  EXPECT_EQ(0u, gcu_pool_count_threads(NULL));
  EXPECT_TRUE(gcu_pool_is_inline(NULL));
  EXPECT_TRUE(gcu_pool_is_shutting_down(NULL));
  gcu_pool_clear_error(NULL);
  gcu_pool_destroy(NULL);
  gcu_pool_abandon(NULL);
  gcu_pool_destroy_in_place(NULL);
  gcu_pool_abandon_in_place(NULL);

  GCU_Pool * pool = gcu_pool_create(NULL);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_FALSE(gcu_pool_enqueue(pool, NULL, NULL));
  guard.destroy();
}

TEST(Pool, ShuttingDownIsReportedAfterTeardownBegins) {
  GCU_Pool * pool = gcu_pool_create(NULL);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  EXPECT_FALSE(gcu_pool_is_shutting_down(pool));
  guard.destroy();
}

//
// Resizing.  The worker count is create-time unless this call changes it.
// A shrink must not return while queued work remains, and a failed allocation
// must not change the count.
//

TEST(Pool, SetThreadCountGrowsAndRunsTasksAtOnce) {
  Gate gate;
  std::atomic<int> ran{0};
  GateCount arg{&gate, &ran};

  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  bool queued = true;
  for (int i = 0; i < 4; ++i) {
    queued = queued && gcu_pool_enqueue(pool, gate_count_task, &arg);
  }
  bool grew = gcu_pool_set_thread_count(pool, 4);

  bool inside = until([&] {
    return gcu_pool_count_active(pool) == 4;
  });
  // A failed assert abandons the pool.  Open the gate first, or the join of
  // a task still waiting on it hangs.
  gate.open.store(true);

  ASSERT_TRUE(queued);
  ASSERT_TRUE(grew);
  ASSERT_TRUE(inside);
  EXPECT_EQ(4u, gcu_pool_count_threads(pool));
  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_EQ(4, ran.load());
  guard.destroy();
}

TEST(Pool, SetThreadCountGrowListsTheWorkerBeforeItsTask) {
  Gate gate;
  ListedOnEntry listed{};

  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);
  listed.pool = pool;

  // Hold the original worker so the tasks below run on the threads grow
  // starts, which is the window where an id can still be unpublished.
  bool held = gcu_pool_enqueue(pool, gate_task, &gate);
  bool busy = until([&] {
    return gcu_pool_count_active(pool) == 1;
  });
  bool queued = true;
  if (held && busy) {
    for (int i = 0; i < 3; ++i) {
      queued = queued && gcu_pool_enqueue(pool, listed_on_entry, &listed);
    }
  }
  bool grew = false;
  bool ran = false;
  if (queued && busy) {
    grew = gcu_pool_set_thread_count(pool, 4);
    ran = until([&] {
      return listed.ran.load() == 3;
    });
  }
  gate.open.store(true);

  ASSERT_TRUE(held);
  ASSERT_TRUE(busy);
  ASSERT_TRUE(queued);
  ASSERT_TRUE(grew);
  ASSERT_TRUE(ran);
  EXPECT_EQ(0, listed.missing.load());
  EXPECT_EQ(0, gcu_pool_wait(pool));
  guard.destroy();
}

struct GrownName {
  GCU_Thread original;
  std::atomic<int> newcomers{0};
  std::atomic<int> bad{0};
};

int grown_name_task(void * ctx) {
  GrownName * arg = (GrownName *)ctx;
  GCU_Thread self = gcu_thread_get_current_id();
  if (self == arg->original) {
    return 0;
  }

  char name[64] = {0};
  bool ok = gcu_thread_get_name(self, name, sizeof(name)) == 0
    && strncmp(name, "gtestpl-", 8) == 0;
  if (!ok) {
    arg->bad.fetch_add(1);
  }
  arg->newcomers.fetch_add(1);
  return ok ? 0 : 1;
}

int capture_id_task(void * ctx) {
  *((GCU_Thread *)ctx) = gcu_thread_get_current_id();
  return 0;
}

TEST(Pool, SetThreadCountGrowNamesNewWorkersWithTheCreatePrefix) {
  GCU_Thread original = 0;
  GrownName probe{};

  GCU_Pool_Config config = {};
  config.thread_count = 1;
  config.name_prefix = "gtestpl";

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  ASSERT_TRUE(gcu_pool_enqueue(pool, capture_id_task, &original));
  ASSERT_EQ(0, gcu_pool_wait(pool));
  ASSERT_NE(0u, original);
  probe.original = original;

  ASSERT_TRUE(gcu_pool_set_thread_count(pool, 4));
  for (int i = 0; i < 16; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue(pool, grown_name_task, &probe));
  }
  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_GT(probe.newcomers.load(), 0);
  EXPECT_EQ(0, probe.bad.load());
  guard.destroy();
}

TEST(Pool, SetThreadCountShrinksAnIdlePool) {
  Gate gate;
  GCU_Thread ids[4] = {};
  std::atomic<size_t> filled{0};
  IdGate arg{&gate, ids, &filled};

  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue(pool, id_gate_task, &arg));
  }
  bool inside = until([&] {
    return gcu_pool_count_active(pool) == 4;
  });
  gate.open.store(true);
  ASSERT_TRUE(inside);
  ASSERT_EQ(0, gcu_pool_wait(pool));

  ASSERT_TRUE(gcu_pool_set_thread_count(pool, 1));
  EXPECT_EQ(1u, gcu_pool_count_threads(pool));

  int left = 0;
  int stayed = 0;
  for (GCU_Thread id : ids) {
    bool joined = false;
    ASSERT_EQ(0, gcu_thread_is_joined(id, &joined));
    if (joined) {
      ++left;
    }
    else {
      ++stayed;
    }
  }
  EXPECT_EQ(3, left);
  EXPECT_EQ(1, stayed);

  g_ran.store(0);
  ASSERT_TRUE(gcu_pool_enqueue(pool, count_task, NULL));
  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_EQ(1, g_ran.load());
  guard.destroy();
}

TEST(Pool, SetThreadCountShrinksOnlyAfterQueuedTasksRun) {
  Gate gate;
  std::atomic<int> ran{0};
  GateCount arg{&gate, &ran};

  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(gcu_pool_enqueue(pool, gate_count_task, &arg));
  }
  bool inside = until([&] {
    return gcu_pool_count_active(pool) == 4
      && gcu_pool_count_queued(pool) == 4;
  });
  // Open before any assertion that can fail, once the resizer below exists.
  // Until then a failed assertion must not leave the workers in the gate.
  if (!inside) {
    gate.open.store(true);
    FAIL() << "workers never occupied the gate with a queue behind them";
  }

  std::atomic<bool> entered{false};
  std::atomic<bool> finished{false};
  bool accepted = false;
  size_t queued_at_return = static_cast<size_t>(-1);
  std::thread resizer([&] {
    entered.store(true);
    accepted = gcu_pool_set_thread_count(pool, 1);
    // Sampled in this thread, at the return, so a drain that happens later
    // cannot hide a return that left the queue non-empty.
    queued_at_return = gcu_pool_count_queued(pool);
    finished.store(true);
  });

  // The gate still holds every worker, so the call cannot finish.  retire is
  // set under the pool mutex once the call is actually inside the shrink;
  // that is what makes "queued > 0 while the call is in progress" observable
  // rather than a sample taken before the call starts.
  bool saw_queued = until([&] {
    if (!entered.load() || finished.load()) {
      return false;
    }
    GCU_MUTEX_LOCK(pool->mutex);
    bool shrinking = pool->retire > 0;
    GCU_MUTEX_UNLOCK(pool->mutex);
    return shrinking && gcu_pool_count_queued(pool) > 0;
  });
  // The call is inside the shrink and the gate still holds every worker.
  // One more task has to be accepted and has to run.
  bool extra = false;
  if (saw_queued) {
    extra = gcu_pool_enqueue(pool, gate_count_task, &arg);
  }
  gate.open.store(true);
  resizer.join();

  ASSERT_TRUE(saw_queued);
  ASSERT_TRUE(extra);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(0u, queued_at_return);
  EXPECT_EQ(1u, gcu_pool_count_threads(pool));
  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_EQ(9, ran.load());
  guard.destroy();
}

TEST(Pool, SetThreadCountJoinsALeaverBeforeTheShrinkReturns) {
  OneGate slots[4];

  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  bool queued = true;
  for (OneGate & slot : slots) {
    queued = queued && gcu_pool_enqueue(pool, one_gate_task, &slot);
  }
  bool held = until([&] {
    for (OneGate & slot : slots) {
      if (!slot.entered.load()) {
        return false;
      }
    }
    return gcu_pool_count_active(pool) == 4;
  });
  if (!queued || !held) {
    for (OneGate & slot : slots) {
      slot.open.store(true);
    }
    FAIL() << "workers never parked on their gates";
  }

  std::atomic<bool> finished{false};
  bool accepted = false;
  std::thread resizer([&] {
    accepted = gcu_pool_set_thread_count(pool, 1);
    finished.store(true);
  });

  // Release one worker.  It is the first that can leave, so a shrink that
  // joins as it goes must have joined this id while the other three are
  // still on their gates and the call has not returned.
  slots[0].open.store(true);
  bool joined_early = until([&] {
    bool joined = false;
    if (gcu_thread_is_joined(slots[0].id, &joined) != 0 || !joined) {
      return false;
    }
    return !finished.load();
  });
  for (OneGate & slot : slots) {
    slot.open.store(true);
  }
  resizer.join();

  ASSERT_TRUE(joined_early);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(1u, gcu_pool_count_threads(pool));
  guard.destroy();
}

#if defined(__linux__)
namespace {

volatile sig_atomic_t g_retired_wait_interrupts = 0;

// ThreadSanitizer does not order a signal handler with the pthread_kill that
// delivered it, so the handler's sig_atomic_t and the test thread's look like
// a race.  They are the interruption count and nothing else.
#if defined(__SANITIZE_THREAD__)
extern "C" void AnnotateBenignRaceSized(
  const char * file, int line, const volatile void * address,
  long size, const char * description);
#endif

extern "C" void gcu_test_retired_wait_interrupt(int) {
  g_retired_wait_interrupts = g_retired_wait_interrupts + 1;
}

/// The first field of /proc/self/task/<tid>/syscall is the syscall the thread
/// is inside, or -1 when it is in user code.
bool read_task_syscall(uint32_t tid, long * number) {
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/task/%u/syscall", tid);
  FILE * file = std::fopen(path, "re");
  if (!file) {
    return false;
  }
  long value = -1;
  int got = std::fscanf(file, "%ld", &value);
  std::fclose(file);
  if (got != 1) {
    return false;
  }
  *number = value;
  return true;
}

/// True when `tid` stays inside one syscall across a few milliseconds.
///
/// A transient call such as the work-semaphore post does not last that long.
/// The retired-semaphore wait does, while the workers are still on the gate.
bool blocked_in_syscall(uint32_t tid, long * number) {
  long first = -1;
  if (!read_task_syscall(tid, &first) || first < 0) {
    return false;
  }
  gcu_thread_sleep(5);
  long second = -1;
  if (!read_task_syscall(tid, &second) || second != first) {
    return false;
  }
  *number = first;
  return true;
}

} // namespace

TEST(Pool, SetThreadCountShrinkRetriesAnInterruptedRetiredWait) {
#if defined(__SANITIZE_THREAD__)
  AnnotateBenignRaceSized(
    __FILE__, __LINE__, &g_retired_wait_interrupts,
    (long)sizeof g_retired_wait_interrupts,
    "retired-wait interruption count");
#endif
  struct sigaction action = {};
  action.sa_handler = gcu_test_retired_wait_interrupt;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  struct sigaction previous = {};
  ASSERT_EQ(0, sigaction(SIGUSR1, &action, &previous));
  struct RestoreSignal {
    struct sigaction previous;
    ~RestoreSignal() { sigaction(SIGUSR1, &previous, nullptr); }
  } restore{previous};

  Gate gate;
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  bool queued = gcu_pool_enqueue(pool, gate_task, &gate)
    && gcu_pool_enqueue(pool, gate_task, &gate);
  bool held = until([&] {
    return gcu_pool_count_active(pool) == 2;
  });
  if (!queued || !held) {
    gate.open.store(true);
    FAIL() << "workers never occupied the gate";
  }

  std::atomic<uint32_t> tid{0};
  std::atomic<bool> finished{false};
  bool accepted = false;
  std::thread resizer([&] {
    tid.store((uint32_t)gcu_thread_get_current_id());
    accepted = gcu_pool_set_thread_count(pool, 1);
    finished.store(true);
  });

  bool shrinking = until([&] {
    if (finished.load() || tid.load() == 0) {
      return false;
    }
    GCU_MUTEX_LOCK(pool->mutex);
    bool retiring = pool->retire > 0;
    GCU_MUTEX_UNLOCK(pool->mutex);
    return retiring;
  });
  if (!shrinking) {
    gate.open.store(true);
    resizer.join();
    FAIL() << "shrink never asked a worker to leave";
  }

  // The gate still holds both workers, so the retired semaphore stays empty
  // and the resizer cannot leave gcu_pool_wait_retired.  A syscall that is
  // still the same one a few milliseconds later is that wait.
  long blocked = -1;
  bool waiting = until([&] {
    if (finished.load()) {
      return false;
    }
    return blocked_in_syscall(tid.load(), &blocked);
  });

  g_retired_wait_interrupts = 0;
  bool signaled = false;
  if (waiting && !finished.load()) {
    signaled = pthread_kill(resizer.native_handle(), SIGUSR1) == 0;
  }
  // The handler is installed without SA_RESTART, so sem_wait returns EINTR
  // and the shrink's retry loop waits again.  The call must still be inside
  // that wait: a token was not taken, and a hard error would have returned.
  bool delivered = signaled && until([&] {
    return g_retired_wait_interrupts > 0 || finished.load();
  }) && g_retired_wait_interrupts > 0 && !finished.load();

  long again = -1;
  bool retried = delivered && until([&] {
    if (finished.load()) {
      return false;
    }
    return blocked_in_syscall(tid.load(), &again) && again == blocked;
  });

  gate.open.store(true);
  resizer.join();

  ASSERT_TRUE(waiting);
  ASSERT_TRUE(delivered);
  ASSERT_TRUE(retried);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(1u, gcu_pool_count_threads(pool));
  guard.destroy();
}
#endif

TEST(Pool, SetThreadCountLeavesTheSameCountAlone) {
  GCU_Pool_Config config = {};
  config.thread_count = 3;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  EXPECT_TRUE(gcu_pool_set_thread_count(pool, 3));
  EXPECT_EQ(3u, gcu_pool_count_threads(pool));
  guard.destroy();
}

TEST(Pool, SetThreadCountAutoMatchesProcessorCount) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  EXPECT_TRUE(gcu_pool_set_thread_count(pool, GCU_POOL_THREADS_AUTO));
  EXPECT_EQ(gcu_thread_get_num_processors(), gcu_pool_count_threads(pool));
  guard.destroy();
}

TEST(Pool, SetThreadCountRefusesZero) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  EXPECT_FALSE(gcu_pool_set_thread_count(pool, 0));
  EXPECT_EQ(2u, gcu_pool_count_threads(pool));
  guard.destroy();
}

TEST(Pool, SetThreadCountRefusesAnInlinePool) {
  GCU_Pool_Config config = {};
  config.thread_count = 0;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  EXPECT_FALSE(gcu_pool_set_thread_count(pool, 4));
  EXPECT_TRUE(gcu_pool_is_inline(pool));
  EXPECT_EQ(0u, gcu_pool_count_threads(pool));
  guard.destroy();
}

TEST(Pool, SetThreadCountRefusesNull) {
  EXPECT_FALSE(gcu_pool_set_thread_count(NULL, 4));
}

struct WorkerResize {
  GCU_Pool * pool;
  GCU_Thread id;
  bool accepted;
};

int worker_resize_task(void * ctx) {
  WorkerResize * arg = (WorkerResize *)ctx;
  arg->id = gcu_thread_get_current_id();
  arg->accepted = gcu_pool_set_thread_count(arg->pool, 4);
  return 0;
}

TEST(Pool, SetThreadCountFromAWorkerIsRefused) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  WorkerResize arg{pool, 0, true};
  ASSERT_TRUE(gcu_pool_enqueue(pool, worker_resize_task, &arg));
  EXPECT_EQ(0, gcu_pool_wait(pool));

  EXPECT_FALSE(arg.accepted);
  EXPECT_EQ(2u, gcu_pool_count_threads(pool));
  bool joined = true;
  ASSERT_EQ(0, gcu_thread_is_joined(arg.id, &joined));
  EXPECT_FALSE(joined);
  guard.destroy();
}

struct FailState {
  std::atomic<bool> fail{false};
};

void * fail_malloc(void *, size_t size) {
  return malloc(size ? size : 1);
}

void * fail_calloc(void * ctx, size_t nitems, size_t size) {
  if (static_cast<FailState *>(ctx)->fail.load()) {
    return nullptr;
  }
  if (nitems == 0 || size == 0) {
    return calloc(1, 1);
  }
  return calloc(nitems, size);
}

void * fail_realloc(void *, void * ptr, size_t size) {
  return realloc(ptr, size ? size : 1);
}

void fail_free(void *, void * ptr) {
  free(ptr);
}

TEST(Pool, SetThreadCountAllocationFailureLeavesTheCountUnchanged) {
  FailState state;
  GCU_Allocator allocator = {};
  allocator.ctx = &state;
  allocator.malloc_fn = fail_malloc;
  allocator.calloc_fn = fail_calloc;
  allocator.realloc_fn = fail_realloc;
  allocator.free_fn = fail_free;

  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.allocator = &allocator;

  GCU_Pool * pool = gcu_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  PoolGuard guard(pool);

  state.fail.store(true);
  EXPECT_FALSE(gcu_pool_set_thread_count(pool, 4));
  EXPECT_EQ(2u, gcu_pool_count_threads(pool));
  EXPECT_FALSE(gcu_pool_set_thread_count(pool, 1));
  EXPECT_EQ(2u, gcu_pool_count_threads(pool));

  state.fail.store(false);
  g_ran.store(0);
  ASSERT_TRUE(gcu_pool_enqueue(pool, count_task, NULL));
  EXPECT_EQ(0, gcu_pool_wait(pool));
  EXPECT_EQ(1, g_ran.load());
  guard.destroy();
}

namespace {

/// Tears a managed pool down however the test ends.
struct ManagedGuard {
  GCU_Managed_Pool * pool;

  explicit ManagedGuard(GCU_Managed_Pool * p) : pool(p) {}
  ManagedGuard(const ManagedGuard &) = delete;
  ManagedGuard & operator=(const ManagedGuard &) = delete;

  void destroy() {
    gcu_managed_pool_destroy(pool);
    pool = nullptr;
  }

  void abandon() {
    gcu_managed_pool_abandon(pool);
    pool = nullptr;
  }

  void release() {
    pool = nullptr;
  }

  ~ManagedGuard() {
    if (pool) {
      gcu_managed_pool_abandon(pool);
    }
  }
};

/// Opens every gate and releases every allocator stall this test armed.
///
/// Declared after ManagedGuard so it runs first on the way out.  A failed
/// assert must not leave the manager blocked in a stall or a worker blocked
/// on a gate, or the guard's join hangs.
struct ReleaseFirst {
  Gate * gate;
  std::atomic<bool> * release_calloc;
  std::atomic<bool> * release_free;

  ~ReleaseFirst() {
    if (release_calloc) {
      release_calloc->store(true);
    }
    if (release_free) {
      release_free->store(true);
    }
    if (gate) {
      gate->open.store(true);
    }
  }
};

struct HookAlloc {
  std::atomic<int> stall_calloc{0};
  std::atomic<bool> release_calloc{false};
  std::atomic<bool> calloc_done{false};
  std::atomic<bool> fail_calloc{false};
  std::atomic<int> stall_free{0};
  std::atomic<bool> release_free{false};
};

void * hook_malloc(void *, size_t size) {
  return std::malloc(size ? size : 1);
}

void * hook_calloc(void * ctx, size_t nitems, size_t size) {
  HookAlloc * hook = static_cast<HookAlloc *>(ctx);
  bool stalled = false;
  if (hook->stall_calloc.load() == 1) {
    stalled = true;
    hook->stall_calloc.store(2);
    while (!hook->release_calloc.load()) {
      gcu_thread_yield();
    }
  }
  void * block = nullptr;
  if (!hook->fail_calloc.load()) {
    if (nitems == 0 || size == 0) {
      block = std::calloc(1, 1);
    }
    else {
      block = std::calloc(nitems, size);
    }
  }
  if (stalled) {
    hook->calloc_done.store(true);
  }
  return block;
}

void * hook_realloc(void *, void * ptr, size_t size) {
  return std::realloc(ptr, size ? size : 1);
}

void hook_free(void * ctx, void * ptr) {
  HookAlloc * hook = static_cast<HookAlloc *>(ctx);
  if (hook->stall_free.load() == 1) {
    hook->stall_free.store(2);
    while (!hook->release_free.load()) {
      gcu_thread_yield();
    }
    hook->stall_free.store(0);
  }
  std::free(ptr);
}

GCU_Allocator hook_allocator(HookAlloc * hook) {
  GCU_Allocator allocator = {};
  allocator.ctx = hook;
  allocator.malloc_fn = hook_malloc;
  allocator.calloc_fn = hook_calloc;
  allocator.realloc_fn = hook_realloc;
  allocator.free_fn = hook_free;
  return allocator;
}

int add_task(void * ctx) {
  static_cast<std::atomic<int> *>(ctx)->fetch_add(1);
  return 0;
}

struct ManagedResize {
  GCU_Managed_Pool * pool;
  GCU_Thread id;
  bool accepted;
};

int managed_resize_task(void * ctx) {
  ManagedResize * arg = static_cast<ManagedResize *>(ctx);
  arg->id = gcu_thread_get_current_id();
  arg->accepted = gcu_managed_pool_set_thread_count(arg->pool, 4);
  return 0;
}

struct CompleteProbe {
  std::atomic<int> ran{0};
  std::atomic<int> completed{0};
  std::atomic<int> status{-1};
};

int complete_task(void * ctx) {
  static_cast<CompleteProbe *>(ctx)->ran.fetch_add(1);
  return 7;
}

void complete_cb(void * ctx, int status, void * user_data) {
  (void)ctx;
  CompleteProbe * probe = static_cast<CompleteProbe *>(user_data);
  probe->status.store(status);
  probe->completed.fetch_add(1);
}

bool bytes_are_zero(const void * memory, size_t size) {
  const unsigned char * bytes = static_cast<const unsigned char *>(memory);
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

/// Runs as the queued work a shrink must finish, then tries to enqueue once
/// abandon has taken over.  The later enqueue is the work abandon discards.
struct AfterShrink {
  GCU_Managed_Pool * pool;
  std::atomic<bool> * started;
  std::atomic<int> * after_ran;
  bool accepted;
};

int after_shrink_task(void * ctx) {
  AfterShrink * arg = static_cast<AfterShrink *>(ctx);
  arg->started->store(true);
  while (!gcu_managed_pool_is_shutting_down(arg->pool)) {
    gcu_thread_yield();
  }
  arg->accepted = gcu_managed_pool_enqueue(
    arg->pool, add_task, arg->after_ran);
  return 0;
}

#if defined(__linux__)
int process_thread_count() {
  FILE * file = std::fopen("/proc/self/status", "re");
  if (!file) {
    return -1;
  }
  char line[256];
  int count = -1;
  while (std::fgets(line, sizeof line, file)) {
    int value = 0;
    if (std::sscanf(line, "Threads: %d", &value) == 1) {
      count = value;
      break;
    }
  }
  std::fclose(file);
  return count;
}
#endif

} // namespace

//
// Managed pool.  The manager is the only caller of resize and teardown, so a
// worker can request a new count without being joined by that request.
//

TEST(ManagedPool, CreateStartsAtTheRequestedCount) {
  GCU_Pool_Config config = {};
  config.thread_count = 4;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  EXPECT_EQ(4u, gcu_managed_pool_count_threads(pool));
  EXPECT_EQ(4u, gcu_managed_pool_desired_thread_count(pool));
  EXPECT_FALSE(gcu_managed_pool_is_shutting_down(pool));
  guard.destroy();
}

TEST(ManagedPool, CreateDefaultStoresAutoAndUsesTheProcessorCount) {
  GCU_Managed_Pool * pool = gcu_managed_pool_create(NULL);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  EXPECT_EQ(GCU_POOL_THREADS_AUTO,
    gcu_managed_pool_desired_thread_count(pool));
  EXPECT_EQ(gcu_thread_get_num_processors(),
    gcu_managed_pool_count_threads(pool));
  guard.destroy();
}

TEST(ManagedPool, CreateRefusesZeroAndLeavesNoThread) {
#if defined(__linux__)
  int before = process_thread_count();
  ASSERT_GT(before, 0);
#endif

  GCU_Pool_Config config = {};
  config.thread_count = 0;
  EXPECT_EQ(nullptr, gcu_managed_pool_create(&config));

  GCU_Managed_Pool storage;
  std::memset(&storage, 0xab, sizeof storage);
  EXPECT_FALSE(gcu_managed_pool_create_in_place(&storage, &config));
  EXPECT_TRUE(bytes_are_zero(&storage, sizeof storage));
  gcu_managed_pool_destroy_in_place(&storage);

#if defined(__linux__)
  EXPECT_EQ(before, process_thread_count());
#endif
}

TEST(ManagedPool, InPlaceDestroyJoinsTheWorkers) {
  GCU_Managed_Pool storage;
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  ASSERT_TRUE(gcu_managed_pool_create_in_place(&storage, &config));

  Gate gate;
  GCU_Thread ids[2] = {};
  std::atomic<size_t> filled{0};
  IdGate arg{&gate, ids, &filled};
  ReleaseFirst release{&gate, nullptr, nullptr};

  bool queued = gcu_managed_pool_enqueue(&storage, id_gate_task, &arg)
    && gcu_managed_pool_enqueue(&storage, id_gate_task, &arg);
  bool inside = until([&] {
    return gcu_managed_pool_count_active(&storage) == 2;
  });
  gate.open.store(true);
  ASSERT_TRUE(queued);
  ASSERT_TRUE(inside);
  EXPECT_EQ(0, gcu_managed_pool_wait(&storage));

  gcu_managed_pool_destroy_in_place(&storage);

  for (GCU_Thread id : ids) {
    bool joined = false;
    ASSERT_EQ(0, gcu_thread_is_joined(id, &joined));
    EXPECT_TRUE(joined);
  }
}

TEST(ManagedPool, SetRecordsResizeWhileManagerIsInsideShrink) {
  HookAlloc hook;
  GCU_Allocator allocator = hook_allocator(&hook);
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.allocator = &allocator;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);
  ReleaseFirst release{nullptr, nullptr, &hook.release_free};

  hook.stall_free.store(1);
  bool shrink_requested = gcu_managed_pool_set_thread_count(pool, 1);
  bool blocked = until([&] {
    return hook.stall_free.load() == 2;
  });
  size_t shrink_target = blocked ? gcu_managed_pool_count_threads(pool) : 0;
  bool recorded = false;
  size_t live_at_return = 0;
  size_t desired = 0;
  if (blocked) {
    recorded = gcu_managed_pool_set_thread_count(pool, 4);
    live_at_return = gcu_managed_pool_count_threads(pool);
    desired = gcu_managed_pool_desired_thread_count(pool);
  }
  hook.release_free.store(true);
  // Staying at the shrink target after the manager is free to run is a
  // missed wakeup: the post during the shrink has to be applied.
  bool applied = blocked && until([&] {
    return gcu_managed_pool_count_threads(pool) == 4;
  });

  ASSERT_TRUE(shrink_requested);
  ASSERT_TRUE(blocked);
  ASSERT_EQ(1u, shrink_target);
  ASSERT_TRUE(recorded);
  ASSERT_EQ(1u, live_at_return);
  ASSERT_EQ(4u, desired);
  ASSERT_TRUE(applied);
  guard.destroy();
}

TEST(ManagedPool, SetFromAWorkerDoesNotJoinThatWorker) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  ManagedResize arg{pool, 0, false};
  ASSERT_TRUE(gcu_managed_pool_enqueue(pool, managed_resize_task, &arg));
  EXPECT_EQ(0, gcu_managed_pool_wait(pool));

  bool joined = true;
  ASSERT_EQ(0, gcu_thread_is_joined(arg.id, &joined));
  bool reached = until([&] {
    return gcu_managed_pool_count_threads(pool) == 4;
  });

  EXPECT_TRUE(arg.accepted);
  EXPECT_FALSE(joined);
  ASSERT_TRUE(reached);
  guard.destroy();
}

TEST(ManagedPool, SetCallsWhileShrinkIsInProgressCoalesce) {
  HookAlloc hook;
  GCU_Allocator allocator = hook_allocator(&hook);
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.allocator = &allocator;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);
  ReleaseFirst release{nullptr, nullptr, &hook.release_free};

  hook.stall_free.store(1);
  bool shrink_requested = gcu_managed_pool_set_thread_count(pool, 1);
  bool blocked = until([&] {
    return hook.stall_free.load() == 2;
  });
  bool first = false;
  bool second = false;
  bool third = false;
  size_t desired = 0;
  if (blocked) {
    first = gcu_managed_pool_set_thread_count(pool, 3);
    second = gcu_managed_pool_set_thread_count(pool, 6);
    third = gcu_managed_pool_set_thread_count(pool, 4);
    desired = gcu_managed_pool_desired_thread_count(pool);
  }
  hook.release_free.store(true);
  bool applied = blocked && until([&] {
    return gcu_managed_pool_count_threads(pool) == 4;
  });

  ASSERT_TRUE(shrink_requested);
  ASSERT_TRUE(blocked);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  ASSERT_EQ(4u, desired);
  ASSERT_TRUE(applied);
  guard.destroy();
}

TEST(ManagedPool, SetRefusesZeroWithoutChangingTheStoredCount) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  EXPECT_FALSE(gcu_managed_pool_set_thread_count(pool, 0));
  EXPECT_EQ(2u, gcu_managed_pool_desired_thread_count(pool));
  EXPECT_EQ(2u, gcu_managed_pool_count_threads(pool));
  guard.destroy();
}

TEST(ManagedPool, SetAutoStaysAutoAndResolvesToTheProcessorCount) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  ASSERT_TRUE(gcu_managed_pool_set_thread_count(pool, GCU_POOL_THREADS_AUTO));
  EXPECT_EQ(GCU_POOL_THREADS_AUTO,
    gcu_managed_pool_desired_thread_count(pool));
  bool resolved = until([&] {
    return gcu_managed_pool_count_threads(pool)
      == gcu_thread_get_num_processors();
  });
  ASSERT_TRUE(resolved);
  guard.destroy();
}

TEST(ManagedPool, GrowAllocationFailureDoesNotSpinAndDestroyReturns) {
  HookAlloc hook;
  GCU_Allocator allocator = hook_allocator(&hook);
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.allocator = &allocator;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);
  ReleaseFirst release{nullptr, &hook.release_calloc, nullptr};

  hook.fail_calloc.store(true);
  hook.stall_calloc.store(1);
  bool recorded = gcu_managed_pool_set_thread_count(pool, 4);
  bool entered = until([&] {
    return hook.stall_calloc.load() == 2;
  });
  size_t live = gcu_managed_pool_count_threads(pool);
  size_t desired = gcu_managed_pool_desired_thread_count(pool);
  hook.release_calloc.store(true);
  bool failed = entered && until([&] {
    return hook.calloc_done.load();
  });

  guard.release();
  std::atomic<bool> destroyed{false};
  std::thread joiner([&] {
    gcu_managed_pool_destroy(pool);
    destroyed.store(true);
  });
  bool returned = until([&] {
    return destroyed.load();
  });
  joiner.join();

  ASSERT_TRUE(recorded);
  ASSERT_TRUE(entered);
  ASSERT_TRUE(failed);
  EXPECT_EQ(2u, live);
  EXPECT_EQ(4u, desired);
  ASSERT_TRUE(returned);
}

TEST(ManagedPool, FailedApplyRetriesWhenTheSameCountIsSetAgain) {
  HookAlloc hook;
  GCU_Allocator allocator = hook_allocator(&hook);
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.allocator = &allocator;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);
  ReleaseFirst release{nullptr, &hook.release_calloc, nullptr};

  hook.fail_calloc.store(true);
  hook.stall_calloc.store(1);
  bool recorded = gcu_managed_pool_set_thread_count(pool, 4);
  bool entered = until([&] {
    return hook.stall_calloc.load() == 2;
  });
  hook.release_calloc.store(true);
  bool failed = entered && until([&] {
    return hook.calloc_done.load();
  });
  size_t live_after_failure = gcu_managed_pool_count_threads(pool);
  // A manager that retries a failed grow on its own would enter this stall
  // without another set.  One that waits does not.
  hook.release_calloc.store(false);
  hook.stall_calloc.store(1);
  hook.fail_calloc.store(false);
  auto retry_deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  bool spun = false;
  while (std::chrono::steady_clock::now() < retry_deadline) {
    if (hook.stall_calloc.load() == 2) {
      spun = true;
      break;
    }
    std::this_thread::yield();
  }
  bool retried = failed && !spun
    && gcu_managed_pool_set_thread_count(pool, 4);
  bool entered_again = retried && until([&] {
    return hook.stall_calloc.load() == 2;
  });
  hook.release_calloc.store(true);
  bool reached = entered_again && until([&] {
    return gcu_managed_pool_count_threads(pool) == 4;
  });

  ASSERT_TRUE(recorded);
  ASSERT_TRUE(entered);
  ASSERT_TRUE(failed);
  EXPECT_EQ(2u, live_after_failure);
  EXPECT_EQ(4u, gcu_managed_pool_desired_thread_count(pool));
  EXPECT_FALSE(spun);
  ASSERT_TRUE(retried);
  ASSERT_TRUE(entered_again);
  ASSERT_TRUE(reached);
  guard.destroy();
}

TEST(ManagedPool, SetRefusesNull) {
  EXPECT_FALSE(gcu_managed_pool_set_thread_count(NULL, 4));
  EXPECT_EQ(0u, gcu_managed_pool_desired_thread_count(NULL));
  gcu_managed_pool_destroy(NULL);
  gcu_managed_pool_abandon(NULL);
  gcu_managed_pool_destroy_in_place(NULL);
  gcu_managed_pool_abandon_in_place(NULL);
}

TEST(ManagedPool, EnqueueRunsAndWaitReturnsZero) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  std::atomic<int> ran{0};
  ASSERT_TRUE(gcu_managed_pool_enqueue(pool, add_task, &ran));
  EXPECT_EQ(0, gcu_managed_pool_wait(pool));
  EXPECT_EQ(1, ran.load());

  CompleteProbe probe;
  ASSERT_TRUE(gcu_managed_pool_enqueue_cb(
    pool, complete_task, &probe, complete_cb, &probe));
  EXPECT_EQ(7, gcu_managed_pool_wait(pool));
  EXPECT_EQ(1, probe.ran.load());
  EXPECT_EQ(1, probe.completed.load());
  EXPECT_EQ(7, probe.status.load());
  gcu_managed_pool_clear_error(pool);
  EXPECT_EQ(0, gcu_managed_pool_wait(pool));

  ASSERT_TRUE(gcu_managed_pool_enqueue_wait(pool, add_task, &ran));
  EXPECT_EQ(0, gcu_managed_pool_wait(pool));
  EXPECT_EQ(2, ran.load());
  guard.destroy();
}

TEST(ManagedPool, DestroyReturnsAfterWorkersAreJoined) {
  GCU_Pool_Config config = {};
  config.thread_count = 2;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  Gate gate;
  GCU_Thread ids[2] = {};
  std::atomic<size_t> filled{0};
  IdGate arg{&gate, ids, &filled};
  ReleaseFirst release{&gate, nullptr, nullptr};

  bool queued = gcu_managed_pool_enqueue(pool, id_gate_task, &arg)
    && gcu_managed_pool_enqueue(pool, id_gate_task, &arg);
  bool inside = until([&] {
    return gcu_managed_pool_count_active(pool) == 2;
  });
  if (!queued || !inside) {
    gate.open.store(true);
    FAIL() << "workers never occupied the gate";
  }

  guard.release();
  std::atomic<bool> destroyed{false};
  std::thread joiner([&] {
    gcu_managed_pool_destroy(pool);
    destroyed.store(true);
  });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  bool finished_early = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (destroyed.load()) {
      finished_early = true;
      break;
    }
    std::this_thread::yield();
  }
  gate.open.store(true);
  joiner.join();

  ASSERT_FALSE(finished_early);
  ASSERT_TRUE(destroyed.load());
  for (GCU_Thread id : ids) {
    bool joined = false;
    ASSERT_EQ(0, gcu_thread_is_joined(id, &joined));
    EXPECT_TRUE(joined);
  }
}

TEST(ManagedPool, AbandonSkipsQueuedWorkAndFinishesRunningWork) {
  GCU_Pool_Config config = {};
  config.thread_count = 1;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  Gate gate;
  std::atomic<int> running{0};
  std::atomic<int> queued_ran{0};
  GateCount held{&gate, &running};
  ReleaseFirst release{&gate, nullptr, nullptr};

  bool held_task = gcu_managed_pool_enqueue(pool, gate_count_task, &held);
  bool busy = until([&] {
    return gcu_managed_pool_count_active(pool) == 1;
  });
  bool queued = busy && gcu_managed_pool_enqueue(pool, add_task, &queued_ran);
  size_t waiting = queued ? gcu_managed_pool_count_queued(pool) : 0;

  guard.release();
  std::atomic<bool> abandoned{false};
  std::thread joiner([&] {
    gcu_managed_pool_abandon(pool);
    abandoned.store(true);
  });
  bool stopping = until([&] {
    return gcu_managed_pool_is_shutting_down(pool);
  });
  int queued_at_stop = queued_ran.load();
  bool refused = false;
  size_t desired_at_stop = 0;
  if (stopping) {
    refused = !gcu_managed_pool_set_thread_count(pool, 4);
    desired_at_stop = gcu_managed_pool_desired_thread_count(pool);
  }
  gate.open.store(true);
  joiner.join();

  ASSERT_TRUE(held_task);
  ASSERT_TRUE(busy);
  ASSERT_TRUE(queued);
  EXPECT_GT(waiting, 0u);
  ASSERT_TRUE(stopping);
  EXPECT_TRUE(refused);
  EXPECT_EQ(1u, desired_at_stop);
  EXPECT_EQ(0, queued_at_stop);
  EXPECT_EQ(0, queued_ran.load());
  EXPECT_EQ(1, running.load());
  ASSERT_TRUE(abandoned.load());
}

TEST(ManagedPool, AbandonDuringShrinkFinishesQueuedWorkFirst) {
  HookAlloc hook;
  GCU_Allocator allocator = hook_allocator(&hook);
  GCU_Pool_Config config = {};
  config.thread_count = 2;
  config.allocator = &allocator;

  GCU_Managed_Pool * pool = gcu_managed_pool_create(&config);
  ASSERT_NE(nullptr, pool);
  ManagedGuard guard(pool);

  OneGate slots[2];
  struct OpenSlots {
    OneGate * slots;
    std::atomic<bool> * release_calloc;
    ~OpenSlots() {
      release_calloc->store(true);
      slots[0].open.store(true);
      slots[1].open.store(true);
    }
  } open{slots, &hook.release_calloc};

  bool queued = gcu_managed_pool_enqueue(pool, one_gate_task, &slots[0])
    && gcu_managed_pool_enqueue(pool, one_gate_task, &slots[1]);
  bool held = until([&] {
    return slots[0].entered.load() && slots[1].entered.load()
      && gcu_managed_pool_count_active(pool) == 2;
  });

  std::atomic<bool> started{false};
  std::atomic<int> after_ran{0};
  AfterShrink after{pool, &started, &after_ran, true};
  bool shrink_work = held
    && gcu_managed_pool_enqueue(pool, after_shrink_task, &after);
  bool waiting = shrink_work && until([&] {
    return gcu_managed_pool_count_queued(pool) >= 1;
  });

  hook.stall_calloc.store(1);
  bool shrink_requested = waiting && gcu_managed_pool_set_thread_count(pool, 1);
  bool inside = shrink_requested && until([&] {
    return hook.stall_calloc.load() == 2;
  });
  size_t still_queued = inside ? gcu_managed_pool_count_queued(pool) : 0;

  std::thread joiner;
  if (inside) {
    guard.release();
    joiner = std::thread([&] {
      gcu_managed_pool_abandon(pool);
    });
  }
  hook.release_calloc.store(true);
  // One worker leaves its gate and runs the queued task.  The other stays,
  // so the surplus worker cannot exit and the shrink cannot finish yet.
  slots[0].open.store(true);
  bool ran_shrink_work = inside && until([&] {
    return started.load();
  });
  slots[1].open.store(true);
  if (joiner.joinable()) {
    joiner.join();
  }

  ASSERT_TRUE(queued);
  ASSERT_TRUE(held);
  ASSERT_TRUE(shrink_work);
  ASSERT_TRUE(waiting);
  ASSERT_TRUE(shrink_requested);
  ASSERT_TRUE(inside);
  EXPECT_GE(still_queued, 1u);
  ASSERT_TRUE(ran_shrink_work);
  // The task tried to enqueue only after shutdown was visible.  That work
  // is what abandon discards: it was not run.
  EXPECT_FALSE(after.accepted);
  EXPECT_EQ(0, after_ran.load());
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
