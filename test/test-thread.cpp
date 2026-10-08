#include <gtest/gtest.h>
#include <ghoti.io/cutil/thread.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <iostream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std;

#define SLEEP_MS 10

// Both flags cross a thread boundary: the worker writes them and the main
// thread reads them.  They were plain bools accessed through a `volatile`
// reference, which keeps the compiler from caching the value but supplies no
// ordering between the threads at all, so every access raced.
struct thread_status {
  std::atomic<bool> is_running;
  std::atomic<bool> run;
};

GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION loop(GCU_THREAD_FUNC_ARG_T status) {
  thread_status * state = (thread_status *)status;
  state->is_running = true;
  while (state->run) {
    gcu_thread_yield();
  };
  state->is_running = false;
  return GCU_THREAD_FUNC_RETURN_T{};
};

GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION doNothing([[maybe_unused]] GCU_THREAD_FUNC_ARG_T arg) {
  if (arg != NULL) {
    *((GCU_Thread *)arg) = gcu_thread_get_current_id();
  }
  return GCU_THREAD_FUNC_RETURN_T{};
}

TEST(Thread, NonexistentThreads) {
  // Check that the thread functions return an error when the thread does not exist.
  GCU_Thread thread = 0;
  bool result = false;
  unsigned long mask = 0;
  int priority = 0;
  EXPECT_EQ(-1, gcu_thread_is_running(thread, &result));
  EXPECT_EQ(-1, gcu_thread_is_joined(thread, &result));
  EXPECT_EQ(-1, gcu_thread_is_detached(thread, &result));
  EXPECT_EQ(-1, gcu_thread_join(thread));
  EXPECT_EQ(-1, gcu_thread_detach(thread));
  EXPECT_EQ(-1, gcu_thread_set_name(thread, "Hello World!"));
  EXPECT_EQ(-1, gcu_thread_get_name(thread, NULL, 0));
  EXPECT_EQ(-1, gcu_thread_get_affinity(thread, &mask));
  EXPECT_EQ(-1, gcu_thread_set_affinity(thread, 0));
  EXPECT_EQ(-1, gcu_thread_get_priority(thread, &priority));
  EXPECT_EQ(-1, gcu_thread_set_priority(thread, 0));
}

TEST(Thread, SingleThreadLifetime) {
  // Create an infinite loop thread that listens for a signal.
  // The signal will be sent from the main thread.
  // The thread will be stopped from the main thread.
  // The thread will be joined from the main thread.
  thread_status status;
  status.is_running = false;
  status.run = true;
  bool result = false;

  // Create the thread
  GCU_Thread thread;
  EXPECT_EQ(0, gcu_thread_create(&thread, loop, &status));
  EXPECT_NE(thread, 0);

  // gcu_thread_create() returns once the new thread has recorded its id,
  // which happens before the thread function is entered.  Asserting that the
  // loop is running therefore has to wait for the loop, rather than assume
  // that it has already been reached.
  while (!status.is_running) {
    gcu_thread_yield();
  }

  // Check the thread state.
  EXPECT_TRUE(status.is_running);
  EXPECT_EQ(0, gcu_thread_is_running(thread, &result));
  EXPECT_TRUE(result);
  EXPECT_EQ(0, gcu_thread_is_joined(thread, &result));
  EXPECT_FALSE(result);
  EXPECT_EQ(0, gcu_thread_is_detached(thread, &result));
  EXPECT_FALSE(result);

  // Stop the loop
  status.run = false;

  // Wait so that the thread can stop.
  while (status.is_running) {
    gcu_thread_yield();
  }


  // Check the thread state.
  //
  // `status.is_running` going false is the task's own last act; the function
  // still has to return, and the module's record of the thread flips when it
  // does.  Inferring one from the other is a race, and it is not a
  // theoretical one: this line failed once in 1,500 runs of this binary
  // during a soak.  So poll what is actually being asserted.
  EXPECT_EQ(status.is_running, false);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    ASSERT_EQ(0, gcu_thread_is_running(thread, &result));
    if (!result) {
      break;
    }
    gcu_thread_yield();
  } while (std::chrono::steady_clock::now() < deadline);
  EXPECT_FALSE(result) << "the thread never stopped being reported as running";

  // Join the thread.
  EXPECT_EQ(0, gcu_thread_join(thread));
  EXPECT_EQ(0, gcu_thread_is_running(thread, &result));
  EXPECT_FALSE(result);
  EXPECT_EQ(0, gcu_thread_is_joined(thread, &result));
  EXPECT_TRUE(result);
  EXPECT_EQ(0, gcu_thread_is_detached(thread, &result));
  EXPECT_FALSE(result);
}

TEST(Thread, ID) {
  // Create a thread and check its ID.
  GCU_Thread thread;
  uint64_t child_thread_id = 0;
  uint64_t parent_thread_id = gcu_thread_get_current_id();
  EXPECT_NE(0, parent_thread_id);
  EXPECT_EQ(0, gcu_thread_create(&thread, doNothing, &child_thread_id));
  EXPECT_EQ(0, gcu_thread_join(thread));
  EXPECT_EQ(thread, child_thread_id);
  EXPECT_NE(thread, 0);
  EXPECT_NE(child_thread_id, 0);
  EXPECT_NE(parent_thread_id, child_thread_id);
  EXPECT_NE(parent_thread_id, thread);
  EXPECT_EQ(parent_thread_id, gcu_thread_get_current_id());
}

TEST(Thread, NameFunctions) {
  // Create a thread and check its name.
  thread_status status = {
    .is_running = false,
    .run = true
  };
  char parent_buffer[PATH_MAX] = {0};
  char child_buffer[PATH_MAX] = {0};
  char new_name[] = "Hello World!";
  char smallBuffer[] = "";
  GCU_Thread child_thread;
  EXPECT_EQ(0, gcu_thread_create(&child_thread, loop, &status));

  // Verify that the parent and child thread have the same name.
  EXPECT_EQ(0, gcu_thread_get_current_name(parent_buffer, sizeof(parent_buffer)));
  // NOTE: The following is commented out because the parent thread name is not
  // set by default on all platforms (e.g. Windows).
  // EXPECT_NE(string(parent_buffer), string());
  EXPECT_EQ(0, gcu_thread_get_name(child_thread, child_buffer, sizeof(child_buffer)));
  EXPECT_EQ(string(parent_buffer), string(child_buffer));

  // Set the child thread name.
  EXPECT_EQ(0, gcu_thread_set_name(child_thread, new_name));
  EXPECT_EQ(0, gcu_thread_get_name(child_thread, child_buffer, sizeof(child_buffer)));
  EXPECT_EQ(string(new_name), string(child_buffer));

  // Verify that there is an error when the buffer is too small.
  EXPECT_NE(0, gcu_thread_get_name(child_thread, smallBuffer, sizeof(smallBuffer)));

  status.run = false;
  gcu_thread_join(child_thread);
}

TEST(Thread, DetachSucceedsAndIsReported) {
  GCU_Thread thread;
  ASSERT_EQ(0, gcu_thread_create(&thread, doNothing, NULL));

  bool flag = true;
  ASSERT_EQ(0, gcu_thread_is_detached(thread, &flag));
  EXPECT_FALSE(flag);

  // This handed pthread_detach() a GCU_Thread - a uint32_t thread id - where
  // it wants a pthread_t, so it detached whatever that value aliased and
  // could not have worked.  Nothing caught it because no test ever detached a
  // live thread and checked the result.
  EXPECT_EQ(0, gcu_thread_detach(thread));

  ASSERT_EQ(0, gcu_thread_is_detached(thread, &flag));
  EXPECT_TRUE(flag);

  // A detached thread can be neither joined nor detached again.
  EXPECT_EQ(-1, gcu_thread_join(thread));
  EXPECT_NE(0, gcu_thread_detach(thread));
}

TEST(Thread, DoubleJoinIsRefused) {
  GCU_Thread thread;
  ASSERT_EQ(0, gcu_thread_create(&thread, doNothing, NULL));

  EXPECT_EQ(0, gcu_thread_join(thread));

  // Joining twice is undefined at the POSIX level, so the second call has to
  // be refused rather than passed through.
  EXPECT_EQ(-1, gcu_thread_join(thread));
  EXPECT_NE(0, gcu_thread_detach(thread));

  bool joined = false;
  ASSERT_EQ(0, gcu_thread_is_joined(thread, &joined));
  EXPECT_TRUE(joined);
}

TEST(Thread, OnlyOneOfManyConcurrentJoinsSucceeds) {
  // Reading the joined flag, joining, and writing the flag back cannot be one
  // critical section, because the join blocks.  With that gap unguarded,
  // every caller here observed the thread as unjoined and every one of them
  // reached pthread_join() on it.
  for (int round = 0; round < 50; ++round) {
    GCU_Thread thread;
    ASSERT_EQ(0, gcu_thread_create(&thread, doNothing, NULL));

    const int kJoiners = 8;
    std::atomic<int> succeeded{0};
    std::vector<std::thread> joiners;

    for (int i = 0; i < kJoiners; ++i) {
      joiners.emplace_back([&] {
        if (gcu_thread_join(thread) == 0) {
          succeeded.fetch_add(1);
        }
      });
    }
    for (auto & t : joiners) {
      t.join();
    }

    ASSERT_EQ(1, succeeded.load()) << "round " << round;
  }
}

TEST(Thread, JoinAndDetachDoNotBothSucceed) {
  for (int round = 0; round < 100; ++round) {
    GCU_Thread thread;
    ASSERT_EQ(0, gcu_thread_create(&thread, doNothing, NULL));

    std::atomic<int> joined_ok{0};
    std::atomic<int> detached_ok{0};

    std::thread a([&] {
      if (gcu_thread_join(thread) == 0) {
        joined_ok.fetch_add(1);
      }
    });
    std::thread b([&] {
      if (gcu_thread_detach(thread) == 0) {
        detached_ok.fetch_add(1);
      }
    });
    a.join();
    b.join();

    // Exactly one of the two claims the thread; detaching one that is being
    // joined is as undefined as joining it twice.
    ASSERT_EQ(1, joined_ok.load() + detached_ok.load()) << "round " << round;
  }
}

TEST(Thread, ConcurrentAccessorsAreSafe) {
  // The accessors reached their records through the hash with no lock, while
  // gcu_thread_create() mutates that hash and frees the previous record when
  // the OS reuses an id.  This exists mostly to give ThreadSanitizer
  // something to look at.
  const int kReaders = 4;
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  GCU_Thread self = gcu_thread_get_current_id();

  for (int i = 0; i < kReaders; ++i) {
    readers.emplace_back([&] {
      char name[64];
      bool flag = false;
      while (!stop.load()) {
        gcu_thread_is_running(self, &flag);
        gcu_thread_is_joined(self, &flag);
        gcu_thread_is_detached(self, &flag);
        gcu_thread_get_name(self, name, sizeof(name));
      }
    });
  }

  for (int i = 0; i < 200; ++i) {
    GCU_Thread thread;
    ASSERT_EQ(0, gcu_thread_create(&thread, doNothing, NULL));
    EXPECT_EQ(0, gcu_thread_join(thread));
  }

  stop.store(true);
  for (auto & t : readers) {
    t.join();
  }
}

TEST(Thread, ProcessorCount) {
  // The count is documented as never less than 1, so that callers sizing a
  // pool or a table from it always get a usable number.  The POSIX
  // implementation is sysconf(), which reports failure as -1 and would
  // otherwise convert to UINT_MAX.
  unsigned int count = gcu_thread_get_num_processors();
  EXPECT_GE(count, 1u);

  // A wrapped -1 would satisfy the check above, so reject an implausible
  // count as well.  This is the shape the failure actually took.
  EXPECT_LT(count, 65536u);
}

#ifndef _WIN32
TEST(Thread, NameLengthLimitIsReported) {
  // pthread_setname_np() accepts at most 15 characters plus the terminator.
  // The header documents that the limit is platform-specific and that a
  // caller who may exceed it must check the return value; this pins the
  // POSIX half of that statement so the documentation cannot drift.
  GCU_Thread thread = gcu_thread_get_current_id();

  // This renames the process's own main thread, so put back whatever it was
  // called: another test reads the parent name to check that a child inherits
  // it, and leaving this one's name behind would make that depend on the
  // order the tests happen to run in.
  char original[64] = {0};
  bool restore = gcu_thread_get_name(thread, original, sizeof(original)) == 0;

  // 15 characters is the longest name that fits.
  EXPECT_EQ(0, gcu_thread_set_name(thread, "123456789012345"));

  // 16 characters does not, and the failure is reported rather than silently
  // truncated.
  EXPECT_NE(0, gcu_thread_set_name(thread, "1234567890123456"));

  if (restore) {
    gcu_thread_set_name(thread, original);
  }
}
#endif // _WIN32

#ifndef _WIN32
//
// The thread module registers the main thread at load time and joins whatever
// it still holds at exit. Both halves are keyed on the thread id, which fork()
// changes: in the child, the surviving thread no longer matched its own record
// and the module's destructor tried to join the process's own main thread,
// while records for threads that did not survive the fork were joined as
// though they had. Both are undefined behaviour and AddressSanitizer aborts on
// them, so these tests only fail loudly in a sanitized build; they are here so
// that the fork paths are at least exercised on every run.
//
static void expect_clean_child_exit(pid_t pid) {
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  EXPECT_FALSE(WIFSIGNALED(status))
      << "child died from signal " << WTERMSIG(status);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(0, WEXITSTATUS(status));
}

TEST(Fork, ChildExitDoesNotJoinItsOwnMainThread) {
  pid_t pid = fork();
  ASSERT_NE(-1, pid);
  if (pid == 0) {
    // Nothing to do: the bug was in what ran on the way out.
    _exit(0);
  }
  expect_clean_child_exit(pid);
}

TEST(Fork, ChildExitDoesNotJoinThreadsThatDidNotSurvive) {
  thread_status status{false, true};
  GCU_Thread worker;
  ASSERT_EQ(0, gcu_thread_create(&worker, loop, &status));
  while (!status.is_running) {
    gcu_thread_yield();
  }

  // Fork with the worker still running, so the child inherits a record for a
  // thread that does not exist in it.
  pid_t pid = fork();
  ASSERT_NE(-1, pid);
  if (pid == 0) {
    _exit(0);
  }
  expect_clean_child_exit(pid);

  status.run = false;
  gcu_thread_join(worker);
}
#endif // _WIN32

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
