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
 * Memory-mapped files.  See mmap.h for the contract, including the SIGBUS
 * warning that makes a mapping different from a buffer.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>
#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/mmap.h>
#include <ghoti.io/cutil/utf.h>

#ifdef _WIN32

#include <windows.h>

int gcu_mmap_open(GCU_Mapped_File * map, const char * path, bool writable) {
  if (!map || !path) {
    return -1;
  }
  memset(map, 0, sizeof(*map));
  map->file = INVALID_HANDLE_VALUE;

  size_t units = gcu_utf8_to_utf16(path, NULL, 0);
  if (!units) {
    return -1;
  }
  GCU_Char16 * wide = malloc(units * sizeof(GCU_Char16));
  if (!wide) {
    return -1;
  }
  gcu_utf8_to_utf16(path, wide, units);

  DWORD access = writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  HANDLE file = CreateFileW((LPCWSTR)wide, access,
      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (file == INVALID_HANDLE_VALUE) {
    return -1;
  }

  LARGE_INTEGER length;
  if (!GetFileSizeEx(file, &length)) {
    CloseHandle(file);
    return -1;
  }

  // An empty file is a success with nothing mapped.  CreateFileMapping
  // refuses a zero length, and "the file is empty" is an ordinary answer
  // rather than an error -- see mmap.h.
  if (length.QuadPart == 0) {
    // Handle kept open, for the reason given in the POSIX branch: otherwise
    // an empty mapping and a closed one are the same bytes.
    map->file = file;
    return 0;
  }

  HANDLE mapping = CreateFileMappingW(file,
      NULL, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, NULL);
  if (!mapping) {
    CloseHandle(file);
    return -1;
  }

  void * data = MapViewOfFile(mapping,
      writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
  if (!data) {
    CloseHandle(mapping);
    CloseHandle(file);
    return -1;
  }

  map->data = data;
  map->size = (size_t)length.QuadPart;
  map->file = file;
  map->mapping = mapping;
  return 0;
}

int gcu_mmap_sync(GCU_Mapped_File * map) {
  if (!map) {
    return -1;
  }
  if (!map->data || map->size == 0) {
    return 0;
  }
  // FlushViewOfFile queues the write; FlushFileBuffers is what waits for it.
  if (!FlushViewOfFile(map->data, 0)) {
    return -1;
  }
  if (FlushFileBuffers(map->file)) {
    return 0;
  }
  // FlushFileBuffers needs a handle opened for writing, and a read-only
  // mapping's is not.  Such a mapping has nothing to write back, which is the
  // success msync() reports for it on POSIX; the handle's access is the only
  // record of which kind this is, since GCU_Mapped_File does not keep one.
  return GetLastError() == ERROR_ACCESS_DENIED ? 0 : -1;
}

int gcu_mmap_close(GCU_Mapped_File * map) {
  if (!map || map->file == INVALID_HANDLE_VALUE || !map->file) {
    return -1;
  }

  bool ok = true;
  if (map->data) {
    ok = UnmapViewOfFile(map->data) && ok;
    ok = CloseHandle(map->mapping) && ok;
  }
  ok = CloseHandle(map->file) && ok;

  memset(map, 0, sizeof(*map));
  map->file = INVALID_HANDLE_VALUE;
  return ok ? 0 : -1;
}

#else

#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* See file_internal.h: a 32-bit off_t makes fstat() refuse a large file. */
_Static_assert(sizeof(off_t) == 8,
  "build with -D_FILE_OFFSET_BITS=64 (LFS_FLAGS in the Makefile)");

int gcu_mmap_open(GCU_Mapped_File * map, const char * path, bool writable) {
  if (!map || !path) {
    return -1;
  }
  memset(map, 0, sizeof(*map));
  map->fd = -1;

  int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;
  int fd = open(path, flags);
  if (fd < 0) {
    return -1;
  }

  struct stat info;
  if (fstat(fd, &info) != 0) {
    close(fd);
    return -1;
  }

  // Refuse anything that is not a regular file.  Mapping a directory fails
  // anyway, but mapping a character device or a socket can succeed and then
  // behave nothing like a file -- and /dev/zero in particular maps happily
  // and reports a size of 0, which would look like an empty file.
  if (!S_ISREG(info.st_mode)) {
    close(fd);
    return -1;
  }

  if (info.st_size == 0) {
    // mmap cannot map zero bytes.  Success with nothing mapped; see mmap.h.
    //
    // The descriptor stays open even though there is nothing to map. Closing
    // it here would leave this struct byte-identical to a *closed* mapping --
    // data NULL, size 0, fd -1 -- and gcu_mmap_close() could not then tell an
    // empty mapping it should accept from a stale one it should refuse. One
    // spare descriptor is worth two states that cannot be confused.
    map->fd = fd;
    return 0;
  }

  int protection = PROT_READ | (writable ? PROT_WRITE : 0);
  void * data = mmap(NULL, (size_t)info.st_size, protection, MAP_SHARED,
      fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return -1;
  }

  map->data = data;
  map->size = (size_t)info.st_size;
  map->fd = fd;
  return 0;
}

int gcu_mmap_sync(GCU_Mapped_File * map) {
  if (!map) {
    return -1;
  }
  if (!map->data || map->size == 0) {
    return 0;
  }
  // MS_SYNC, not MS_ASYNC: the point of calling this is to know the bytes
  // have landed, and MS_ASYNC returns before they have.
  return msync(map->data, map->size, MS_SYNC) == 0 ? 0 : -1;
}

int gcu_mmap_close(GCU_Mapped_File * map) {
  if (!map || map->fd < 0) {
    // fd < 0 is the only "already closed" test, because it is the one field
    // an empty mapping does not share with a closed one.
    return -1;
  }

  // An empty mapping has a descriptor and nothing mapped.
  int unmapped = map->data ? munmap(map->data, map->size) : 0;
  int closed = close(map->fd);

  memset(map, 0, sizeof(*map));
  map->fd = -1;
  return (unmapped == 0 && closed == 0) ? 0 : -1;
}

#endif
