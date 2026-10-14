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
 * Making, removing and walking directories.
 */

// mkdtemp, readdir and the rest of the POSIX directory calls, on the same
// terms as file.c and path.c.
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/path.h>

#include "file_internal.h"
#include "path_internal.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/** The trailing Xs mkdtemp() replaces. */
#define GCU_DIR_TEMPLATE "XXXXXX"

GCU_File_Result gcu_dir_create(const char * path) {
  if (!path || !*path) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  wchar_t * wide = gcu_path_internal_to_wide(NULL, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  BOOL ok = CreateDirectoryW(wide, NULL);
  DWORD err = ok ? 0 : GetLastError();
  gcu_allocator_free(NULL, wide);
  if (ok) {
    return GCU_FILE_OK;
  }
  if (err == ERROR_ALREADY_EXISTS) {
    return GCU_FILE_ERR_EXISTS;
  }
  if (err == ERROR_PATH_NOT_FOUND) {
    return GCU_FILE_ERR_NOT_FOUND;
  }
  return err == ERROR_ACCESS_DENIED ? GCU_FILE_ERR_ACCESS : GCU_FILE_ERR_IO;
#else
  // 0777 and not 0700: the umask is what narrows it, which is the platform's
  // own answer rather than one this library invents.  See the permissions
  // section of documentation/file.md.
  if (mkdir(path, 0777) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_dir_create_all(const char * path,
    const GCU_Allocator * allocator) {
  if (!path || !*path) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  // Walked forwards over a private copy, creating each prefix in turn.  A copy
  // because the separators are temporarily overwritten, and the caller's
  // string is not ours to write to.
  size_t len = strlen(path);
  char * work = (char *)gcu_allocator_malloc(allocator, len + 1);
  if (!work) {
    return GCU_FILE_ERR_OOM;
  }
  memcpy(work, path, len + 1);

  GCU_File_Result result = GCU_FILE_OK;
  // Started past the root, so that the root itself is never a thing to create:
  // "/" on POSIX, and "C:\" or a UNC share on Windows, already exist by
  // definition and answering EXISTS for them would be noise.
  size_t root = gcu_path_root_length(GCU_PATH_NATIVE, work);
  size_t i = root;

  for (;;) {
    while (i < len && !gcu_path_is_separator(GCU_PATH_NATIVE, work[i])) {
      ++i;
    }
    bool last = (i == len);
    char saved = work[i];
    work[i] = '\0';

    // A path that is nothing but its root reaches here with the root alone,
    // which is not a thing to create either.  POSIX forgives the attempt -
    // mkdir("/") is EEXIST, which the arm below accepts - but Windows answers
    // CreateDirectory("/") with ERROR_ACCESS_DENIED.
    if (i > root && *work) {
      GCU_File_Result made = gcu_dir_create(work);
      if (made == GCU_FILE_ERR_EXISTS) {
        // Already there is the outcome asked for - unless it is there as
        // something that is not a directory, which is a genuine collision and
        // must not be reported as success.
        if (!gcu_file_is_directory(work)) {
          result = GCU_FILE_ERR_EXISTS;
        }
      }
      else if (made != GCU_FILE_OK) {
        result = made;
      }
    }

    work[i] = saved;
    if (last || result != GCU_FILE_OK) {
      break;
    }
    // Past this separator, and past any that repeat after it.
    while (i < len && gcu_path_is_separator(GCU_PATH_NATIVE, work[i])) {
      ++i;
    }
  }

  gcu_allocator_free(allocator, work);
  return result;
}

GCU_File_Result gcu_dir_remove(const char * path) {
  if (!path || !*path) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  wchar_t * wide = gcu_path_internal_to_wide(NULL, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  BOOL ok = RemoveDirectoryW(wide);
  DWORD err = ok ? 0 : GetLastError();
  gcu_allocator_free(NULL, wide);
  if (ok) {
    return GCU_FILE_OK;
  }
  if (err == ERROR_DIR_NOT_EMPTY) {
    return GCU_FILE_ERR_NOT_EMPTY;
  }
  if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
    return GCU_FILE_ERR_NOT_FOUND;
  }
  return err == ERROR_ACCESS_DENIED ? GCU_FILE_ERR_ACCESS : GCU_FILE_ERR_IO;
#else
  if (rmdir(path) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}

void gcu_dir_free_path(const GCU_Allocator * allocator, char * path) {
  if (!allocator) {
    allocator = gcu_allocator_default();
  }
  gcu_allocator_free(allocator, path);
}

GCU_File_Result gcu_dir_temp_create(const char * parent, const char * prefix,
    const GCU_Allocator * allocator, char ** out_path) {
  if (!out_path) {
    return GCU_FILE_ERR_INVALID;
  }
  *out_path = NULL;
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  char * owned_parent = NULL;
  if (!parent) {
    if (gcu_path_temp_dir(allocator, &owned_parent) != GCU_PATH_OK) {
      return GCU_FILE_ERR_IO;
    }
    parent = owned_parent;
  }
  if (!prefix) {
    prefix = "tmp";
  }

  size_t stem = 0;
  if (gcu_path_join(GCU_PATH_NATIVE, parent, prefix, NULL, 0, &stem)
      != GCU_PATH_OK) {
    gcu_path_free(allocator, owned_parent);
    return GCU_FILE_ERR_INVALID;
  }
  char * path =
      (char *)gcu_allocator_malloc(allocator, stem + sizeof GCU_DIR_TEMPLATE);
  if (!path) {
    gcu_path_free(allocator, owned_parent);
    return GCU_FILE_ERR_OOM;
  }
  GCU_Path_Result joined =
      gcu_path_join(GCU_PATH_NATIVE, parent, prefix, path, stem + 1, NULL);
  gcu_path_free(allocator, owned_parent);
  if (joined != GCU_PATH_OK) {
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
  memcpy(path + stem, GCU_DIR_TEMPLATE, sizeof GCU_DIR_TEMPLATE);

#ifdef _WIN32
  /* A random name, as mkdtemp() chooses one - see
   * gcu_file_internal_randomize_tail() for why not _wmktemp_s - and
   * CreateDirectoryW is what makes taking it fail rather than succeed if
   * somebody got there first, in which case another name is tried. */
  wchar_t * wide = gcu_path_internal_to_wide(allocator, path);
  if (!wide) {
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_OOM;
  }
  wchar_t * tail = wide + wcslen(wide) - (sizeof GCU_DIR_TEMPLATE - 1);
  bool made = false;
  for (int attempt = 0; attempt < GCU_FILE_INTERNAL_TEMP_ATTEMPTS; ++attempt) {
    if (!gcu_file_internal_randomize_tail(tail)) {
      break;
    }
    if (CreateDirectoryW(wide, NULL)) {
      made = true;
      break;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      break;
    }
  }
  if (!made) {
    gcu_allocator_free(allocator, wide);
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
  char * chosen = gcu_path_internal_from_wide(allocator, wide);
  gcu_allocator_free(allocator, wide);
  if (!chosen) {
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_OOM;
  }
  gcu_allocator_free(allocator, path);
  path = chosen;
#else
  // mkdtemp() chooses the name and creates the directory in one step that
  // fails if the name is taken, and creates it 0700.
  if (!mkdtemp(path)) {
    GCU_File_Result failed = gcu_file_internal_from_errno(errno);
    gcu_allocator_free(allocator, path);
    return failed;
  }
#endif

  *out_path = path;
  return GCU_FILE_OK;
}

#ifndef _WIN32
/**
 * Ask the filesystem what an entry is, when the directory would not say.
 *
 * Does not follow a symbolic link:  a walk that resolves links is a walk that
 * can leave the tree it was asked about.  A failure leaves @p out_type as it
 * was, since not knowing is better represented as OTHER than as a guess.
 */
static void dir_type_by_asking(GCU_Dir * dir, const char * name,
    GCU_File_Type * out_type) {
  size_t need = 0;
  if (gcu_path_join(GCU_PATH_NATIVE, dir->path, name, NULL, 0, &need)
      != GCU_PATH_OK) {
    return;
  }
  char * full = (char *)gcu_allocator_malloc(dir->allocator, need + 1);
  if (!full) {
    return;
  }
  if (gcu_path_join(GCU_PATH_NATIVE, dir->path, name, full, need + 1, NULL)
      == GCU_PATH_OK) {
    GCU_File_Info info = GCU_FILE_INFO_INIT;
    if (gcu_file_stat_link(full, &info) == GCU_FILE_OK) {
      *out_type = info.type;
    }
  }
  gcu_allocator_free(dir->allocator, full);
}
#endif

GCU_File_Result gcu_dir_open(GCU_Dir * dir, const char * path,
    const GCU_Allocator * allocator) {
  if (!dir) {
    return GCU_FILE_ERR_INVALID;
  }
  // Zeroed before anything can fail, so that closing a handle from a failed
  // open is closing a handle that is safe to close.
  memset(dir, 0, sizeof *dir);
  if (!path || !*path) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }
  dir->allocator = allocator;

#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows.  FindFirstFileW needs a
   * wildcard appended, and it hands back the first entry immediately rather
   * than on the first read, so the entry is held until then. */
  size_t len = 0;
  if (gcu_path_join(GCU_PATH_NATIVE, path, "*", NULL, 0, &len)
      != GCU_PATH_OK) {
    return GCU_FILE_ERR_INVALID;
  }
  char * pattern = (char *)gcu_allocator_malloc(allocator, len + 1);
  if (!pattern) {
    return GCU_FILE_ERR_OOM;
  }
  if (gcu_path_join(GCU_PATH_NATIVE, path, "*", pattern, len + 1, NULL)
      != GCU_PATH_OK) {
    gcu_allocator_free(allocator, pattern);
    return GCU_FILE_ERR_IO;
  }
  wchar_t * wide = gcu_path_internal_to_wide(allocator, pattern);
  gcu_allocator_free(allocator, pattern);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  WIN32_FIND_DATAW * found =
      (WIN32_FIND_DATAW *)gcu_allocator_malloc(allocator, sizeof *found);
  if (!found) {
    gcu_allocator_free(allocator, wide);
    return GCU_FILE_ERR_OOM;
  }
  HANDLE h = FindFirstFileW(wide, found);
  gcu_allocator_free(allocator, wide);
  if (h == INVALID_HANDLE_VALUE) {
    gcu_allocator_free(allocator, found);
    // What POSIX reports as ENOENT or ENOTDIR, both of which are NOT_FOUND
    // there.  Opening a regular file as a directory is ERROR_DIRECTORY or,
    // with the "\*" appended, ERROR_PATH_NOT_FOUND depending on the version.
    switch (GetLastError()) {
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND:
      case ERROR_DIRECTORY:
      case ERROR_INVALID_NAME:
        return GCU_FILE_ERR_NOT_FOUND;
      case ERROR_ACCESS_DENIED:
        return GCU_FILE_ERR_ACCESS;
      default:
        return GCU_FILE_ERR_IO;
    }
  }
  dir->handle = h;
  dir->path = (char *)found;
  return GCU_FILE_OK;
#else
  DIR * d = opendir(path);
  if (!d) {
    return gcu_file_internal_from_errno(errno);
  }
  size_t len = strlen(path);
  dir->path = (char *)gcu_allocator_malloc(allocator, len + 1);
  if (!dir->path) {
    closedir(d);
    return GCU_FILE_ERR_OOM;
  }
  memcpy(dir->path, path, len + 1);
  dir->handle = d;
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_dir_read(GCU_Dir * dir, const char ** out_name,
    GCU_File_Type * out_type, bool * out_done) {
  if (!dir || !dir->handle || !out_name || !out_done) {
    return GCU_FILE_ERR_INVALID;
  }
  *out_done = false;

#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  WIN32_FIND_DATAW * found = (WIN32_FIND_DATAW *)dir->path;
  for (;;) {
    if (!found) {
      *out_done = true;
      return GCU_FILE_OK;
    }
    char * name = gcu_path_internal_from_wide(dir->allocator, found->cFileName);
    if (!name) {
      return GCU_FILE_ERR_OOM;
    }
    bool dot = (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
    DWORD attrs = found->dwFileAttributes;
    if (!FindNextFileW((HANDLE)dir->handle, found)) {
      gcu_allocator_free(dir->allocator, dir->path);
      dir->path = NULL;
      found = NULL;
    }
    if (dot) {
      gcu_allocator_free(dir->allocator, name);
      continue;
    }
    gcu_allocator_free(dir->allocator, dir->entry);
    dir->entry = name;
    if (out_type) {
      *out_type = (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
          ? GCU_FILE_TYPE_SYMLINK
          : ((attrs & FILE_ATTRIBUTE_DIRECTORY) ? GCU_FILE_TYPE_DIRECTORY
                                                : GCU_FILE_TYPE_REGULAR);
    }
    *out_name = dir->entry;
    return GCU_FILE_OK;
  }
#else
  for (;;) {
    errno = 0;
    struct dirent * entry = readdir((DIR *)dir->handle);
    if (!entry) {
      // readdir() reports both the end of the directory and a failure by
      // returning NULL, and errno is the only thing that tells them apart.
      if (errno != 0) {
        return GCU_FILE_ERR_IO;
      }
      *out_done = true;
      return GCU_FILE_OK;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (out_type) {
      // d_type is an optional extension.  XFS without ftype, and several
      // network filesystems, answer DT_UNKNOWN for everything - so taking it
      // at face value would report every entry as OTHER on those, and any
      // caller filtering for directories would find none.  Asking directly is
      // slower and is only done when there is no other answer.
      switch (entry->d_type) {
        case DT_REG: *out_type = GCU_FILE_TYPE_REGULAR; break;
        case DT_DIR: *out_type = GCU_FILE_TYPE_DIRECTORY; break;
        case DT_LNK: *out_type = GCU_FILE_TYPE_SYMLINK; break;
        default:
          *out_type = GCU_FILE_TYPE_OTHER;
          if (entry->d_type == DT_UNKNOWN) {
            dir_type_by_asking(dir, entry->d_name, out_type);
          }
          break;
      }
    }
    *out_name = entry->d_name;
    return GCU_FILE_OK;
  }
#endif
}

void gcu_dir_close(GCU_Dir * dir) {
  if (!dir || !dir->handle) {
    return;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  FindClose((HANDLE)dir->handle);
  gcu_allocator_free(dir->allocator, dir->path);
  gcu_allocator_free(dir->allocator, dir->entry);
#else
  closedir((DIR *)dir->handle);
  gcu_allocator_free(dir->allocator, dir->path);
#endif
  memset(dir, 0, sizeof *dir);
}
