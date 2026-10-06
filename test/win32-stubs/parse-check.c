/*
 * Parse-checks the `#ifdef _WIN32` branches of this library's headers on a
 * non-Windows compiler.  Every macro is *used*, not merely defined: an unused
 * macro is uninstantiated text and proves nothing.  See win32-stubs/windows.h.
 */

#define _WIN32 1

#include <string.h>

#include <ghoti.io/cutil/mutex.h>
#include <ghoti.io/cutil/cond.h>
#include <ghoti.io/cutil/once.h>
#include <ghoti.io/cutil/rwlock.h>
#include <ghoti.io/cutil/error.h>
#include <ghoti.io/cutil/tls.h>
#include <ghoti.io/cutil/env.h>
#include <ghoti.io/cutil/library.h>
#include <ghoti.io/cutil/filelock.h>
#include <ghoti.io/cutil/mmap.h>
#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/cutil/subprocess.h>
#include <ghoti.io/cutil/barrier.h>
#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/cutil/socket.h>
#include <ghoti.io/cutil/loop.h>

static void loop_callback(GCU_Loop_Op * op, void * user_data) {
  (void)op;
  (void)user_data;
}

static void fiber_entry(void * arg) {
  (void)arg;
  gcu_fiber_yield();
}

int main(void) {
  GCU_MUTEX_T m;
  int rc = 0;
  rc |= GCU_MUTEX_CREATE(m);
  rc |= GCU_MUTEX_LOCK(m);
  rc |= GCU_MUTEX_UNLOCK(m);
  rc |= GCU_MUTEX_TRYLOCK(m);
  rc |= GCU_MUTEX_UNLOCK(m);
  rc |= GCU_MUTEX_DESTROY(m);

  // cond.h's Windows branch: the typedef and the declarations.  The bodies
  // live in src/cond.c, which the check-win32-parse rule compiles separately
  // with -fsyntax-only against these same stubs.
  GCU_Cond cv;
  rc |= gcu_cond_create(&cv);
  rc |= gcu_cond_signal(&cv);
  rc |= gcu_cond_broadcast(&cv);
  rc |= gcu_cond_wait(&cv, &m);
  rc |= gcu_cond_timedwait(&cv, &m, 10);
  rc |= gcu_cond_destroy(&cv);

  static GCU_Once once = GCU_ONCE_INIT;
  rc |= gcu_once(&once, 0);

  GCU_RWLock rw;
  rc |= gcu_rwlock_create(&rw);
  rc |= gcu_rwlock_read_lock(&rw);
  rc |= gcu_rwlock_read_trylock(&rw);
  rc |= gcu_rwlock_read_unlock(&rw);
  rc |= gcu_rwlock_write_lock(&rw);
  rc |= gcu_rwlock_write_trylock(&rw);
  rc |= gcu_rwlock_write_unlock(&rw);
  rc |= gcu_rwlock_destroy(&rw);

  char errbuf[GCU_ERROR_STRING_MAX];
  rc |= gcu_error_last();
  rc |= gcu_error_string(2, errbuf, sizeof(errbuf));
  rc |= gcu_error_string_last(errbuf, sizeof(errbuf));

  GCU_TLS tls;
  rc |= gcu_tls_create(&tls, 0);
  rc |= gcu_tls_set(tls, 0);
  rc |= gcu_tls_get(tls) ? 1 : 0;
  rc |= gcu_tls_destroy(&tls);

  char envbuf[64];
  rc |= (int)gcu_env_get("PATH", envbuf, sizeof(envbuf));
  rc |= gcu_env_has("PATH") ? 1 : 0;
  rc |= gcu_env_set("GHOTI_X", "1");
  rc |= gcu_env_unset("GHOTI_X");

  GCU_Library dyn;
  char libmsg[128];
  rc |= gcu_library_open(&dyn, "x");
  rc |= gcu_library_symbol(dyn, "y") ? 1 : 0;
  rc |= gcu_library_error(libmsg, sizeof(libmsg));
  rc |= gcu_library_close(&dyn);

  GCU_File_Lock flock_handle;
  rc |= gcu_file_lock(&flock_handle, "x", 1, 0);
  rc |= gcu_file_unlock(&flock_handle);

  GCU_Mapped_File mapped;
  rc |= gcu_mmap_open(&mapped, "x", 0);
  rc |= gcu_mmap_sync(&mapped);
  rc |= gcu_mmap_close(&mapped);

  GCU_Atomic_Int counter;
  gcu_atomic_int_init(&counter, 0);
  rc |= gcu_atomic_int_fetch_add(&counter, 1);
  rc |= gcu_atomic_int_load(&counter);

  static const char * const spawn_argv[] = {"x", NULL};
  GCU_Subprocess_Options spawn;
  memset(&spawn, 0, sizeof(spawn));
  spawn.argv = spawn_argv;
  spawn.timeout = 10;
  GCU_Subprocess_Result spawned;
  rc |= gcu_subprocess_run(&spawn, &spawned);
  rc |= (int)spawned.outcome;
  rc |= GCU_SUBPROCESS_NOT_STARTED;
  gcu_subprocess_result_free(&spawned);

  GCU_Barrier gate;
  rc |= gcu_barrier_create(&gate, 1);
  rc |= gcu_barrier_wait(&gate);
  rc |= gcu_barrier_destroy(&gate);
  rc |= GCU_BARRIER_SERIAL;

  // fiber.h's Windows branch is only the support macro and the shared
  // declarations; the Fiber API calls are in src/fiber.c, which the
  // check-win32-parse rule compiles separately against these stubs.
  GCU_Fiber * fiber = 0;
  rc |= GCU_FIBER_SUPPORTED ? 0 : 1;
  rc |= (int)gcu_fiber_create(&fiber, fiber_entry, 0,
    GCU_FIBER_DEFAULT_STACK_SIZE, 0);
  rc |= (int)gcu_fiber_switch_to(fiber);
  rc |= (int)gcu_fiber_yield();
  rc |= gcu_fiber_current() ? 1 : 0;
  rc |= gcu_fiber_is_finished(fiber) ? 1 : 0;
  rc |= (int)gcu_fiber_destroy(fiber);
  rc |= gcu_fiber_result_string(GCU_FIBER_ERR_THREAD) ? 0 : 1;

  // socket.h and loop.h's Windows-facing parts are the support macro, the
  // plain-data layouts and the declarations; the Winsock and completion-port
  // calls are in src/socket.c and src/loop.c, which the check-win32-parse
  // rule compiles separately against these stubs.
  GCU_Socket_Address address;
  static const uint8_t four[4] = {127, 0, 0, 1};
  rc |= (int)gcu_socket_address_ipv4(&address, four, 80);
  rc |= (int)gcu_socket_address_parse(&address, "::1", 1);
  char text[GCU_SOCKET_ADDRESS_STRING_MAX];
  rc |= (int)gcu_socket_address_format(&address, text, sizeof(text));
  GCU_Socket * sock = 0;
  rc |= (int)gcu_socket_create(&sock, GCU_SOCKET_IPV4, GCU_SOCKET_STREAM, 0);
  rc |= (int)gcu_socket_bind(sock, &address);
  rc |= (int)gcu_socket_set_option(sock, GCU_SOCKET_OPT_NO_DELAY, 1);
  rc |= (int)gcu_socket_error_kind(10061);
  rc |= GCU_LOOP_SUPPORTED ? 0 : 1;
  GCU_Loop * event_loop = 0;
  GCU_Loop_Op loop_op;
  gcu_loop_op_init(&loop_op, loop_callback, 0);
  char loop_buffer[8];
  rc |= (int)gcu_loop_create(&event_loop, 0);
  rc |= (int)gcu_loop_read(event_loop, &loop_op, sock, loop_buffer, sizeof(loop_buffer));
  rc |= (int)gcu_loop_cancel(event_loop, &loop_op);
  rc |= (int)gcu_loop_run_once(event_loop, 0);
  rc |= (int)gcu_loop_post(event_loop, &loop_op);
  rc |= (int)gcu_loop_destroy(event_loop);
  rc |= (int)gcu_socket_close(sock);

  return rc;
}
