# The event loop and sockets

**Status:** Implemented.  `include/ghoti.io/cutil/socket.h`,
`include/ghoti.io/cutil/loop.h`, `src/socket.c`, `src/loop.c`.

## 1. What it is

A socket is a handle on the operating system's object.  The loop is how
anything is done with one: start a read, a write, an accept, a connect, a
datagram send or receive, or a timer, and the loop tells you when that
operation has completed.  Nothing blocks the thread, and nothing exposes
whether a descriptor is ready.

```c
GCU_Loop * loop;
gcu_loop_create(&loop, NULL);

GCU_Loop_Op read_op;                       /* yours: the loop allocates nothing */
gcu_loop_op_init(&read_op, on_read, ctx);
gcu_loop_read(loop, &read_op, socket, buffer, sizeof(buffer));

gcu_loop_run(loop);                        /* on_read runs here, on this thread */
```

It is the mechanism and nothing else.  Which fiber runs next, how requests are
shared out between tenants and how long one may take are policy, and belong to
the scheduler that builds on this (the Defiant spine's AD-9).  The loop keeps
no run queue of fibers.

## 2. The decisions

### 2.1 Completion-shaped, not readiness-shaped

Linux (`epoll`), the BSDs and macOS (`kqueue`) report that a socket is
*ready*.  Windows (I/O completion ports) reports that an operation has
*finished*.  A completion interface can be built on readiness by doing the
call when readiness is reported.  A readiness interface cannot be built on
completion without an undocumented ioctl (`IOCTL_AFD_POLL`, which wepoll and
mio use) or a very recent API (`ProcessSocketNotifications`, Windows build
20348 and later).  So the one thing a caller ever learns is "this operation is
done, and here is what it moved", and a descriptor's readiness is never part
of the interface.

On Linux the arm attempts each call at once and arms `epoll` only when the OS
says it would block.  `EAGAIN` and `WSAEWOULDBLOCK` therefore never reach the
caller, because waiting is what the loop is for.  kqueue is intended and not
written: on the BSDs and macOS the library links, and every call that would
create a loop returns `GCU_LOOP_ERR_UNSUPPORTED`, as fibers do for macOS.

### 2.2 Operations are caller-owned records

An operation is a `GCU_Loop_Op` that the caller provides.  Nothing is
allocated per operation, so the buffer rule is visible in the type instead of
being a sentence in a manual:

> The loop owns the record and the buffer it names from the moment a start
> call returns `GCU_LOOP_OK` until its completion callback is entered.

**Cancelling does not end that earlier.**  `gcu_loop_cancel()` returns at once
and the loop reports `GCU_LOOP_CANCELLED` through the completion, never
sooner.  The reason is the one the spine gives in AD-9: a buffer freed because
a cancel was *asked for* is a buffer freed under an operation the OS may still
be writing to.  On Linux the cancel removes the operation from its socket's
queue before the completion is decided, so nothing reads into the buffer
afterwards; on Windows `CancelIoEx` asks the OS to let go and the completion
packet is the proof that it has.

A callback is only ever entered from `gcu_loop_run()` or
`gcu_loop_run_once()`, never from inside a start call or a cancel, even for an
operation that finished at once.  That is what lets a caller hold a lock
across a start without the callback re-entering it, and it makes "the
completion arrives after cancel returns" a rule with no exceptions.

An operation whose result is already decided (it completed, and only its
callback has yet to run) is not cancelled: `gcu_loop_cancel()` returns
`GCU_LOOP_ERR_STATE`, and the operation keeps its result.  Turning a finished
read into a cancelled one would lose bytes that have already left the socket.
On Windows the same is true of an operation the OS finished before the cancel
whose packet the loop has not collected yet: `CancelIoEx` reports
`ERROR_NOT_FOUND`, the cancel is refused, and the packet carries the real
result.  Any other `CancelIoEx` failure means no abort was requested and no
aborted packet will come, so the cancel is refused too, and a write that was
partly done when a cancel was accepted completes `CANCELLED` with the bytes
already moved instead of reissuing the rest.

Cancelling a connect that is in progress on Linux removes it from the loop's
queue but cannot call back the OS: the socket is half-connected, and unusable
for another connect or for data.  Close it.  Every other cancelled operation
leaves its socket as it was.

### 2.3 One thread, two exceptions

A loop is used by the thread that created it.  Every call from another thread
is refused with `GCU_LOOP_ERR_THREAD` except `gcu_loop_post()` and
`gcu_loop_wake()`.  This is the same rule the fibers have, for the same
reason: the Tang and Wasm contexts a fiber parks in a host call must resume on
their own thread (runtime AD-6), so everything that can resume them is
pinned there.

`gcu_loop_post()` is how another thread hands work to the loop, and in
particular how the offload pool (`pool.h`) returns a result: post a record
whose callback does the rest on the loop's thread.  The record is the
caller's, like any operation, and is queued intrusively, so a post allocates
nothing and cannot fail for lack of memory.  Posts run in the order they were
made.  A waiting loop is woken (an `eventfd` on Linux,
`PostQueuedCompletionStatus` on Windows).

The list is protected by a mutex, and the wake is made while holding it.
That is how destroy is kept from freeing the loop between an append and its
wake: destroy takes the same mutex to close the list, so a post either
finished first or is refused with `GCU_LOOP_ERR_STATE`.  What no mutex can do
is make a post safe against a loop that is already gone; as with `pool.h`,
the caller must not post concurrently with, or after, destroy returning.

### 2.4 Timers

One-shot, in integer milliseconds on a monotonic clock, started from the
moment of the call.  The clock is read in nanoseconds and the wait is rounded
*up* to a whole millisecond, so a timer never fires before its time, which
the test measures from outside.  How late it fires depends on what else the
loop is doing.  They are kept in a binary heap ordered by deadline, with start
order breaking ties, so timers due together fire in the order they were
started.  The heap grows through the loop's allocator, so a timer start can
return `GCU_LOOP_ERR_OOM`, and the record is then idle.  There are no
periodic timers: start another from the callback, which is allowed, because
the record is idle when its callback is entered.

### 2.5 The fiber helper

`gcu_loop_fiber_wait(op)` suspends the running fiber until the operation
completes.  The record must stay valid until the wait returns, not only until
its callback is entered, since the resumed fiber reads its result.  It lives in `loop.h` because only the loop knows when an operation
finished.  The loop resumes the fiber with `gcu_fiber_switch_to()` from inside
its iteration, after the record's own callback, on the loop's thread (which
is the fiber's thread, since fibers never move).  The fiber runs until it
waits again or finishes, and control comes back to the loop.

Which fiber runs *next* in any wider sense is not decided here.  A scheduler
that wants a run queue of its own starts operations with a callback that
enqueues the fiber, and resumes it itself; the helper is for the simple case
and for the tests.  Waiting outside a fiber, on a record that is not in
flight, on a posted record, or with another fiber already waiting returns
`GCU_LOOP_ERR_STATE`.

### 2.6 Sockets

`gcu_loop_pending()` and `gcu_loop_op_is_pending()` are owner-thread
questions: from another thread the first answers 0 and the second races the
loop.  A datagram longer than the receive buffer is cut to it with no flag to
the caller (`bytes` is the buffer's size), and `gcu_socket_address_format()`
writes text for people, not input for `gcu_socket_address_parse()` (it has a
port, brackets and a `%zone`), and refuses a buffer too small for all of it.
A post whose wake fails is taken back: the record is idle and no callback
will come.  An epoll registration that fails fails every operation queued on
that socket with `GCU_LOOP_ERR_OS`, so none waits for ever.

Sockets are non-blocking from creation and not inherited by child processes.
On Windows they are created overlapped, and `socket.c` owns `WSAStartup`: it
runs once, the first time a socket or an address is created, and is never
undone, since a `WSACleanup` racing a socket another thread still holds is the
worse hazard.  A new IPv6 socket is IPv6-only on every platform (Linux
defaults to dual-stack, Windows does not); an option turns it off.

An address is a number.  Names are not resolved here (that is the offload
pool's job and a later library's), and parsing refuses anything that is not a
dotted quad or colon-hex.  `gcu_socket_error_kind()` sorts an OS error code
into the dozen cases a caller acts on, the same way on every platform, and the
raw code stays available for `gcu_error_string()`.

`gcu_socket_close()` is refused while an operation on the socket is in
flight: the buffer is the loop's until the completion arrives, and a close
would free the socket under the loop's queue.  A close from inside a callback
is fine, including of the socket the callback was told about; its memory is
released at the end of the iteration that closed it, so an event already
collected for it can never name freed memory.  A socket belongs to the first
loop that uses it, and outlives that loop but cannot join another.

### 2.7 An accept takes its memory first

The accepted socket is a `GCU_Socket` allocated with the loop's allocator, and
it is allocated *before* a connection is taken off the OS.  A failure to
allocate completes with `GCU_LOOP_ERR_OOM` and leaves the connection in the
listener's backlog for the next accept.  On Windows this is forced, since
`AcceptEx` needs its socket beforehand; on Linux it is done the same way so
that running out of memory does not accept a connection and then close it on
a client that did nothing wrong.

### 2.8 Destroying a loop

`gcu_loop_destroy()` cancels every operation in flight and every posted record
that has not run, delivers each completion with `GCU_LOOP_CANCELLED`, and
returns only then.  A callback run during that drain can start nothing new
(`GCU_LOOP_ERR_STATE`), which is what makes the drain finish.  An operation
that had already completed keeps its real result.  On Windows the cancelled
operations are the OS's until their packets arrive, so the drain waits for
them.  The wait is bounded at thirty one-second waits, and when it gives up
destroy frees nothing (the OS may still write into a caller's buffer) and
returns `GCU_LOOP_ERR_STATE`: the loop stays valid, what was delivered stays
delivered, and destroy can be called again.

## 3. The Linux arm

`epoll` with level-triggered registration, dropped whenever both of a
socket's queues are empty.  A registration with no interest still reports
hang-up and error, and a loop told about a closed connection it has nothing to
do with spins, so a socket is only in the set while something waits on it.
Each socket has two queues, one for reads, accepts and receives and one for
writes, connects and sends, and only the head of each queue is tried, so
operations on one socket finish in the order started.

A connect is handed to the OS and, after `EINPROGRESS`, finished by reading
`SO_ERROR` when the socket reports writable.  `SO_ERROR` reads 0 while the
connection is still in progress, so only an event that says the connection
ended either way is believed.  Sends use `MSG_NOSIGNAL`: a write to a closed
peer is an error completion, not a signal.

## 4. The Windows arm

An I/O completion port, with an `OVERLAPPED` in the record, `AcceptEx`,
`ConnectEx` and `WSARecv`, `WSASend`, `WSARecvFrom` and `WSASendTo`.  A packet
comes back for every operation, including one that finished at once; nothing
here asks for success to skip the port, because one path for every completion
is worth more than the wake-ups saved.  Four things the first run under wine
found and the code now says:

- A write the OS took only part of is a new request for the rest, so a write
  completes with all of its bytes.
- A datagram longer than the buffer fails with `WSAEMSGSIZE` at once and is
  *still* completed through the port.  Treating the immediate failure as the
  result delivered the record twice; it is pending, and the packet is the
  answer, cut to the buffer as on Linux.
- A packet for a record that is not waiting for one is dropped, so a stray one
  cannot run a callback twice.
- `ConnectEx` wants the socket bound first, so a socket that was not is bound
  to the wildcard address.

It is verified under wine (`tools/xwin/loop.sh`) and has not run on a Windows
machine.

## 5. What the gates are

`make check-loop-defects` builds the library with one deliberate defect at a
time and requires the test that carries it to fail, after requiring the real
library to pass the same test.  The defects are a post that does not wake the
loop (the test waits in the OS with no timeout, so the hang guard names it), a
cancel that reports before the loop has let go of the buffer (a test that the
callback has not run when cancel returns, and an AddressSanitizer run that
frees the buffer in the completion and then sends data) and a timer queue
ordered by start.  `tools/xwin/loop.sh` and `tools/xarch/loop.sh` plant the
same defects against the Windows arm and the aarch64 build.  The measurements
and what was found are in `notes/cutil/event-loop.md`.
