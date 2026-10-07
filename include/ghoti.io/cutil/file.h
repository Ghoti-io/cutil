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
 * Reading a whole file, and replacing one without ever leaving it partly
 * written.
 *
 * The design and the reasoning behind each decision are recorded in
 * `documentation/file.md`.
 */

#ifndef GHOTI_IO_GCU_FILE_H
#define GHOTI_IO_GCU_FILE_H

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Passed as `max_bytes` to read a file of any size.
 */
#define GCU_FILE_UNLIMITED ((size_t)0)

/**
 * The outcome of a file operation.
 */
typedef enum GCU_File_Result {
  GCU_FILE_OK = 0,       ///< Succeeded.
  GCU_FILE_ERR_INVALID,  ///< A caller-supplied argument is wrong.
  GCU_FILE_ERR_OOM,      ///< The allocator returned NULL.
  GCU_FILE_ERR_LIMIT,    ///< The file is larger than `max_bytes`.
  GCU_FILE_ERR_IO,       ///< Open, read, write, rename or sync failed.
  /**
   * There is nothing at that path.
   *
   * Told apart from ::GCU_FILE_ERR_IO deliberately:  a path that is not there
   * is usually the caller's own mistake, and a disk that will not read is
   * not.  Collapsing the two forces every caller that cares to ask the
   * filesystem a second question afterwards, which is a race as well as a
   * duplication.
   */
  GCU_FILE_ERR_NOT_FOUND,
  GCU_FILE_ERR_EXISTS,    ///< Something is already there.
  GCU_FILE_ERR_ACCESS,    ///< The filesystem refused on permission grounds.
  GCU_FILE_ERR_NOT_EMPTY, ///< A directory still has entries in it.
  /**
   * One past the last code.  **Its value changes whenever this enum grows**,
   * which is the point of it - so it is safe to name (a `case` label keeping a
   * switch exhaustive, a bound in code compiled against this same header) and
   * unsafe to *keep*.  Never store it, transmit it, or size anything with it
   * that outlives the compilation: a `names[GCU_FILE_RESULT_COUNT]` sized
   * against
   * an older header is indexed past its end by a newer library.
   *
   * Never returned.
   */
  GCU_FILE_RESULT_COUNT,
} GCU_File_Result;

/**
 * What sort of thing is at a path.
 */
typedef enum GCU_File_Type {
  GCU_FILE_TYPE_REGULAR = 0, ///< An ordinary file.
  GCU_FILE_TYPE_DIRECTORY,   ///< A directory.
  GCU_FILE_TYPE_SYMLINK,     ///< A symbolic link, from a query that did not
                             ///< follow it.
  GCU_FILE_TYPE_OTHER,       ///< A device, socket, FIFO or anything else.
} GCU_File_Type;

/**
 * What the filesystem knows about a path.
 *
 * Deliberately small.  Ownership, permissions, link counts, device numbers and
 * the several kinds of timestamp a platform may or may not keep are left out
 * for the reason given in `documentation/file.md`:  they cannot be described
 * accurately on both POSIX and Windows, and a struct that pretended otherwise
 * would be wrong somewhere rather than absent everywhere.
 */
typedef struct GCU_File_Info {
  GCU_File_Type type; ///< What it is.
  uint64_t size;      ///< Bytes.  Meaningful for a regular file.
  /**
   * Last modification, in nanoseconds since 1970-01-01T00:00:00Z.
   *
   * A plain integer and not a `chron` type, because `chron` is built on this
   * library and not the other way round.  The resolution the filesystem
   * actually keeps varies - many record whole seconds - so this is the value
   * converted, not the precision promised.  Windows counts 100-nanosecond
   * ticks from 1601 and is converted at the boundary.
   */
  int64_t mtime_ns;
} GCU_File_Info;

/**
 * Name a result, for diagnostics.
 *
 * @param result The result to name.
 * @return A static string, owned by the library.  Never NULL: an
 *   out-of-range value yields `"unknown"`.
 */
GCU_API const char * gcu_file_result_string(GCU_File_Result result);

/**
 * How hard to try to get bytes onto the disk before reporting success.
 */
typedef enum GCU_File_Sync {
  /**
   * Flush the file and ask the operating system to commit it before the
   * rename, and ask it to commit the directory entry afterwards.
   *
   * This is the zero value, so a caller who does not think about it gets the
   * durable behaviour rather than the fast one.
   */
  GCU_FILE_SYNC_FULL = 0,
  /**
   * Flush, but leave the commit to the filesystem's own writeback.
   *
   * The rename is still atomic - a reader sees the old file or the new one,
   * never a mixture - but after a power loss the new one may be absent or
   * empty even though the call reported success.  Reasonable for output that
   * can simply be regenerated; wrong for anything that cannot.
   */
  GCU_FILE_SYNC_NONE,
} GCU_File_Sync;

/**
 * What permissions the finished file should carry.
 *
 * Deliberately three values and not a `mode_t`.  An access model cannot be
 * described accurately across platforms - POSIX derives a new file's
 * permissions from the process umask, Windows inherits access control entries
 * from the parent directory and has no umask at all - so this library does not
 * try to model one.  It asks the only question it can answer on both: should
 * the file be readable by anyone other than its owner?  Anything finer is the
 * caller's to do, with its own platform's own API.
 *
 * The temporary file is owner-only for its whole life regardless of this
 * setting.  The choice is applied immediately before the rename, so content is
 * never readable before it is complete.
 */
typedef enum GCU_File_Perms {
  /**
   * Only the owner may read or write it.
   *
   * This is the zero value, so a caller who does not think about it does not
   * publish anything by omission.  It is the right choice for a token, a
   * key, a session cache - anything a caller would not put in a world-
   * readable directory on purpose.
   */
  GCU_FILE_PERMS_PRIVATE = 0,
  /**
   * Whatever this platform would have given a newly created file.
   *
   * The same permissions an ordinary `fopen()` of the destination would have
   * produced, which on POSIX means the umask is honoured and a default ACL on
   * the containing directory overrides it.  This library does not compute
   * that - it asks the operating system, because the rules that combine the
   * umask with an inherited ACL are the operating system's and reproducing
   * them here would only reproduce them wrongly.
   */
  GCU_FILE_PERMS_DEFAULT,
  /**
   * The permissions the destination already has, or ::GCU_FILE_PERMS_DEFAULT
   * if it does not exist yet.
   *
   * The right choice for replacing a file rather than creating one:  a
   * configuration file somebody has deliberately narrowed should not be
   * widened by being rewritten, and one they have deliberately widened should
   * not be narrowed.  Replacing a file is not the same act as creating it,
   * and this is the value that says so.
   */
  GCU_FILE_PERMS_PRESERVE,
} GCU_File_Perms;

/**
 * Read an entire file into memory.
 *
 * The file is read in chunks rather than sized first, so it works on inputs
 * that report no size at all - pipes, character devices, and everything under
 * `/proc`.
 *
 * The buffer handed back always has a NUL one byte past `out_len`, which is
 * not counted in it.  A text caller may therefore use the result as a C
 * string without copying it, and a binary caller can ignore the byte.
 *
 * @param path The file to read.
 * @param max_bytes Refuse a file larger than this, or ::GCU_FILE_UNLIMITED
 *   for no limit.  A file that exceeds it yields ::GCU_FILE_ERR_LIMIT and
 *   nothing is allocated - the limit is a promise, not a truncation.
 * @param allocator Allocator for the buffer, or NULL for the default.
 * @param out_data Receives the buffer, owned by the caller and released with
 *   ::gcu_file_free().  Written only on success.
 * @param out_len Receives the length, excluding the added NUL.  Written only
 *   on success.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_OOM,
 *   ::GCU_FILE_ERR_LIMIT, ::GCU_FILE_ERR_NOT_FOUND, ::GCU_FILE_ERR_ACCESS or
 *   ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_read(const char * path, size_t max_bytes,
  const GCU_Allocator * allocator, void ** out_data, size_t * out_len);

/**
 * Release a buffer from ::gcu_file_read().
 *
 * @param allocator The allocator the buffer came from, or NULL for the
 *   default.  It must be the same one.
 * @param data The buffer.  NULL is accepted and ignored.
 */
GCU_API void gcu_file_free(const GCU_Allocator * allocator, void * data);

/**
 * A temporary file, open and owned by the caller.
 *
 * The struct is published so that it can live on the stack.  Every field is
 * private; reach the file through ::gcu_file_temp_stream() and its name
 * through ::gcu_file_temp_path().
 */
typedef struct GCU_File_Temp {
  char * path;                     ///< Private.  The name on disk.
  FILE * stream;                   ///< Private.  Open for reading and writing.
  const GCU_Allocator * allocator; ///< Private.  Where `path` came from.
} GCU_File_Temp;

/**
 * Create and open a uniquely named temporary file.
 *
 * The name is chosen and the file created in one step that fails if the name
 * already exists, and it is opened so that only its owner may read it.  The
 * alternative - inventing a name and then opening it - is a standing
 * invitation to have something else put a symbolic link there in between.
 *
 * Where to put it:
 *
 * - For scratch space, pass `dir = NULL` and it goes wherever
 *   ::gcu_path_temp_dir() says.
 * - To replace an existing file atomically, pass the *destination's own
 *   directory*, so that the later rename stays on one filesystem and is
 *   therefore atomic.  ::gcu_file_write_atomic() does this for you.
 *
 * The handle must be disposed of exactly once, by ::gcu_file_temp_commit()
 * or ::gcu_file_temp_abort().  Both leave it zeroed, and abort accepts a
 * zeroed handle, so `abort` may be called unconditionally on a cleanup path
 * without tracking whether commit already ran.
 *
 * @param temp Receives the handle.  Zeroed first, so it is safe to abort
 *   even if this call fails.
 * @param dir Directory to create it in, or NULL for the system temporary
 *   directory.
 * @param prefix Start of the filename, or NULL for `"tmp"`.  Six random
 *   characters are appended.
 * @param allocator Allocator for the stored name, or NULL for the default.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_OOM or
 *   ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_temp_create(GCU_File_Temp * temp,
  const char * dir, const char * prefix, const GCU_Allocator * allocator);

/**
 * The open stream of a temporary file.
 *
 * Positioned at the start and open for both reading and writing.
 *
 * @param temp The handle.
 * @return The stream, or NULL if @p temp is NULL or already disposed of.
 *   The handle owns it; do not `fclose()` it.
 */
GCU_API FILE * gcu_file_temp_stream(const GCU_File_Temp * temp);

/**
 * The name of a temporary file on disk.
 *
 * @param temp The handle.
 * @return The path, or NULL if @p temp is NULL or already disposed of.  The
 *   handle owns it; it stops being valid when the handle is disposed of.
 */
GCU_API const char * gcu_file_temp_path(const GCU_File_Temp * temp);

/**
 * Close a temporary file and move it into place, replacing @p dest.
 *
 * A reader sees either the whole of the old file or the whole of the new one
 * and never a mixture, provided the temporary file was created in the
 * destination's own directory - a rename across filesystems is a copy, and a
 * copy is not atomic.
 *
 * On failure the temporary file is removed, @p dest is left exactly as it
 * was, and the handle is spent either way.
 *
 * With ::GCU_FILE_SYNC_FULL the file is committed before the rename; a
 * failure there is reported, because those are the bytes.  The directory
 * entry is committed afterwards on a best-effort basis and a failure there is
 * *not* reported, because several filesystems refuse the request outright and
 * failing an otherwise complete replacement over it would be worse than the
 * weaker promise.  `documentation/file.md` says what that promise is.
 *
 * The temporary file is owner-only until this call, whatever @p perms says.
 * The permissions are applied immediately before the rename, so no reader can
 * see the content until all of it is there.  A refusal to apply them fails
 * the call and leaves @p dest untouched, because handing back a file with
 * permissions other than the ones asked for is worse than not writing it.
 *
 * @param temp The handle, from ::gcu_file_temp_create().
 * @param dest The path to replace.  It need not already exist.
 * @param sync How hard to try to reach the disk.
 * @param perms What permissions the finished file should carry.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_temp_commit(GCU_File_Temp * temp,
  const char * dest, GCU_File_Sync sync, GCU_File_Perms perms);

/**
 * Close and delete a temporary file.
 *
 * This is the cleanup path, and it is a named function precisely so that it
 * cannot be forgotten the way an open-coded `remove()` can: there is exactly
 * one of it, rather than one per error branch.
 *
 * @param temp The handle.  NULL, and a handle already disposed of, are
 *   accepted and ignored.
 */
GCU_API void gcu_file_temp_abort(GCU_File_Temp * temp);

/**
 * How an open file may be used.
 */
typedef enum GCU_File_Open_Mode {
  GCU_FILE_OPEN_READ = 0, ///< Read only.  The file must already exist.
  GCU_FILE_OPEN_WRITE,    ///< Write only.  Created, or emptied if it exists.
  GCU_FILE_OPEN_APPEND,   ///< Write only, always at the end.  Created if
                          ///< absent.
  GCU_FILE_OPEN_UPDATE,   ///< Read and write.  The file must already exist.
} GCU_File_Open_Mode;

/**
 * Where an offset is measured from.
 */
typedef enum GCU_File_Seek_From {
  GCU_FILE_SEEK_SET = 0, ///< From the beginning.
  GCU_FILE_SEEK_CUR,     ///< From where the file is now.
  GCU_FILE_SEEK_END,     ///< From the end; a negative offset goes backwards.
} GCU_File_Seek_From;

/**
 * An open file.
 *
 * Opaque, and disposed of by ::gcu_file_close().  Treat the members as
 * private.
 */
typedef struct GCU_File_Handle {
  FILE * stream;                   ///< Private.
  const GCU_Allocator * allocator; ///< Private.
} GCU_File_Handle;

/**
 * Open a file for reading, writing or both.
 *
 * This module is mostly about whole files, and deliberately so.  What this
 * adds is the case that whole-file reading cannot serve:  a file too large to
 * hold, or one whose interesting part is somewhere in the middle.
 *
 * It is a thin layer over stdio, because `fopen` and `fread` are already
 * standard C and wrapping them again would buy nothing.  What it fixes is the
 * three things stdio does *not* get right across platforms:
 *
 * - **UTF-8 paths.**  `fopen` on Windows cannot open a path whose bytes are
 *   UTF-8; this opens through the wide entry point, as the rest of the module
 *   does.
 * - **Offsets past 2 GB.**  `ftell` returns `long`, which is 32 bits on
 *   64-bit Windows, so seeking a large file through plain stdio silently
 *   stops working at two gigabytes.  ::gcu_file_seek() and ::gcu_file_tell()
 *   are 64-bit everywhere.
 * - **Line endings.**  Always binary.  A text-mode read on Windows rewrites
 *   the bytes and makes `ftell` disagree with how many there are.
 *
 * @param handle Receives the open file.  Zeroed before anything can fail, so
 *   a handle from a failed open is safe to pass to ::gcu_file_close().
 * @param path The file to open.
 * @param mode What it will be used for.
 * @param perms Permissions for a file this call creates; ignored for one that
 *   already exists, whose permissions are never changed by opening it.
 *   ::GCU_FILE_PERMS_PRESERVE and ::GCU_FILE_PERMS_DEFAULT are the same thing
 *   here, since there is nothing to preserve when the file is new.
 * @param allocator Allocator for working memory, or NULL for the default.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_ACCESS, ::GCU_FILE_ERR_OOM or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_open(GCU_File_Handle * handle,
  const char * path, GCU_File_Open_Mode mode, GCU_File_Perms perms,
  const GCU_Allocator * allocator);

/**
 * Close an open file.
 *
 * Returns a result, and the result is worth reading:  on a buffered write the
 * close is where buffered bytes finally reach the operating system, so it is
 * the call that reports a full disk.  Ignoring it is how a program loses the
 * last few kilobytes of what it wrote and never finds out.
 *
 * The handle is spent either way, and closing an already-closed or zeroed
 * handle is accepted and does nothing - so this may sit on an unconditional
 * cleanup path.
 *
 * @param handle The handle.  NULL is accepted and ignored.
 * @return ::GCU_FILE_OK, or ::GCU_FILE_ERR_IO if buffered output could not be
 *   written.
 */
GCU_API GCU_File_Result gcu_file_close(GCU_File_Handle * handle);

/**
 * Read up to @p size bytes.
 *
 * A short read is not an error:  @p out_got says how many arrived, and zero
 * with ::GCU_FILE_OK means the end of the file.  Anything reading a stream
 * has to cope with this, which is why the count is reported rather than
 * demanded.
 *
 * @param handle The handle.
 * @param buffer Where to put them.
 * @param size How many at most.
 * @param out_got Receives how many were read.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_read_bytes(GCU_File_Handle * handle,
  void * buffer, size_t size, size_t * out_got);

/**
 * Write @p len bytes.
 *
 * Unlike a read, a short write *is* an error:  there is no reason for one
 * except a failure, and a caller that carried on would be building a file
 * with a hole in it.
 *
 * @param handle The handle.
 * @param data The bytes.  May be NULL only if @p len is 0.
 * @param len How many.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_write_bytes(GCU_File_Handle * handle,
  const void * data, size_t len);

/**
 * Move to another position in the file.
 *
 * Seeking past the end is allowed and is how a sparse file is made:  the gap
 * reads back as zeroes once something has been written beyond it.  Seeking to
 * before the beginning is refused.
 *
 * @param handle The handle.
 * @param offset How far, in bytes.  64-bit on every platform.
 * @param from Where @p offset is measured from.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_seek(GCU_File_Handle * handle,
  int64_t offset, GCU_File_Seek_From from);

/**
 * Say where in the file the handle is.
 *
 * @param handle The handle.
 * @param out_offset Receives the position, in bytes from the beginning.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_tell(GCU_File_Handle * handle,
  int64_t * out_offset);

/**
 * Whether a previous read reached the end of the file.
 *
 * Only ever true after a read has already come up short.  Testing it before
 * reading answers the previous question, not the next one, which is the
 * mistake this flag invites in every language that has it.
 *
 * @param handle The handle.  NULL is false.
 * @return true if the end has been reached.
 */
GCU_API bool gcu_file_eof(const GCU_File_Handle * handle);

/**
 * Push buffered output to the operating system.
 *
 * Not to the disk.  ::gcu_file_sync() is that, and the difference is the one
 * section 6 of `documentation/file.md` is about.
 *
 * @param handle The handle.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_flush(GCU_File_Handle * handle);

/**
 * Ask the operating system to commit the file to the disk.
 *
 * Flushes first, so a caller need not do both.  This is the call that survives
 * a power loss; a flush alone survives only a killed process.
 *
 * @param handle The handle.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_sync(GCU_File_Handle * handle);

/**
 * Ask the filesystem what is at a path.
 *
 * Follows symbolic links, so a link to a directory reports
 * ::GCU_FILE_TYPE_DIRECTORY.  Use ::gcu_file_stat_link() to ask about the link
 * itself.
 *
 * @param path The path to ask about.
 * @param out Filled in on success; untouched otherwise.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_stat(const char * path, GCU_File_Info * out);

/**
 * Ask about a path without following a symbolic link at the end of it.
 *
 * The distinction matters to anything that walks a tree:  following links is
 * how a walk leaves the tree it was asked about, and how a recursive delete
 * removes something it was never pointed at.
 *
 * @param path The path to ask about.
 * @param out Filled in on success; untouched otherwise.
 * @return As ::gcu_file_stat().
 */
GCU_API GCU_File_Result gcu_file_stat_link(const char * path,
  GCU_File_Info * out);

/**
 * Whether anything exists at a path.
 *
 * Convenience over ::gcu_file_stat(), and it answers "no" both for a path that
 * is absent and for one the caller is not allowed to look at.  When that
 * difference matters - and for anything security-shaped it does - ask
 * ::gcu_file_stat() instead and read the result.
 *
 * Note also that the answer is history by the time it is returned:  anything
 * may create or remove that path immediately afterwards.  Use it to give a
 * better error message, not to decide that a later operation will succeed.
 *
 * @param path The path to test.  NULL is "no".
 * @return true if something is there.
 */
GCU_API bool gcu_file_exists(const char * path);

/**
 * Whether a path names a directory, following symbolic links.
 *
 * Carries the same caveats as ::gcu_file_exists().
 *
 * @param path The path to test.  NULL is "no".
 * @return true if it is a directory.
 */
GCU_API bool gcu_file_is_directory(const char * path);

/**
 * Delete a file.
 *
 * Not a directory:  ::gcu_dir_remove() is that, and keeping them apart means a
 * caller cannot delete a whole directory by passing the wrong variable.
 *
 * @param path The file to delete.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_remove(const char * path);

/**
 * Move a file, replacing anything already at the destination.
 *
 * Atomic only within one filesystem; across filesystems this fails rather than
 * silently becoming a copy, because a copy is not atomic and a caller who
 * asked for a rename is usually relying on that.  ::gcu_file_copy() followed
 * by ::gcu_file_remove() is the explicit spelling of the other thing.
 *
 * @param from The existing path.
 * @param to The path to move it to.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_rename(const char * from, const char * to);

/**
 * Copy a file's contents to another path.
 *
 * Goes through ::gcu_file_temp_create() and ::gcu_file_temp_commit(), so the
 * destination appears whole or not at all and a failed copy leaves no
 * half-written file behind.  @p perms and @p sync mean what they mean there;
 * the source's own permissions are *not* carried across, because that would be
 * this library making the access-model decision it declines to make.
 *
 * @param from The file to read.
 * @param to The path to write.  Replaced if it exists.
 * @param sync How hard to try to reach the disk.
 * @param perms What permissions the copy should carry.
 * @param allocator Allocator for working memory, or NULL for the default.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_OOM, ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_copy(const char * from, const char * to,
  GCU_File_Sync sync, GCU_File_Perms perms, const GCU_Allocator * allocator);

/**
 * Replace a file's contents, atomically.
 *
 * Writes @p data to a temporary file in @p path's own directory and renames
 * it over @p path.  Equivalent to ::gcu_file_temp_create(),
 * ::gcu_file_temp_stream() and ::gcu_file_temp_commit() done in order, for
 * the common case where the whole content is already in memory.
 *
 * @param path The file to replace.  It need not already exist.
 * @param data The bytes to write.  May be NULL only if @p len is 0.
 * @param len How many bytes.
 * @param sync How hard to try to reach the disk.
 * @param perms What permissions the finished file should carry.  Note that
 *   the zero value is ::GCU_FILE_PERMS_PRIVATE, not the permissions a plain
 *   `fopen()` would have produced; pass ::GCU_FILE_PERMS_DEFAULT for those.
 * @param allocator Allocator for working memory, or NULL for the default.
 *   Nothing is handed back to free.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_OOM or
 *   ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_write_atomic(const char * path,
  const void * data, size_t len, GCU_File_Sync sync, GCU_File_Perms perms,
  const GCU_Allocator * allocator);

/**
 * Which file a path is, for telling two names of one file apart.
 *
 * ::GCU_File_Info leaves this out on purpose: a uid or a permission model
 * cannot be described on both platforms, and that struct refuses to pretend.
 * A hard link is the case that still needs an answer. Two directory entries
 * are the same file when the device and inode agree, and `links` is how many
 * names the file has, so a walk can see that one of them is outside the tree
 * it was given. `mode` is the permission bits only (setuid, setgid and sticky
 * included, the file-type bits not). On Windows a Unix mode does not apply
 * and `mode` is 0; the identity itself is still reported.
 *
 * Does not follow a symbolic link at the end of the path.
 */
typedef struct GCU_File_Identity {
  uint64_t device; ///< The device holding the file.
  uint64_t inode;  ///< The file's identity on that device.
  uint64_t links;  ///< How many directory entries name it.
  uint32_t mode;   ///< Permission bits, or 0 where a Unix mode does not apply.
} GCU_File_Identity;

/**
 * Ask which file is at a path, without following a final symbolic link.
 *
 * @param path The path to ask about.
 * @param out Filled in on success; untouched otherwise.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_identity(const char * path,
  GCU_File_Identity * out);

/**
 * Create a symbolic link.
 *
 * @p target is stored as the link text. It need not name anything that
 * exists, and it is not rewritten: a relative target stays relative.
 *
 * On Windows this does not apply. The call returns ::GCU_FILE_ERR_ACCESS and
 * creates nothing, rather than reporting success or writing an ordinary file
 * in the link's place. A symbolic link there needs a privilege a normal user
 * does not have, and pretending one was created is worse than saying so.
 *
 * @param target The link text. Not NULL.
 * @param path The path of the new link. Not NULL.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_EXISTS,
 *   ::GCU_FILE_ERR_NOT_FOUND (a missing parent), ::GCU_FILE_ERR_ACCESS or
 *   ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_symlink(const char * target,
  const char * path);

/**
 * Read the text of a symbolic link.
 *
 * The buffer is NUL-terminated one byte past `out_len`, which is not counted
 * in it, the same arrangement as ::gcu_file_read(). The text is what the link
 * stores, not a resolved path, and it may be relative.
 *
 * @param path The link to read.
 * @param allocator Allocator for the buffer, or NULL for the default.
 * @param out_target Receives the buffer, owned by the caller and released
 *   with ::gcu_file_free(). Written only on success.
 * @param out_len Receives the length, excluding the added NUL. Written only
 *   on success.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_OOM, ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_read_link(const char * path,
  const GCU_Allocator * allocator, char ** out_target, size_t * out_len);

/**
 * Create a hard link.
 *
 * @p path becomes another name for the file at @p existing. The two are one
 * file afterwards: the bytes are not copied.
 *
 * @param existing A path that already names the file.
 * @param path The new name. It must not already exist.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_EXISTS, ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_hardlink(const char * existing,
  const char * path);

/**
 * Set the permission bits of a path.
 *
 * Applies @p mode as it is given. Setuid, setgid and sticky are included when
 * the caller passes them and cleared when the caller does not: this function
 * does not have a policy about them. It follows a symbolic link, as `chmod`
 * does, so the bits land on the file the link names.
 *
 * On Windows a Unix mode does not apply. The call returns
 * ::GCU_FILE_ERR_ACCESS and changes nothing, rather than reporting success
 * for a mode it did not set. A path that is not there is still
 * ::GCU_FILE_ERR_NOT_FOUND, so the two failures stay apart.
 *
 * @param path The path to change.
 * @param mode The permission bits to set.
 * @return ::GCU_FILE_OK, ::GCU_FILE_ERR_INVALID, ::GCU_FILE_ERR_NOT_FOUND,
 *   ::GCU_FILE_ERR_ACCESS or ::GCU_FILE_ERR_IO.
 */
GCU_API GCU_File_Result gcu_file_set_mode(const char * path, uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_FILE_H
