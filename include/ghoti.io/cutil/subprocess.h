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
 * Running another program: give it an argv, get back its output and its exit
 * code.
 *
 * This is deliberately one operation rather than a process-control API. There
 * is no way to start a child and keep talking to it, no pid to hold, no
 * signals to send. That covers `system()`-shaped work -- ask a tool a
 * question, get the answer -- which is nearly all of it, and it is the shape
 * that can be made correct on both platforms without the caller doing
 * anything.
 *
 * ## There is no shell
 *
 * @p argv is handed to the program as its argument vector. Nothing expands
 * `*`, nothing splits on spaces, nothing interprets `>` or `|` or `;`, and a
 * filename containing a quote or a newline is just a filename. So a value
 * that came from a user cannot turn one command into two, which is the entire
 * reason to prefer this over `system()`.
 *
 * If you genuinely want shell syntax, ask for it explicitly -- pass
 * `{"/bin/sh", "-c", script, NULL}` -- and then the injection risk is yours,
 * visible at the call site, rather than hidden in the wrapper.
 *
 * ## Both streams are read at once
 *
 * A child that writes more than a pipe will hold -- 64 KiB on Linux -- blocks
 * until someone reads it. So a parent that drains stdout to the end and only
 * then looks at stderr will hang forever against a child that fills stderr
 * first, and will pass every test written with a small fixture. This reads
 * from both as they fill, and writes `GCU_Subprocess_Options::input` at
 * the same time, so no ordering of the child's writes can deadlock it.
 *
 * ## The output is collected in memory
 *
 * All of it, before the call returns. That is the price of the simple shape.
 * For a child whose output is unbounded -- or merely large enough to matter
 * -- set `GCU_Subprocess_Options::output_limit`, which stops the run
 * rather than the machine.
 *
 * ## The child gets three descriptors and no others
 *
 * Both platforms promise this, by opposite means, which is worth knowing
 * because the POSIX half is the one that is easy to get wrong. A Windows
 * handle is not inheritable unless it was created that way, so the child
 * inherits only the three this code marks. A POSIX descriptor *is* inherited
 * unless it is close-on-exec, so the child closes everything above 2 itself.
 *
 * That matters beyond tidiness: an inherited descriptor keeps a file, a lock
 * or a socket open for as long as the child lives, and a child that outlives
 * the parent then holds them after the parent has exited.
 *
 * ## Only the child is killed
 *
 * `GCU_Subprocess_Options::timeout` and
 * `GCU_Subprocess_Options::output_limit` both end a run by killing the
 * process that was started -- not anything it started. A child that forks and
 * exits leaves its own children running, and this cannot see them. Where that
 * matters, the child needs to be something that cleans up after itself.
 */

#ifndef GHOTI_IO_GCU_SUBPROCESS_H
#define GHOTI_IO_GCU_SUBPROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <ghoti.io/cutil/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returned by gcu_subprocess_run() when the program never ran.
 *
 * Distinct from 0, which means the program ran and its exit code is worth
 * reading, and from -1, which means this library or the operating system
 * failed before it got that far.
 *
 * The distinction is the reason this code exists rather than a bare
 * `exit_code`. A shell reports "command not found" as exit code 127, and so
 * does a program that chose to exit 127; a caller cannot tell "your path is
 * wrong" from "the tool ran and told you something" from the number alone.
 *
 * Why the program could not be run is left in the platform's error slot, so
 * `gcu_error_string_last()` describes it -- `ENOENT` for a name that is not
 * on `PATH`, `EACCES` for a file that is not executable, and whatever
 * `GCU_Subprocess_Options::directory` failed with.
 */
#define GCU_SUBPROCESS_NOT_STARTED (-2)

/**
 * How a run ended.
 *
 * One field rather than a set of booleans, because the states are exclusive
 * and three booleans can be made to disagree.
 */
typedef enum {
  /// The program ran to completion.  `GCU_Subprocess_Result::exit_code`
  /// is what it passed to `exit()`.
  GCU_SUBPROCESS_EXITED = 0,
  /// A signal killed it.  `GCU_Subprocess_Result::signal` says which.
  /// POSIX only; a Windows process has no equivalent and never reports this.
  GCU_SUBPROCESS_SIGNALED,
  /// `GCU_Subprocess_Options::timeout` elapsed and this killed the child.
  GCU_SUBPROCESS_TIMED_OUT,
  /// `GCU_Subprocess_Options::output_limit` was reached and this killed
  /// the child.  The output collected so far is kept.
  GCU_SUBPROCESS_OUTPUT_LIMIT,
} GCU_Subprocess_Outcome;

/**
 * What to run and how.
 *
 * Zero-initialise it and set what you need: every field's zero value is the
 * unconstrained one -- inherit the environment, inherit the directory, send
 * no input, wait as long as it takes, collect as much as it produces.
 */
typedef struct {
  /**
   * The program and its arguments, NULL-terminated.  Required, and
   * `argv[0]` must be present.
   *
   * `argv[0]` names the program. If it contains a directory separator it is
   * used as a path; otherwise it is looked up on `PATH` -- the `PATH` in
   * @p environment when that is given, so a replaced environment replaces
   * the search too.
   *
   * By convention `argv[0]` is also what the program sees as its own name,
   * and this passes it through unchanged rather than trying to be clever
   * about the two roles.
   */
  const char * const * argv;

  /**
   * Directory to run in, UTF-8, or NULL to inherit this process's.
   *
   * A directory that cannot be entered is a `GCU_SUBPROCESS_NOT_STARTED`,
   * not a child that runs somewhere unexpected.
   */
  const char * directory;

  /**
   * The child's whole environment as `"NAME=VALUE"` entries,
   * NULL-terminated, or NULL to inherit this process's.
   *
   * Whole, not added to: giving one entry gives the child exactly that one.
   * To add a variable, copy what `gcu_env_get()` gives you and append. The
   * replacing form is the one that can express both.
   */
  const char * const * environment;

  /// Bytes for the child's standard input, or NULL for none.  The child sees
  /// end-of-file once they have been written.
  const char * input;

  /// How many bytes of @p input.  Ignored when @p input is NULL.
  size_t input_size;

  /**
   * Give the child one stream instead of two: its standard error arrives in
   * `GCU_Subprocess_Result::out`, interleaved with its standard output as
   * a terminal would show it, and `GCU_Subprocess_Result::err` stays
   * empty.
   *
   * Interleaved by arrival, which is the child's flushing order and not
   * necessarily the order it wrote them in.
   */
  bool merge_stderr;

  /**
   * Give up after this many milliseconds and kill the child.  Zero or
   * negative waits as long as the child takes.
   *
   * Milliseconds as a plain `int`, matching `gcu_cond_timedwait()` and
   * `gcu_semaphore_timedwait()`: this library is the root of the dependency
   * graph and cannot name a type from a library above it.
   *
   * Waiting forever is worth a thought when the child might start something
   * that outlives it. This waits for the output streams to close, and a
   * grandchild holding one open keeps them open after the child has gone.
   */
  int timeout;

  /**
   * Stop once this many bytes have been captured, across both streams, and
   * kill the child.  Zero collects however much it produces.
   *
   * Zero means no limit -- the unconstrained value, like every other zero in
   * this struct -- and not some built-in default. A limit you did not ask for
   * would truncate output for a reason nothing in your code mentions.
   */
  size_t output_limit;
} GCU_Subprocess_Options;

/**
 * What happened.
 *
 * Fill it by calling gcu_subprocess_run(), read it, then hand it to
 * gcu_subprocess_result_free().
 */
typedef struct {
  /// How it ended; qualifies the two fields below.
  GCU_Subprocess_Outcome outcome;

  /// What the program passed to `exit()`.  Meaningful only for
  /// `GCU_SUBPROCESS_EXITED`.
  int exit_code;

  /// The signal that killed it, for `GCU_SUBPROCESS_SIGNALED`, and also
  /// for the two outcomes where this library did the killing -- the outcome,
  /// not this, is what says whose decision it was.  0 otherwise.
  int signal;

  /// Captured standard output, NUL-terminated for convenience.  NULL if the
  /// child wrote nothing.  It may contain NUL bytes of its own, so @p out
  /// is the length and `strlen()` is not.
  char * out;

  /// Bytes in @p out, not counting the terminator.
  size_t out_size;

  /// Captured standard error, on the same terms as @p out.  Always empty
  /// when `GCU_Subprocess_Options::merge_stderr` was set.
  char * err;

  /// Bytes in @p err, not counting the terminator.
  size_t err_size;
} GCU_Subprocess_Result;

/**
 * Run a program and wait for it.
 *
 * Success means the program ran, whatever it then did: a child that exits 1,
 * or crashes, or is killed for taking too long, is a run that happened and
 * returns 0. Read `GCU_Subprocess_Result::out`come to find out which.
 *
 * @param options What to run.  See @ref GCU_Subprocess_Options.
 * @param result Receives the outcome and the captured output.  Overwritten
 *   whatever the call returns, so a previous result must be freed first.
 *   Free it with gcu_subprocess_result_free() when the return is 0.
 * @return 0 if the program ran, `GCU_SUBPROCESS_NOT_STARTED` if it could
 *   not be started, -1 if this call failed before getting that far.
 */
GCU_API int gcu_subprocess_run(const GCU_Subprocess_Options * options,
    GCU_Subprocess_Result * result);

/**
 * Release the captured output.
 *
 * Safe on a zeroed result and safe to repeat, so an error path need not work
 * out whether the run got far enough to allocate anything.
 *
 * @param result The result to clear.  NULL is accepted and ignored.
 */
GCU_API void gcu_subprocess_result_free(GCU_Subprocess_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_SUBPROCESS_H
