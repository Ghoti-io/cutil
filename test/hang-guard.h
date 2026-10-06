/**
 * @file
 * A per-test watchdog that says which test hung.
 *
 * For suites whose failures are hangs rather than wrong answers -- anything
 * that blocks on another thread or another process.  A plain `alarm()` turns
 * a hang into a dead binary, which is already much better than a suite that
 * stops forever and gets blamed on the machine.  But `make test` runs with
 * `--gtest_brief=1`, so a binary killed by SIGALRM prints nothing at all:
 * the run fails and says nothing about where.
 *
 * So the alarm is caught instead, and the handler names the test.  The name
 * is captured in SetUp and written with `write()`, both async-signal-safe;
 * asking gtest for it from inside the handler would not be.
 *
 * Not a general timeout: the alarm has to be generous enough that a slow
 * machine never trips it, so it is a diagnosis of a hang, not a measurement
 * of duration.  Bound the operation itself where the API can.
 */

#ifndef GHOTI_IO_GCU_TEST_HANG_GUARD_H
#define GHOTI_IO_GCU_TEST_HANG_GUARD_H

#include <csignal>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <io.h>
#include <cstdlib>
#endif

namespace ghoti_test {

inline char * hangGuardName() {
  static char name[256] = "";
  return name;
}

inline size_t & hangGuardLength() {
  static size_t length = 0;
  return length;
}

#ifndef _WIN32
extern "C" inline void hangGuardFired(int) {
  static const char prefix[] = "\n*** TIMED OUT, no answer from: ";
  static const char suffix[] = " ***\n";
  ssize_t written = write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
  written = write(STDERR_FILENO, hangGuardName(), hangGuardLength());
  written = write(STDERR_FILENO, suffix, sizeof(suffix) - 1);
  (void)written;
  // _exit, not abort: a non-zero status the Makefile reports, without a core
  // dump for something that is not a crash.
  _exit(1);
}
#else
// Windows has no SIGALRM, so a timer-queue timer plays its part: its callback
// runs on a pool thread, names the test, and ends the process.  Before this
// the guard was empty here, which meant a hang under wine or on Windows was a
// hang -- and the loop's lost-wakeup test exists to turn exactly that into a
// failure.
inline HANDLE & hangGuardTimer() {
  static HANDLE timer = NULL;
  return timer;
}

inline VOID CALLBACK hangGuardFired(PVOID, BOOLEAN) {
  static const char prefix[] = "\n*** TIMED OUT, no answer from: ";
  static const char suffix[] = " ***\n";
  _write(2, prefix, (unsigned)(sizeof(prefix) - 1));
  _write(2, hangGuardName(), (unsigned)hangGuardLength());
  _write(2, suffix, (unsigned)(sizeof(suffix) - 1));
  _exit(1);
}
#endif

/**
 * Derive a fixture from this to have every one of its tests watched.
 *
 * @tparam Seconds How long to allow.  Pick a large multiple of the test's
 *   real duration; this is a deadlock detector, not a stopwatch.
 */
template <unsigned int Seconds>
class HangGuarded : public ::testing::Test {
protected:
  void SetUp() override {
    const ::testing::TestInfo * running =
        ::testing::UnitTest::GetInstance()->current_test_info();
    snprintf(hangGuardName(), 256, "%s.%s",
        running ? running->test_suite_name() : "?",
        running ? running->name() : "?");
    hangGuardLength() = strlen(hangGuardName());
#ifndef _WIN32
    signal(SIGALRM, hangGuardFired);
    alarm(Seconds);
#else
    CreateTimerQueueTimer(&hangGuardTimer(), NULL, hangGuardFired, NULL,
        Seconds * 1000, 0, WT_EXECUTEONLYONCE);
#endif
  }
  void TearDown() override {
#ifndef _WIN32
    alarm(0);
    signal(SIGALRM, SIG_DFL);
#else
    if (hangGuardTimer()) {
      DeleteTimerQueueTimer(NULL, hangGuardTimer(), INVALID_HANDLE_VALUE);
      hangGuardTimer() = NULL;
    }
#endif
  }
};

} // namespace ghoti_test

#endif // GHOTI_IO_GCU_TEST_HANG_GUARD_H
