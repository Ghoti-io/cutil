/**
 * @file
 *
 * Tests for the event loop, loopback only, on port 0.
 *
 * What each group is for, since several only fail against one specific wrong
 * implementation (`make check-loop-defects` plants them and requires the test
 * named here to notice):
 *
 *   - CancelledReadCompletesOnceAndNeverTouchesItsBuffer fails if a cancel
 *     reports `CANCELLED` before the loop has let go of the buffer: it checks
 *     that the callback has not run when cancel returns, and that bytes the
 *     peer sends afterwards do not land in the buffer.
 *   - CancelledReadBufferMayBeFreedByItsCompletion frees the buffer inside
 *     the completion callback and then sends data; under AddressSanitizer a
 *     loop that is still reading into it is a heap-use-after-free report.
 *   - TimersFireInDeadlineOrderAndNotBeforeTheirTime fails if the timer queue
 *     is ordered by when a timer was started.
 *   - PostFromAnotherThreadWakesAWaitingLoop waits in the OS with no timeout;
 *     a post that does not wake the loop hangs it, and the hang guard names
 *     the test.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gtest/gtest.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/error.h>
#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/cutil/loop.h>
#include <ghoti.io/cutil/socket.h>

#include "hang-guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32) && defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GCU_TEST_HAVE_VALGRIND_H 1
#endif
#endif

#ifndef _WIN32
#include <csignal>
#include <malloc.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define GCU_TEST_SANITIZED 1
#elif defined(__SANITIZE_THREAD__)
#define GCU_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define GCU_TEST_SANITIZED 1
#endif
#endif

using namespace std;
using namespace std::chrono;

namespace {

#define REQUIRE_LOOP() \
  do { \
    if (!GCU_LOOP_SUPPORTED) { \
      GTEST_SKIP() << "no event loop on this platform"; \
    } \
  } while (0)

/**
 * A record with a counter, a copy of its completion and the thread its
 * callback ran on.  Never copied or moved: the loop holds its address.
 */
struct Rec {
  GCU_Loop_Op op;
  int calls = 0;
  GCU_Loop_Completion r;
  thread::id tid;
  function<void(Rec &)> then;

  Rec() {
    gcu_loop_op_init(&op, &Rec::cb, this);
    memset(&r, 0, sizeof(r));
  }
  Rec(const Rec &) = delete;
  Rec & operator=(const Rec &) = delete;

  static void cb(GCU_Loop_Op *, void * user) {
    Rec * self = static_cast<Rec *>(user);
    ++self->calls;
    self->r = self->op.result;
    self->tid = this_thread::get_id();
    if (self->then) {
      self->then(*self);
    }
  }
};

class LoopTest : public ghoti_test::HangGuarded<20> {
protected:
  GCU_Loop * loop = nullptr;
  thread::id mainThread = this_thread::get_id();

  void SetUp() override {
    ghoti_test::HangGuarded<20>::SetUp();
    REQUIRE_LOOP();
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_create(&loop, nullptr));
  }

  void TearDown() override {
    if (loop) {
      EXPECT_EQ(GCU_LOOP_OK, gcu_loop_destroy(loop));
      loop = nullptr;
    }
    ghoti_test::HangGuarded<20>::TearDown();
  }

  /** Run the loop until @p done or the time is up. */
  bool pump(const function<bool()> & done, int limitMs = 8000) {
    auto end = steady_clock::now() + milliseconds(limitMs);
    while (!done()) {
      if (steady_clock::now() > end) return false;
      EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, 10));
    }
    return true;
  }

  /** Run the loop for a fixed time, delivering whatever happens. */
  void settle(int ms) {
    auto end = steady_clock::now() + milliseconds(ms);
    while (steady_clock::now() < end) {
      EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, 5));
    }
  }
};

/**
 * A connected pair on loopback: `client` and `server` are the two ends, and
 * `listener` is still open.
 */
struct Conn {
  GCU_Socket * listener = nullptr;
  GCU_Socket * client = nullptr;
  GCU_Socket * server = nullptr;
  GCU_Socket_Address address;
  ~Conn() {
    gcu_socket_close(client);
    gcu_socket_close(server);
    gcu_socket_close(listener);
  }
};

bool familyUsable(GCU_Socket_Family family) {
  GCU_Socket * s = nullptr;
  if (gcu_socket_create(&s, family, GCU_SOCKET_STREAM, nullptr)
      != GCU_SOCKET_OK) {
    return false;
  }
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, family, 0);
  bool ok = gcu_socket_bind(s, &a) == GCU_SOCKET_OK;
  gcu_socket_close(s);
  return ok;
}

#define REQUIRE_IPV6() \
  do { \
    if (!familyUsable(GCU_SOCKET_IPV6)) { \
      GTEST_SKIP() << "::1 is not available on this host"; \
    } \
  } while (0)

class LoopNet : public LoopTest {
protected:
  void connectPair(Conn & c, GCU_Socket_Family family = GCU_SOCKET_IPV4) {
    ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_create(
      &c.listener, family, GCU_SOCKET_STREAM, nullptr));
    GCU_Socket_Address any;
    ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_address_loopback(&any, family, 0));
    ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(c.listener, &any));
    ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(c.listener, 16));
    ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(c.listener, &c.address));
    ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_create(
      &c.client, family, GCU_SOCKET_STREAM, nullptr));

    Rec accepted;
    Rec connected;
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &accepted.op, c.listener));
    ASSERT_EQ(GCU_LOOP_OK,
      gcu_loop_connect(loop, &connected.op, c.client, &c.address));
    ASSERT_TRUE(pump([&] { return accepted.calls && connected.calls; }));
    ASSERT_EQ(GCU_LOOP_OK, accepted.r.status);
    ASSERT_EQ(GCU_LOOP_OK, connected.r.status);
    ASSERT_NE(nullptr, accepted.r.accepted);
    c.server = accepted.r.accepted;
  }

  /** Write @p n bytes and wait for the write to finish. */
  void writeAll(GCU_Socket * s, const void * data, size_t n) {
    Rec w;
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &w.op, s, data, n));
    ASSERT_TRUE(pump([&] { return w.calls; }));
    ASSERT_EQ(GCU_LOOP_OK, w.r.status);
    ASSERT_EQ(n, w.r.bytes);
  }
};

//
// Basics.
//

TEST_F(LoopTest, AnIdleLoopCreatesAndDestroys) {
  EXPECT_EQ(0u, gcu_loop_pending(loop));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, 0));
}

TEST_F(LoopTest, CallsRefuseNullAndUninitialisedRecords) {
  GCU_Loop * made = reinterpret_cast<GCU_Loop *>(0x1);
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_create(nullptr, nullptr));
  EXPECT_EQ(reinterpret_cast<GCU_Loop *>(0x1), made);
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_destroy(nullptr));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_run(nullptr));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_run_once(nullptr, 0));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_stop(nullptr));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_wake(nullptr));
  EXPECT_EQ(0u, gcu_loop_pending(nullptr));

  Rec rec;
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_timer_start(nullptr, &rec.op, 1));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_timer_start(loop, nullptr, 1));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_cancel(loop, nullptr));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_post(loop, nullptr));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_fiber_wait(nullptr));

  // A record that was never initialised has no size, and is not guessed at.
  GCU_Loop_Op raw;
  memset(&raw, 0, sizeof(raw));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_timer_start(loop, &raw, 1));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_post(loop, &raw));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_cancel(loop, &raw));
  raw.size = sizeof(raw) - 1;
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_timer_start(loop, &raw, 1));

  // A post needs something to run.
  Rec nothing;
  nothing.op.callback = nullptr;
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_post(loop, &nothing.op));
}

TEST_F(LoopTest, EveryResultHasADistinctName) {
  for (int r = 0; r < (int)GCU_LOOP_RESULT_COUNT; ++r) {
    const char * name = gcu_loop_result_string((GCU_Loop_Result)r);
    ASSERT_NE(nullptr, name);
    EXPECT_GT(strlen(name), 0u) << r;
    for (int q = 0; q < r; ++q) {
      EXPECT_STRNE(name, gcu_loop_result_string((GCU_Loop_Result)q));
    }
  }
  EXPECT_STREQ("unknown loop result", gcu_loop_result_string((GCU_Loop_Result)99));
}

//
// Timers.
//

TEST_F(LoopTest, TimersFireInDeadlineOrderAndNotBeforeTheirTime) {
  struct Timed : Rec {
    steady_clock::time_point started;
    steady_clock::time_point fired;
  };
  vector<int> order;
  Timed a, b, c;
  int delays[3] = {30, 10, 20};
  Timed * timers[3] = {&a, &b, &c};
  for (int i = 0; i < 3; ++i) {
    timers[i]->then = [&order, i, t = timers[i]](Rec &) {
      static_cast<Timed *>(t)->fired = steady_clock::now();
      order.push_back(i);
    };
  }
  for (int i = 0; i < 3; ++i) {
    timers[i]->started = steady_clock::now();
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &timers[i]->op, delays[i]));
  }
  ASSERT_TRUE(pump([&] { return order.size() == 3; }));
  EXPECT_EQ((vector<int>{1, 2, 0}), order) << "fired in the order 10, 20, 30";
  for (int i = 0; i < 3; ++i) {
    EXPECT_GE(duration_cast<microseconds>(timers[i]->fired - timers[i]->started)
      .count(), delays[i] * 1000) << "timer " << i << " fired early";
    EXPECT_EQ(GCU_LOOP_OK, timers[i]->r.status);
    EXPECT_EQ(1, timers[i]->calls);
    EXPECT_EQ(mainThread, timers[i]->tid);
  }
}

TEST_F(LoopTest, ThousandsOfTimersFireInDeadlineOrder) {
  // Delays are multiples of 5 ms, far enough apart that the microseconds the
  // starts take cannot reorder two of them, so the right order is a stable
  // sort by delay.
  const int n = 2000;
  deque<Rec> timers(n);
  vector<int> delays(n);
  mt19937 rng(12345);
  for (int i = 0; i < n; ++i) delays[i] = 5 * (int)(rng() % 9);
  vector<int> fired;
  for (int i = 0; i < n; ++i) {
    timers[i].then = [&fired, i](Rec &) { fired.push_back(i); };
  }
  for (int i = 0; i < n; ++i) {
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &timers[i].op, delays[i]));
  }
  EXPECT_EQ((size_t)n, gcu_loop_pending(loop));
  ASSERT_TRUE(pump([&] { return fired.size() == (size_t)n; }));
  vector<int> expected(n);
  iota(expected.begin(), expected.end(), 0);
  stable_sort(expected.begin(), expected.end(),
    [&](int x, int y) { return delays[x] < delays[y]; });
  EXPECT_EQ(expected, fired);
  EXPECT_EQ(0u, gcu_loop_pending(loop));
}

TEST_F(LoopTest, StartingAnOperationNeverRunsItsCallbackInline) {
  Rec zero;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &zero.op, 0));
  EXPECT_EQ(0, zero.calls);
  EXPECT_TRUE(gcu_loop_op_is_pending(&zero.op));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, 0));
  EXPECT_EQ(1, zero.calls);
  EXPECT_FALSE(gcu_loop_op_is_pending(&zero.op));
}

TEST_F(LoopTest, ACallbackMayStartItsRecordAgain) {
  Rec t;
  int left = 3;
  t.then = [&](Rec & self) {
    if (--left > 0) {
      EXPECT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &self.op, 1));
    }
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, 1));
  ASSERT_TRUE(pump([&] { return t.calls == 3; }));
  settle(20);
  EXPECT_EQ(3, t.calls);
}

TEST_F(LoopTest, ACancelledTimerCompletesCancelledAndNeverFires) {
  Rec t;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, 20));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_cancel(loop, &t.op));
  EXPECT_EQ(0, t.calls) << "the completion is the loop's, not cancel's";
  ASSERT_TRUE(pump([&] { return t.calls; }));
  EXPECT_EQ(GCU_LOOP_CANCELLED, t.r.status);
  settle(40);
  EXPECT_EQ(1, t.calls);
}

TEST_F(LoopTest, ATimerNearTheDelayClampDoesNotFireAndCancels) {
  // The largest delay an unsigned long holds is clamped inside the library so
  // that adding it to the clock cannot overflow.  It must still be a timer a
  // long way off, not one that fires at once or wraps into the past.
  Rec t;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, ULONG_MAX));
  EXPECT_EQ(1u, gcu_loop_pending(loop));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, 0));
  settle(30);
  EXPECT_EQ(0, t.calls);
  EXPECT_EQ(1u, gcu_loop_pending(loop));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_cancel(loop, &t.op));
  ASSERT_TRUE(pump([&] { return t.calls; }));
  EXPECT_EQ(GCU_LOOP_CANCELLED, t.r.status);
  EXPECT_EQ(0u, gcu_loop_pending(loop));
}

TEST_F(LoopTest, StartingARecordThatIsInFlightIsAStateError) {
  Rec t;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, 1000));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_timer_start(loop, &t.op, 1));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &t.op));
  ASSERT_TRUE(pump([&] { return t.calls; }));
}

TEST_F(LoopTest, CancellingWhatIsNotInFlightIsAStateError) {
  Rec idle;
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_cancel(loop, &idle.op));
  Rec done;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &done.op, 0));
  ASSERT_TRUE(pump([&] { return done.calls; }));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_cancel(loop, &done.op));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_timer_cancel(loop, &done.op));
}

TEST_F(LoopTest, ACancelAfterTheResultIsDecidedLeavesTheResultAlone) {
  // Both timers are due in the same iteration, so both are decided before
  // either callback runs.  The first callback cancels the second, whose
  // result is already fixed: the cancel is refused and the timer is reported
  // as the one that fired, not as a cancelled one.
  Rec first, second;
  GCU_Loop_Result got = GCU_LOOP_OK;
  first.then = [&](Rec &) { got = gcu_loop_cancel(loop, &second.op); };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &first.op, 0));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &second.op, 0));
  ASSERT_TRUE(pump([&] { return first.calls && second.calls; }));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, got);
  EXPECT_EQ(GCU_LOOP_OK, second.r.status);
}

TEST_F(LoopTest, TimerCancelRefusesARecordThatIsNotATimer) {
  Rec posted;
  // A posted record is not a timer, and cannot be cancelled by anyone.
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_post(loop, &posted.op));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_timer_cancel(loop, &posted.op));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_cancel(loop, &posted.op));
  ASSERT_TRUE(pump([&] { return posted.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, posted.r.status);
}

TEST_F(LoopTest, PendingCountsWhatIsInFlight) {
  Rec a, b;
  EXPECT_EQ(0u, gcu_loop_pending(loop));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &a.op, 5000));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &b.op, 5000));
  EXPECT_EQ(2u, gcu_loop_pending(loop));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &a.op));
  EXPECT_EQ(2u, gcu_loop_pending(loop)) << "until the completion is delivered";
  ASSERT_TRUE(pump([&] { return a.calls; }));
  EXPECT_EQ(1u, gcu_loop_pending(loop));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &b.op));
  ASSERT_TRUE(pump([&] { return b.calls; }));
  EXPECT_EQ(0u, gcu_loop_pending(loop));
}

TEST_F(LoopTest, RunReturnsAfterAStopAskedForFromACallback) {
  Rec t;
  t.then = [&](Rec &) { EXPECT_EQ(GCU_LOOP_OK, gcu_loop_stop(loop)); };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, 5));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run(loop));
  EXPECT_EQ(1, t.calls);

  // A stop asked for before run is honoured once, so run returns at once.
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_stop(loop));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run(loop));
}

TEST_F(LoopTest, ARunOrADestroyFromInsideACallbackIsRefused) {
  Rec t;
  GCU_Loop_Result once = GCU_LOOP_OK, run = GCU_LOOP_OK, destroy = GCU_LOOP_OK;
  t.then = [&](Rec &) {
    once = gcu_loop_run_once(loop, 0);
    run = gcu_loop_run(loop);
    destroy = gcu_loop_destroy(loop);
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, 0));
  ASSERT_TRUE(pump([&] { return t.calls; }));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, once);
  EXPECT_EQ(GCU_LOOP_ERR_STATE, run);
  EXPECT_EQ(GCU_LOOP_ERR_STATE, destroy);
}

//
// Sockets through the loop.
//

TEST_F(LoopNet, EchoCompletesEveryOperationOnceOnTheLoopThread) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));

  char in[16] = {0};
  char back[16] = {0};
  Rec read1, write1, read2, write2;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &read1.op, c.server, in, sizeof(in)));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &write1.op, c.client, "ping", 4));
  ASSERT_TRUE(pump([&] { return read1.calls && write1.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, write1.r.status);
  EXPECT_EQ(4u, write1.r.bytes);
  EXPECT_EQ(GCU_LOOP_OK, read1.r.status);
  ASSERT_EQ(4u, read1.r.bytes);
  EXPECT_EQ(0, memcmp(in, "ping", 4));

  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &write2.op, c.server, in, 4));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &read2.op, c.client, back, sizeof(back)));
  ASSERT_TRUE(pump([&] { return read2.calls && write2.calls; }));
  ASSERT_EQ(4u, read2.r.bytes);
  EXPECT_EQ(0, memcmp(back, "ping", 4));

  // Exactly once: nothing more arrives however long the loop runs.
  settle(30);
  for (Rec * r : {&read1, &write1, &read2, &write2}) {
    EXPECT_EQ(1, r->calls);
    EXPECT_EQ(mainThread, r->tid);
  }
}

TEST_F(LoopNet, AcceptReportsThePeerAndConnectSucceeds) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  GCU_Socket_Address clientLocal, serverPeer, serverLocal;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(c.client, &clientLocal));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_peer_address(c.server, &serverPeer));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(c.server, &serverLocal));
  EXPECT_EQ(clientLocal.port, serverPeer.port);
  EXPECT_EQ(0, memcmp(clientLocal.bytes, serverPeer.bytes, 4));
  EXPECT_EQ(c.address.port, serverLocal.port);
}

TEST_F(LoopNet, AnAcceptCompletionCarriesThePeerAddress) {
  Conn c;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&c.listener, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address any;
  gcu_socket_address_loopback(&any, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(c.listener, &any));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(c.listener, 4));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(c.listener, &c.address));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&c.client, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  Rec accepted, connected;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &accepted.op, c.listener));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_connect(loop, &connected.op, c.client, &c.address));
  ASSERT_TRUE(pump([&] { return accepted.calls && connected.calls; }));
  c.server = accepted.r.accepted;
  GCU_Socket_Address clientLocal;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(c.client, &clientLocal));
  EXPECT_EQ(GCU_SOCKET_IPV4, accepted.r.peer.family);
  EXPECT_EQ(clientLocal.port, accepted.r.peer.port);
  EXPECT_EQ(0, memcmp(accepted.r.peer.bytes, clientLocal.bytes, 4));
  EXPECT_EQ(sizeof(GCU_Socket_Address), accepted.r.peer.size);
}

TEST_F(LoopNet, IPv6EchoWorksWhenLoopbackIsAvailable) {
  REQUIRE_IPV6();
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c, GCU_SOCKET_IPV6));
  char in[8] = {0};
  Rec r;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r.op, c.server, in, sizeof(in)));
  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "six", 3));
  ASSERT_TRUE(pump([&] { return r.calls; }));
  EXPECT_EQ(3u, r.r.bytes);
  EXPECT_EQ(0, memcmp(in, "six", 3));
}

TEST_F(LoopNet, AWriteLargerThanTheSocketBufferCompletesAfterAllBytes) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  // Modest buffers on both ends, so that the OS takes only part of a large
  // write at a time and the rest has to wait.  Smaller ones work and make the
  // run slow: the window shrinks to a few KiB and every round trip waits on
  // the peer's delayed acknowledgement.
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(c.client, GCU_SOCKET_OPT_SEND_BUFFER, 32768));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(c.server, GCU_SOCKET_OPT_RECV_BUFFER, 32768));

  const size_t total = 1u << 20;
  vector<uint8_t> payload(total);
  for (size_t i = 0; i < total; ++i) payload[i] = (uint8_t)(i * 31 + (i >> 8));
  vector<uint8_t> received(total, 0);
  size_t got = 0;
  int reads = 0;
  size_t smallest = total;

  Rec write, read;
  read.then = [&](Rec & self) {
    ASSERT_EQ(GCU_LOOP_OK, self.r.status);
    ASSERT_GT(self.r.bytes, 0u);
    got += self.r.bytes;
    ++reads;
    smallest = min(smallest, self.r.bytes);
    if (got < total) {
      ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &self.op,
        c.server, received.data() + got, total - got));
    }
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &write.op, c.client,
    payload.data(), total));
  EXPECT_EQ(0, write.calls);
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &read.op, c.server,
    received.data(), total));
  ASSERT_TRUE(pump([&] { return write.calls && got == total; }, 15000));
  EXPECT_EQ(GCU_LOOP_OK, write.r.status);
  EXPECT_EQ(total, write.r.bytes) << "the write reports all of it";
  EXPECT_GT(reads, 1) << "a read completes with what has arrived, not all";
  EXPECT_EQ(payload, received);
  EXPECT_EQ(1, write.calls);
}

TEST_F(LoopNet, AReadAtEndOfStreamCompletesWithZeroBytes) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  char buf[8];
  Rec r;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r.op, c.server, buf, sizeof(buf)));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_shutdown(c.client, GCU_SOCKET_SHUTDOWN_WRITE));
  ASSERT_TRUE(pump([&] { return r.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, r.r.status);
  EXPECT_EQ(0u, r.r.bytes);
}

TEST_F(LoopNet, ZeroLengthReadsAndWritesCompleteWithZeroBytes) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  Rec r, w;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &w.op, c.client, nullptr, 0));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r.op, c.server, nullptr, 0));
  ASSERT_TRUE(pump([&] { return r.calls && w.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, w.r.status);
  EXPECT_EQ(0u, w.r.bytes);
  EXPECT_EQ(GCU_LOOP_OK, r.r.status);
  EXPECT_EQ(0u, r.r.bytes);
}

TEST_F(LoopNet, TwoReadsOnOneSocketFinishInTheOrderStarted) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  char one[2], two[2];
  vector<int> order;
  Rec r1, r2;
  r1.then = [&](Rec &) { order.push_back(1); };
  r2.then = [&](Rec &) { order.push_back(2); };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r1.op, c.server, one, 2));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r2.op, c.server, two, 2));
  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "abcd", 4));
  ASSERT_TRUE(pump([&] { return order.size() == 2; }));
  EXPECT_EQ((vector<int>{1, 2}), order);
  EXPECT_EQ(0, memcmp(one, "ab", 2));
  EXPECT_EQ(0, memcmp(two, "cd", 2));
}

TEST_F(LoopNet, AcceptWaitsWithoutBlockingTheThread) {
  // Pins that sockets are non-blocking: a blocking accept here would stop the
  // thread until the hang guard fired.
  Conn c;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&c.listener, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address any;
  gcu_socket_address_loopback(&any, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(c.listener, &any));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(c.listener, 4));
  Rec acc;
  auto t0 = steady_clock::now();
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &acc.op, c.listener));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, 0));
  EXPECT_LT(duration_cast<milliseconds>(steady_clock::now() - t0).count(), 1000);
  EXPECT_EQ(0, acc.calls);
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &acc.op));
  ASSERT_TRUE(pump([&] { return acc.calls; }));
  EXPECT_EQ(GCU_LOOP_CANCELLED, acc.r.status);
  EXPECT_EQ(nullptr, acc.r.accepted);
}

TEST_F(LoopNet, ABlockingSocketIsRefusedByTheLoop) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_set_nonblocking(c.server, false));
  char buf[4];
  Rec r;
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_read(loop, &r.op, c.server, buf, 4));
  EXPECT_FALSE(gcu_loop_op_is_pending(&r.op));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_set_nonblocking(c.server, true));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r.op, c.server, buf, 4));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &r.op));
  ASSERT_TRUE(pump([&] { return r.calls; }));
}

TEST_F(LoopNet, TheWrongKindOfSocketIsRefused) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  GCU_Socket * udp = nullptr;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&udp, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, nullptr));
  char buf[4];
  Rec r;
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_read(loop, &r.op, udp, buf, 4));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_accept(loop, &r.op, udp));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_recvfrom(loop, &r.op, c.client, buf, 4));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_sendto(loop, &r.op, c.client, buf, 4,
    &c.address));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_read(loop, &r.op, nullptr, buf, 4));
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_read(loop, &r.op, c.client, nullptr, 4));
  GCU_Socket_Address v6;
  gcu_socket_address_loopback(&v6, GCU_SOCKET_IPV6, 1);
  Rec k;
  EXPECT_EQ(GCU_LOOP_ERR_INVALID, gcu_loop_connect(loop, &k.op, c.client, &v6))
    << "an address of the wrong family";
  gcu_socket_close(udp);
}

TEST_F(LoopNet, ASocketBelongsToTheFirstLoopThatUsesIt) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  GCU_Loop * other = nullptr;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_create(&other, nullptr));
  char buf[4];
  Rec r;
  // The client joined this loop when it connected; the server socket has not
  // been used with any loop yet, so it is the client that is refused.
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_read(other, &r.op, c.client, buf, 4));
  EXPECT_FALSE(gcu_loop_op_is_pending(&r.op));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_destroy(other));
}

TEST_F(LoopNet, ClosingASocketWithAnOperationInFlightIsRefused) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  char buf[4];
  Rec r;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r.op, c.server, buf, 4));
  EXPECT_EQ(GCU_SOCKET_ERR_STATE, gcu_socket_close(c.server));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &r.op));
  EXPECT_EQ(GCU_SOCKET_ERR_STATE, gcu_socket_close(c.server))
    << "the buffer is the loop's until the completion arrives";
  ASSERT_TRUE(pump([&] { return r.calls; }));
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_close(c.server));
  c.server = nullptr;
}

TEST_F(LoopNet, ASocketMayBeClosedFromItsOwnCompletionCallback) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  char buf[4];
  Rec r, again;
  GCU_Socket_Result closed = GCU_SOCKET_ERR_STATE;
  r.then = [&](Rec &) {
    closed = gcu_socket_close(c.server);
    c.server = nullptr;
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &r.op, c.server, buf, 4));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_shutdown(c.client, GCU_SOCKET_SHUTDOWN_WRITE));
  ASSERT_TRUE(pump([&] { return r.calls; }));
  EXPECT_EQ(GCU_SOCKET_OK, closed);
  settle(20);
}

//
// Cancelling.
//

TEST_F(LoopNet, CancelledReadCompletesOnceAndNeverTouchesItsBuffer) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  unsigned char buf[64];
  memset(buf, 0xAB, sizeof(buf));
  Rec rd;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &rd.op, c.server, buf, sizeof(buf)));

  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &rd.op));
  EXPECT_EQ(0, rd.calls) << "the completion never arrives before cancel returns";
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_cancel(loop, &rd.op))
    << "a second cancel finds the result already decided";
  ASSERT_TRUE(pump([&] { return rd.calls; }));
  EXPECT_EQ(GCU_LOOP_CANCELLED, rd.r.status);
  EXPECT_EQ(0u, rd.r.bytes);

  // The peer writes only now.  The buffer is still the caller's and must stay
  // untouched, and the bytes must wait in the socket for a read that wants
  // them.
  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "late-data", 9));
  settle(60);
  for (unsigned char b : buf) {
    ASSERT_EQ(0xAB, b) << "the cancelled read wrote into its buffer";
  }
  EXPECT_EQ(1, rd.calls);

  char fresh[16] = {0};
  Rec again;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &again.op, c.server, fresh, 16));
  ASSERT_TRUE(pump([&] { return again.calls; }));
  ASSERT_EQ(9u, again.r.bytes);
  EXPECT_EQ(0, memcmp(fresh, "late-data", 9));
}

TEST_F(LoopNet, CancelledReadBufferMayBeFreedByItsCompletion) {
  // Under AddressSanitizer a loop that is still reading into the buffer after
  // the completion has freed it is a heap-use-after-free; the planted early
  // release in `make check-loop-defects` is caught here.  In a plain build the
  // sibling test above is what notices.
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  unsigned char * buf = new unsigned char[64];
  memset(buf, 0, 64);
  Rec rd;
  rd.then = [&](Rec &) {
    delete[] buf;
    buf = nullptr;
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &rd.op, c.server, buf, 64));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &rd.op));
  ASSERT_TRUE(pump([&] { return rd.calls; }));
  ASSERT_EQ(nullptr, buf);
  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "after-free", 10));
  settle(60);
  EXPECT_EQ(1, rd.calls);
}

TEST_F(LoopNet, AnOperationWhoseResultIsDecidedKeepsItAgainstACancel) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "x", 1));
  char buf[4];
  Rec rd;
  // The data is already there (loopback delivers at once; the settle is for
  // a slow machine), so the read is decided by the start call and only its
  // callback is waiting.
  settle(20);
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &rd.op, c.server, buf, sizeof(buf)));
  EXPECT_EQ(0, rd.calls);
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_cancel(loop, &rd.op));
  ASSERT_TRUE(pump([&] { return rd.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, rd.r.status) << "the byte was read, so not cancelled";
  EXPECT_EQ(1u, rd.r.bytes);
}

TEST_F(LoopNet, ASocketClosedFromACallbackWhileItsEventIsInTheSameBatchIsSafe) {
  // Two sockets become readable together, so one epoll_wait returns both.
  // The second callback to run closes the other's socket, whose event was in
  // that batch and whose callback has already run.  The close happens while the loop is
  // running, so the memory is held to the end of the iteration (the zombie
  // path); freeing it at once would be a use after free under ASan.
  Conn a, b;
  ASSERT_NO_FATAL_FAILURE(connectPair(a));
  ASSERT_NO_FATAL_FAILURE(connectPair(b));
  char bufA[4], bufB[4];
  Rec readA, readB;
  GCU_Socket_Result closed = GCU_SOCKET_ERR_STATE;
  // Whichever callback runs second closes the other's socket.
  readA.then = [&](Rec &) {
    if (readB.calls) {
      closed = gcu_socket_close(b.server);
      b.server = nullptr;
    }
  };
  readB.then = [&](Rec &) {
    if (readA.calls) {
      closed = gcu_socket_close(a.server);
      a.server = nullptr;
    }
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &readA.op, a.server, bufA, 4));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &readB.op, b.server, bufB, 4));
  Rec wa, wb;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &wa.op, a.client, "a", 1));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &wb.op, b.client, "b", 1));
  ASSERT_TRUE(pump([&] { return readA.calls && readB.calls; }));
  EXPECT_EQ(GCU_SOCKET_OK, closed);
  EXPECT_EQ(1, readA.calls);
  EXPECT_EQ(1, readB.calls);
  settle(30);
}

TEST_F(LoopNet, ASocketUsedWithALoopCannotBeMadeBlocking) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  EXPECT_EQ(GCU_SOCKET_ERR_STATE, gcu_socket_set_nonblocking(c.client, false));
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_set_nonblocking(c.client, true));
  GCU_Socket * fresh = nullptr;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&fresh, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_set_nonblocking(fresh, false))
    << "a socket no loop has seen may be made blocking";
  gcu_socket_close(fresh);
}

TEST_F(LoopNet, ACancelledConnectOrAcceptReleasesItsSocket) {
  Conn c;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&c.listener, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address any;
  gcu_socket_address_loopback(&any, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(c.listener, &any));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(c.listener, 4));
  Rec acc;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &acc.op, c.listener));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &acc.op));
  ASSERT_TRUE(pump([&] { return acc.calls; }));
  EXPECT_EQ(GCU_LOOP_CANCELLED, acc.r.status);
  EXPECT_EQ(GCU_SOCKET_OK, gcu_socket_close(c.listener));
  c.listener = nullptr;
}

//
// Errors.
//

TEST_F(LoopNet, ConnectingToAClosedPortCompletesWithRefused) {
  GCU_Socket * probe = nullptr;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&probe, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address a;
  gcu_socket_address_loopback(&a, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(probe, &a));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(probe, &a));
  gcu_socket_close(probe);   // the port is now closed: nothing listens there

  GCU_Socket * s = nullptr;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  Rec con;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_connect(loop, &con.op, s, &a));
  ASSERT_TRUE(pump([&] { return con.calls; }));
  EXPECT_EQ(GCU_LOOP_ERR_OS, con.r.status);
  EXPECT_EQ(GCU_SOCKET_ERROR_REFUSED, gcu_socket_error_kind(con.r.os_error));
#ifndef _WIN32
  // As in test-socket.cpp: wine lacks some Winsock message texts.
  char message[GCU_ERROR_STRING_MAX];
  EXPECT_EQ(0, gcu_error_string(con.r.os_error, message, sizeof(message)));
  EXPECT_GT(strlen(message), 0u);
#endif
  gcu_socket_close(s);
}

TEST_F(LoopNet, APeerResetIsReportedToAPendingRead) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_set_option(c.server, GCU_SOCKET_OPT_LINGER, 0));
  char buf[8];
  Rec rd;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &rd.op, c.client, buf, sizeof(buf)));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_close(c.server));  // abortive: a reset
  c.server = nullptr;
  ASSERT_TRUE(pump([&] { return rd.calls; }));
  EXPECT_EQ(GCU_LOOP_ERR_OS, rd.r.status);
  EXPECT_EQ(GCU_SOCKET_ERROR_RESET, gcu_socket_error_kind(rd.r.os_error));
}

TEST_F(LoopNet, WritingToAClosedPeerIsAnErrorCompletionNotASignal) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_close(c.server));
  c.server = nullptr;
  vector<char> chunk(64 * 1024, 'x');
  GCU_Loop_Result last = GCU_LOOP_OK;
  int os = 0;
  // The first write may be accepted before the peer's reset comes back; a
  // later one cannot be.  Reaching the error with the process alive is the
  // point (SIGPIPE would end it).
  for (int i = 0; i < 40 && last == GCU_LOOP_OK; ++i) {
    Rec w;
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_write(loop, &w.op, c.client, chunk.data(),
      chunk.size()));
    ASSERT_TRUE(pump([&] { return w.calls; }));
    last = w.r.status;
    os = w.r.os_error;
    if (last != GCU_LOOP_OK) {
      EXPECT_LE(w.r.bytes, chunk.size());
    }
    settle(5);
  }
  ASSERT_EQ(GCU_LOOP_ERR_OS, last);
  GCU_Socket_Error_Kind kind = gcu_socket_error_kind(os);
  EXPECT_TRUE(kind == GCU_SOCKET_ERROR_BROKEN_PIPE
    || kind == GCU_SOCKET_ERROR_RESET || kind == GCU_SOCKET_ERROR_ABORTED)
    << "kind " << kind << ", os error " << os;
}

//
// Datagrams.
//

TEST_F(LoopNet, ADatagramTravelsBetweenTwoSocketsWithItsSender) {
  GCU_Socket * a = nullptr;
  GCU_Socket * b = nullptr;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&a, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, nullptr));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&b, GCU_SOCKET_IPV4, GCU_SOCKET_DATAGRAM, nullptr));
  GCU_Socket_Address addrA, addrB;
  gcu_socket_address_loopback(&addrA, GCU_SOCKET_IPV4, 0);
  gcu_socket_address_loopback(&addrB, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(a, &addrA));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(b, &addrB));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(a, &addrA));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(b, &addrB));

  char buf[32] = {0};
  Rec recv, send;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_recvfrom(loop, &recv.op, a, buf, sizeof(buf)));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_sendto(loop, &send.op, b, "hello", 5, &addrA));
  ASSERT_TRUE(pump([&] { return recv.calls && send.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, send.r.status);
  EXPECT_EQ(5u, send.r.bytes);
  ASSERT_EQ(GCU_LOOP_OK, recv.r.status);
  ASSERT_EQ(5u, recv.r.bytes);
  EXPECT_EQ(0, memcmp(buf, "hello", 5));
  EXPECT_EQ(addrB.port, recv.r.peer.port);
  EXPECT_EQ(GCU_SOCKET_IPV4, recv.r.peer.family);

  // A datagram sent first and received second is decided by the receive's
  // own start; a longer one is cut to the buffer, as on every platform here.
  Rec send2, recv2;
  char small[8] = {0};
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_sendto(loop, &send2.op, b,
    "0123456789abcdef", 16, &addrA));
  ASSERT_TRUE(pump([&] { return send2.calls; }));
  settle(20);
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_recvfrom(loop, &recv2.op, a, small, sizeof(small)));
  ASSERT_TRUE(pump([&] { return recv2.calls; }));
  EXPECT_EQ(GCU_LOOP_OK, recv2.r.status);
  EXPECT_EQ(8u, recv2.r.bytes);
  EXPECT_EQ(0, memcmp(small, "01234567", 8));
  gcu_socket_close(a);
  gcu_socket_close(b);
}

//
// Fibers.
//

struct FiberCase {
  GCU_Loop * loop = nullptr;
  GCU_Socket * socket = nullptr;
  Rec rd;
  char buf[16] = {0};
  thread::id fiberThread;
  GCU_Loop_Result started = GCU_LOOP_RESULT_COUNT;
  GCU_Loop_Result waited = GCU_LOOP_RESULT_COUNT;
  bool done = false;
};

TEST_F(LoopNet, AFiberWaitingOnAReadResumesOnItsOwnThreadWithTheResult) {
  if (!GCU_FIBER_SUPPORTED) GTEST_SKIP() << "no fibers here";
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  FiberCase fc;
  fc.loop = loop;
  fc.socket = c.server;
  GCU_Fiber * fiber = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fiber, [](void * arg) {
    FiberCase * f = static_cast<FiberCase *>(arg);
    f->started = gcu_loop_read(f->loop, &f->rd.op, f->socket, f->buf,
      sizeof(f->buf));
    f->waited = gcu_loop_fiber_wait(&f->rd.op);
    f->fiberThread = this_thread::get_id();
    f->done = true;
  }, &fc, GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));

  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fiber));
  EXPECT_FALSE(fc.done) << "the fiber is parked on its read";
  EXPECT_EQ(GCU_LOOP_OK, fc.started);
  EXPECT_TRUE(gcu_loop_op_is_pending(&fc.rd.op));

  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "fiber", 5));
  ASSERT_TRUE(pump([&] { return fc.done; }));
  EXPECT_EQ(GCU_LOOP_OK, fc.waited);
  EXPECT_EQ(mainThread, fc.fiberThread);
  EXPECT_EQ(GCU_LOOP_OK, fc.rd.op.result.status);
  EXPECT_EQ(5u, fc.rd.op.result.bytes);
  EXPECT_EQ(0, memcmp(fc.buf, "fiber", 5));
  EXPECT_EQ(1, fc.rd.calls) << "a record's callback runs, and then the fiber";
  EXPECT_TRUE(gcu_fiber_is_finished(fiber));
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fiber));
}

TEST_F(LoopTest, WaitingOutsideAFiberOrOnAnIdleRecordIsAnError) {
  Rec t;
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_fiber_wait(&t.op))
    << "not on a fiber";
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &t.op, 5000));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_fiber_wait(&t.op))
    << "still not on a fiber, whatever the record";
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &t.op));
  ASSERT_TRUE(pump([&] { return t.calls; }));

  if (!GCU_FIBER_SUPPORTED) return;
  GCU_Loop_Result idleResult = GCU_LOOP_OK;
  struct Ctx { Rec * rec; GCU_Loop_Result * out; } ctx{&t, &idleResult};
  GCU_Fiber * fiber = nullptr;
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fiber, [](void * arg) {
    Ctx * x = static_cast<Ctx *>(arg);
    *x->out = gcu_loop_fiber_wait(&x->rec->op);
  }, &ctx, GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
  ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fiber));
  EXPECT_EQ(GCU_LOOP_ERR_STATE, idleResult) << "the record is not in flight";
  EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fiber));
}

TEST_F(LoopTest, FibersWaitingOnTimersResumeInDeadlineOrderEachOnItsOwnThread) {
  if (!GCU_FIBER_SUPPORTED) GTEST_SKIP() << "no fibers here";
  struct Waiter {
    GCU_Loop * loop;
    Rec timer;
    unsigned long delay;
    vector<int> * order;
    int id;
    thread::id tid;
  };
  vector<int> order;
  Waiter w[3];
  unsigned long delays[3] = {30, 10, 20};
  GCU_Fiber * fibers[3] = {nullptr, nullptr, nullptr};
  for (int i = 0; i < 3; ++i) {
    w[i].loop = loop;
    w[i].delay = delays[i];
    w[i].order = &order;
    w[i].id = i;
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_create(&fibers[i], [](void * arg) {
      Waiter * x = static_cast<Waiter *>(arg);
      gcu_loop_timer_start(x->loop, &x->timer.op, x->delay);
      gcu_loop_fiber_wait(&x->timer.op);
      x->tid = this_thread::get_id();
      x->order->push_back(x->id);
    }, &w[i], GCU_FIBER_DEFAULT_STACK_SIZE, nullptr));
    ASSERT_EQ(GCU_FIBER_OK, gcu_fiber_switch_to(fibers[i]));
  }
  ASSERT_TRUE(pump([&] { return order.size() == 3; }));
  EXPECT_EQ((vector<int>{1, 2, 0}), order);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(mainThread, w[i].tid);
    EXPECT_TRUE(gcu_fiber_is_finished(fibers[i]));
    EXPECT_EQ(GCU_FIBER_OK, gcu_fiber_destroy(fibers[i]));
  }
}

//
// Another thread.
//

TEST_F(LoopTest, PostFromAnotherThreadWakesAWaitingLoop) {
  // The loop waits with no timeout, so the only thing that can end the wait
  // is the post.  A post that queues and does not wake leaves it there for
  // good, which the hang guard reports under this test's name.
  Rec posted;
  auto t0 = steady_clock::now();
  thread poster([&] {
    this_thread::sleep_for(milliseconds(60));
    EXPECT_EQ(GCU_LOOP_OK, gcu_loop_post(loop, &posted.op));
  });
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, -1));
  poster.join();
  EXPECT_EQ(1, posted.calls) << "run_once returned without running the post";
  EXPECT_EQ(GCU_LOOP_OK, posted.r.status);
  EXPECT_EQ(mainThread, posted.tid) << "the callback runs on the loop thread";
  EXPECT_GE(duration_cast<milliseconds>(steady_clock::now() - t0).count(), 50);
}

TEST_F(LoopTest, ManyThreadsPostAndEveryCallbackRunsInPerThreadOrder) {
  const int threads = 4;
  const int each = 500;
  struct Post {
    GCU_Loop_Op op;
    int thread, seq;
  };
  vector<vector<Post>> posts(threads, vector<Post>(each));
  vector<vector<int>> seen(threads);
  atomic<int> ran{0};
  struct Ctx { vector<vector<int>> * seen; atomic<int> * ran; };
  Ctx ctx{&seen, &ran};
  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < each; ++i) {
      Post & p = posts[t][i];
      gcu_loop_op_init(&p.op, [](GCU_Loop_Op * op, void * user) {
        Post * self = reinterpret_cast<Post *>(op);
        Ctx * c = static_cast<Ctx *>(user);
        (*c->seen)[self->thread].push_back(self->seq);
        ++*c->ran;
      }, &ctx);
      p.thread = t;
      p.seq = i;
    }
  }
  vector<thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      for (int i = 0; i < each; ++i) {
        EXPECT_EQ(GCU_LOOP_OK, gcu_loop_post(loop, &posts[t][i].op));
      }
    });
  }
  ASSERT_TRUE(pump([&] { return ran.load() == threads * each; }, 15000));
  for (auto & th : pool) th.join();
  for (int t = 0; t < threads; ++t) {
    ASSERT_EQ((size_t)each, seen[t].size());
    for (int i = 0; i < each; ++i) {
      ASSERT_EQ(i, seen[t][i]) << "thread " << t << " callbacks reordered";
    }
  }
  EXPECT_EQ(0u, gcu_loop_pending(loop));
}

TEST_F(LoopTest, AWakeWithNothingToDeliverEndsTheWait) {
  thread waker([&] {
    this_thread::sleep_for(milliseconds(40));
    EXPECT_EQ(GCU_LOOP_OK, gcu_loop_wake(loop));
  });
  auto t0 = steady_clock::now();
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_run_once(loop, -1));
  waker.join();
  EXPECT_GE(duration_cast<milliseconds>(steady_clock::now() - t0).count(), 30);
}

TEST_F(LoopTest, EveryOtherCallFromAnotherThreadIsRefusedAndChangesNothing) {
  Rec t, timer;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &timer.op, 5000));
  Conn dummy;
  GCU_Loop_Result r[9];
  size_t pendingSeen = 99;
  thread other([&] {
    Rec x;
    char buf[4];
    r[0] = gcu_loop_run_once(loop, 0);
    r[1] = gcu_loop_run(loop);
    r[2] = gcu_loop_stop(loop);
    r[3] = gcu_loop_timer_start(loop, &x.op, 1);
    r[4] = gcu_loop_cancel(loop, &timer.op);
    r[5] = gcu_loop_destroy(loop);
    GCU_Socket * s = nullptr;
    gcu_socket_create(&s, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr);
    r[6] = gcu_loop_read(loop, &x.op, s, buf, 4);
    gcu_socket_close(s);
    r[7] = gcu_loop_timer_cancel(loop, &timer.op);
    r[8] = gcu_loop_fiber_wait(&timer.op);
    pendingSeen = gcu_loop_pending(loop);
  });
  other.join();
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(GCU_LOOP_ERR_THREAD, r[i]) << "call " << i;
  }
  // Waiting needs a fiber and the thread is not on one: refused either way.
  EXPECT_NE(GCU_LOOP_OK, r[8]);
  EXPECT_EQ(0u, pendingSeen);
  // Nothing changed: the loop and its timer are as they were.
  EXPECT_EQ(1u, gcu_loop_pending(loop));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_cancel(loop, &timer.op));
  ASSERT_TRUE(pump([&] { return timer.calls; }));
  EXPECT_EQ(GCU_LOOP_CANCELLED, timer.r.status);
}

//
// Destroying a loop.
//

TEST_F(LoopNet, DestroyingALoopWithWorkOutstandingCancelsEachThenReturns) {
  Conn c;
  ASSERT_NO_FATAL_FAILURE(connectPair(c));
  Conn listening;
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_create(
    &listening.listener, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address any;
  gcu_socket_address_loopback(&any, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(listening.listener, &any));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(listening.listener, 4));

  unsigned char * b1 = new unsigned char[32];
  unsigned char * b2 = new unsigned char[32];
  Rec read1, read2, accept, timer1, timer2, posted;
  GCU_Loop_Result refused = GCU_LOOP_OK;
  Rec lateStart;
  read1.then = [&](Rec &) { delete[] b1; b1 = nullptr; };
  read2.then = [&](Rec &) { delete[] b2; b2 = nullptr; };
  timer1.then = [&](Rec &) {
    refused = gcu_loop_timer_start(loop, &lateStart.op, 1);
  };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &read1.op, c.server, b1, 32));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &read2.op, c.client, b2, 32));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &accept.op, listening.listener));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &timer1.op, 60000));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(loop, &timer2.op, 60000));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_post(loop, &posted.op));
  EXPECT_EQ(6u, gcu_loop_pending(loop));

  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(loop));
  loop = nullptr;
  for (Rec * r : {&read1, &read2, &accept, &timer1, &timer2, &posted}) {
    EXPECT_EQ(1, r->calls);
    EXPECT_EQ(GCU_LOOP_CANCELLED, r->r.status);
    EXPECT_EQ(mainThread, r->tid);
  }
  EXPECT_EQ(GCU_LOOP_ERR_STATE, refused)
    << "a callback run by destroy may start nothing";
  EXPECT_EQ(nullptr, b1);
  EXPECT_EQ(nullptr, b2);
  EXPECT_EQ(0, lateStart.calls);

  // The sockets outlive the loop, can be closed, and cannot join another.
  GCU_Loop * second = nullptr;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_create(&second, nullptr));
  char buf[4];
  Rec r;
  EXPECT_EQ(GCU_LOOP_ERR_STATE, gcu_loop_read(second, &r.op, c.server, buf, 4));
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_destroy(second));
}

TEST_F(LoopTest, APostRefusedAfterDestroyBeganAndOneNeverRunIsCancelled) {
  // A post that was accepted and never ran is owed a completion; a post that
  // arrives later is refused.  (The second is checked from a callback run by
  // the drain, which is the only place "during destroy" can be observed from
  // the loop's own thread.)
  Rec accepted, late;
  GCU_Loop_Result duringDrain = GCU_LOOP_OK;
  accepted.then = [&](Rec &) { duringDrain = gcu_loop_post(loop, &late.op); };
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_post(loop, &accepted.op));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(loop));
  loop = nullptr;
  EXPECT_EQ(1, accepted.calls);
  EXPECT_EQ(GCU_LOOP_CANCELLED, accepted.r.status);
  EXPECT_EQ(GCU_LOOP_ERR_STATE, duringDrain);
  EXPECT_EQ(0, late.calls);
}

//
// Allocation failure.
//

struct Flaky {
  atomic<int> allocs{0};
  atomic<int> frees{0};
  atomic<int> failAt{-1};      // the Nth allocation fails (0 = the next)
  atomic<bool> failGrowth{false};
  atomic<bool> failAll{false};
};
bool flakyShouldFail(Flaky * f) {
  if (f->failAll.load()) return true;
  int at = f->failAt.load();
  if (at < 0) return false;
  if (f->failAt.fetch_sub(1) == 0) return true;
  return false;
}
void * flakyMalloc(void * ctx, size_t n) {
  Flaky * f = static_cast<Flaky *>(ctx);
  if (flakyShouldFail(f)) return nullptr;
  ++f->allocs;
  return malloc(n);
}
void * flakyCalloc(void * ctx, size_t items, size_t n) {
  Flaky * f = static_cast<Flaky *>(ctx);
  if (flakyShouldFail(f)) return nullptr;
  ++f->allocs;
  return calloc(items, n);
}
void * flakyRealloc(void * ctx, void * p, size_t n) {
  Flaky * f = static_cast<Flaky *>(ctx);
  if (f->failGrowth.load() || flakyShouldFail(f)) return nullptr;
  if (!p) ++f->allocs;
  return realloc(p, n);
}
void flakyFree(void * ctx, void * p) {
  Flaky * f = static_cast<Flaky *>(ctx);
  if (p) ++f->frees;
  free(p);
}
GCU_Allocator flakyAllocator(Flaky * f) {
  return GCU_Allocator{f, flakyMalloc, flakyCalloc, flakyRealloc, flakyFree};
}

TEST_F(LoopTest, CreatingALoopFailsCleanlyAtEachAllocation) {
  Flaky f;
  GCU_Allocator alloc = flakyAllocator(&f);
  for (int n = 0; n < 4; ++n) {
    f.failAt = n;
    GCU_Loop * made = reinterpret_cast<GCU_Loop *>(0x1);
    GCU_Loop_Result r = gcu_loop_create(&made, &alloc);
    if (r == GCU_LOOP_OK) {
      ASSERT_NE(reinterpret_cast<GCU_Loop *>(0x1), made);
      ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(made));
    }
    else {
      EXPECT_EQ(GCU_LOOP_ERR_OOM, r) << "allocation " << n;
      EXPECT_EQ(reinterpret_cast<GCU_Loop *>(0x1), made) << "written on failure";
    }
    EXPECT_EQ(f.allocs.load(), f.frees.load()) << "allocation " << n;
  }
  EXPECT_GT(f.allocs.load(), 0);
}

TEST_F(LoopTest, ATimerThatCannotBeQueuedIsAnErrorAndLeavesTheRecordIdle) {
  Flaky f;
  GCU_Allocator alloc = flakyAllocator(&f);
  GCU_Loop * mine = nullptr;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_create(&mine, &alloc));
  deque<Rec> timers(10);
  for (int i = 0; i < 8; ++i) {
    ASSERT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(mine, &timers[i].op, 60000));
  }
  f.failGrowth = true;   // the ninth makes the queue grow
  EXPECT_EQ(GCU_LOOP_ERR_OOM, gcu_loop_timer_start(mine, &timers[8].op, 60000));
  EXPECT_FALSE(gcu_loop_op_is_pending(&timers[8].op));
  EXPECT_EQ(8u, gcu_loop_pending(mine));
  f.failGrowth = false;
  EXPECT_EQ(GCU_LOOP_OK, gcu_loop_timer_start(mine, &timers[8].op, 60000));
  EXPECT_EQ(9u, gcu_loop_pending(mine));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(mine));
  for (int i = 0; i < 9; ++i) {
    EXPECT_EQ(1, timers[i].calls);
    EXPECT_EQ(GCU_LOOP_CANCELLED, timers[i].r.status);
  }
  EXPECT_EQ(f.allocs.load(), f.frees.load());
}

TEST_F(LoopNet, AnAcceptThatCannotAllocateItsSocketLeavesTheConnectionWaiting) {
  Flaky f;
  GCU_Allocator alloc = flakyAllocator(&f);
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(loop));
  loop = nullptr;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_create(&loop, &alloc));

  Conn c;
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&c.listener, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));
  GCU_Socket_Address any;
  gcu_socket_address_loopback(&any, GCU_SOCKET_IPV4, 0);
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_bind(c.listener, &any));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_listen(c.listener, 4));
  ASSERT_EQ(GCU_SOCKET_OK, gcu_socket_local_address(c.listener, &c.address));
  ASSERT_EQ(GCU_SOCKET_OK,
    gcu_socket_create(&c.client, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, nullptr));

  Rec accept, connect;
  f.failAll = true;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &accept.op, c.listener));
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_connect(loop, &connect.op, c.client, &c.address));
  ASSERT_TRUE(pump([&] { return accept.calls && connect.calls; }));
  f.failAll = false;
  EXPECT_EQ(GCU_LOOP_ERR_OOM, accept.r.status);
  EXPECT_EQ(nullptr, accept.r.accepted);

  // The connection was not taken: it is still in the listener's backlog, and
  // the next accept, with memory, gets it.
  Rec second;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_accept(loop, &second.op, c.listener));
  ASSERT_TRUE(pump([&] { return second.calls; }));
  ASSERT_EQ(GCU_LOOP_OK, second.r.status);
  ASSERT_NE(nullptr, second.r.accepted);
  c.server = second.r.accepted;
  char buf[4];
  Rec rd;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_read(loop, &rd.op, c.server, buf, 4));
  ASSERT_NO_FATAL_FAILURE(writeAll(c.client, "ok", 2));
  ASSERT_TRUE(pump([&] { return rd.calls; }));
  EXPECT_EQ(2u, rd.r.bytes);

  // The loop was built with an allocator that lives in this frame, so it
  // goes before the frame does, not in the fixture's teardown.
  gcu_socket_close(c.client);
  c.client = nullptr;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(loop));
  loop = nullptr;
}

#if !defined(_WIN32) && !defined(GCU_TEST_SANITIZED)
uint64_t addressSpaceBytes() {
  FILE * file = fopen("/proc/self/statm", "r");
  if (file == nullptr) return 0;
  unsigned long pages = 0;
  int got = fscanf(file, "%lu", &pages);
  fclose(file);
  return got == 1 ? (uint64_t)pages * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
}

const char * const kAddressSpaceChild = "GCU_TEST_LOOP_ADDRESS_SPACE_CHILD";

/**
 * The body of the address-space test, run in a fresh copy of this program.
 * A fresh process, because a limit is permanent and because what a growing
 * queue meets depends on what the allocator has free: a forked copy of a
 * process that has run fifty other tests has stretches of free heap a queue
 * grows into without ever asking the OS, and the limit is then never
 * reached.  Started from nothing, the queue's growth is what pays.
 */
void addressSpaceBody() {
  Flaky counts;
  GCU_Allocator alloc = flakyAllocator(&counts);
  const int n = 200000;
  Rec * records = new Rec[n];   // before the limit, so that only the queue grows
  GCU_Loop * mine = nullptr;
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_create(&mine, &alloc));
  uint64_t used = addressSpaceBytes();
  ASSERT_NE(0u, used) << "no /proc/self/statm";
  struct rlimit lim;
  lim.rlim_cur = lim.rlim_max = (rlim_t)(used + (64u << 10));
  ASSERT_EQ(0, setrlimit(RLIMIT_AS, &lim));

  int started = 0;
  GCU_Loop_Result last = GCU_LOOP_OK;
  for (int i = 0; i < n; ++i) {
    last = gcu_loop_timer_start(mine, &records[i].op, 60000);
    if (last != GCU_LOOP_OK) break;
    ++started;
  }
  EXPECT_EQ(GCU_LOOP_ERR_OOM, last) << "started " << started;
  EXPECT_GT(started, 0);
  EXPECT_LT(started, n);
  EXPECT_EQ((size_t)started, gcu_loop_pending(mine));
  EXPECT_FALSE(gcu_loop_op_is_pending(&records[started].op));

  // Lift the limit to tear down: the point is the start that failed.
  lim.rlim_cur = lim.rlim_max = RLIM_INFINITY;
  setrlimit(RLIMIT_AS, &lim);
  ASSERT_EQ(GCU_LOOP_OK, gcu_loop_destroy(mine));
  for (int i = 0; i < started; ++i) {
    ASSERT_EQ(1, records[i].calls);
  }
  EXPECT_EQ(counts.allocs.load(), counts.frees.load()) << "leaked";
  delete[] records;
}

TEST_F(LoopTest, RunningOutOfAddressSpaceIsAnErrorNotACrashOrALeak) {
  // Not under a sanitizer, whose own mappings make the limit meaningless; the
  // allocator tests above cover those builds.
#ifdef GCU_TEST_HAVE_VALGRIND_H
  if (RUNNING_ON_VALGRIND) {
    GTEST_SKIP() << "an address-space limit means nothing under Valgrind, and "
                    "/proc/self/exe is Valgrind itself";
  }
#endif
  if (getenv(kAddressSpaceChild)) {
#ifdef __GLIBC__
    // A block over this size is mapped rather than carved from the heap.
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
#endif
    addressSpaceBody();
    return;
  }
  fflush(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    setenv(kAddressSpaceChild, "1", 1);
    execl("/proc/self/exe", "test-loop",
      "--gtest_filter=LoopTest.RunningOutOfAddressSpaceIsAnErrorNotACrashOrALeak",
      "--gtest_brief=1", static_cast<char *>(nullptr));
    _exit(127);
  }
  int status = 0;
  ASSERT_GT(pid, 0);
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status)) << "status " << status;
  EXPECT_EQ(0, WEXITSTATUS(status)) << "see the child's output above"
    << " (127: /proc/self/exe could not be run)";
}
#endif

} // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
