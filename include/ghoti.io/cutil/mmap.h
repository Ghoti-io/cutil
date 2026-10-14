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
 * Reading a file as memory instead of copying it into a buffer.
 *
 * `gcu_file_read()` reads a whole file into an allocation; this maps it
 * instead, so a large file costs address space rather than resident memory
 * and the kernel pages in what is actually touched. Worth it for files large
 * relative to memory, for random access into a large file, and for sharing
 * one copy between processes. Not worth it for small files, where the mapping
 * setup costs more than the read.
 *
 * ## Touching the mapping can raise a signal, not return an error
 *
 * This is the property that makes a mapping different from a buffer, and it
 * is not optional or avoidable.
 *
 * A read from a buffer cannot fail. A read from a mapping is a page fault
 * that the kernel services by reading the file, and if that read fails -- an
 * I/O error, a network filesystem going away, or **the file being truncated
 * under you** -- the process gets `SIGBUS`. There is no return value to
 * check, because the failure happens at a memory access.
 *
 * So: map files you control, and do not map a file another process is
 * shortening. If that is not something you can promise, read the file
 * normally. A truncation racing a read through `gcu_file_read()` gives you a
 * short buffer; the same race here kills the process.
 *
 * ## An empty file maps to nothing, successfully
 *
 * `mmap` cannot map zero bytes and `CreateFileMapping` refuses a zero-length
 * file, so an empty file would naturally be an error. It is not one here: the
 * call succeeds with `data` NULL and `size` 0, because "the file is empty" is
 * an ordinary answer and making every caller special-case it would mean every
 * caller getting it wrong once. Do not dereference `data` without checking
 * `size`, which is true of any buffer.
 *
 * ## Writes
 *
 * A writable mapping is written by storing into `data`. There is no write
 * call. Stores may reach the file at any time or not until unmap, so
 * gcu_mmap_sync() exists for the case where the order matters -- a crash
 * between two stores can leave the file with either, neither, or only the
 * second. A writable mapping cannot change the file's length: to grow a file,
 * write it normally, or unmap, extend, and map again.
 */

#ifndef GHOTI_IO_GCU_MMAP_H
#define GHOTI_IO_GCU_MMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <ghoti.io/cutil/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A mapped file.
 *
 * @p data and @p size are yours to read.  The rest is not.
 */
typedef struct {
  void * data;       ///< The mapping, or NULL for an empty file.
  size_t size;       ///< Bytes mapped.  0 for an empty file.
#ifdef _WIN32
  void * file;       ///< Private.
  void * mapping;    ///< Private.
#else
  int fd;            ///< Private.
#endif
} GCU_Mapped_File;

/**
 * Map a whole file.
 *
 * @param map Receives the mapping.  Only valid when the call returns 0.
 * @param path The file, UTF-8.  Must exist; this never creates one.
 * @param writable true for a read-write mapping whose stores reach the file.
 *   The file is opened for writing, so this fails on a read-only file.
 * @return 0 on success, -1 on failure.  A file too long for `size_t` (4 GiB
 *   or more on a 32-bit build) is refused with `errno` set to `EFBIG` on
 *   POSIX, and never mapped short.
 */
GCU_API int gcu_mmap_open(GCU_Mapped_File * map, const char * path,
    bool writable);

/**
 * Unmap and close.  Any pointer into @p data becomes invalid.
 *
 * For a writable mapping this also flushes, so an explicit gcu_mmap_sync() is
 * only needed before that point.
 *
 * @param map A mapping from a successful gcu_mmap_open().  Cleared.
 * @return 0 on success, -1 on failure.
 */
GCU_API int gcu_mmap_close(GCU_Mapped_File * map);

/**
 * Push pending stores to the file and wait for them to land.
 *
 * Only meaningful for a writable mapping; succeeds and does nothing for a
 * read-only or empty one, so a caller need not branch.
 *
 * @param map The mapping.
 * @return 0 on success, -1 on failure.
 */
GCU_API int gcu_mmap_sync(GCU_Mapped_File * map);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_MMAP_H
