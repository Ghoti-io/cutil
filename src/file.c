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
 * Reading a whole file, and replacing one atomically.
 *
 * The design and the reasoning behind each decision are recorded in
 * `documentation/file.md`.
 */

/* mkstemp() and fsync() are POSIX, and this library compiles with -std=c17,
 * which declares neither.  The feature-test macro goes at the very top,
 * before any header: glibc reads it when the first one is included and
 * ignores it afterwards.  The suite spells it this way in cutil's thread.c
 * and path.c. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
#include "file_internal.h"
#include "path_internal.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/** How much is read at a time before the buffer is grown. */
#define GCU_FILE_CHUNK ((size_t)8192)

/** The six characters mkstemp() replaces, plus the terminator. */
#define GCU_FILE_TEMPLATE "XXXXXX"

const char * gcu_file_result_string(GCU_File_Result result) {
  switch (result) {
    case GCU_FILE_OK:           return "ok";
    case GCU_FILE_ERR_INVALID:  return "invalid argument";
    case GCU_FILE_ERR_OOM:      return "out of memory";
    case GCU_FILE_ERR_LIMIT:    return "file is larger than the limit";
    case GCU_FILE_ERR_IO:       return "file operation failed";
    case GCU_FILE_ERR_NOT_FOUND: return "no such file or directory";
    case GCU_FILE_ERR_EXISTS:   return "already exists";
    case GCU_FILE_ERR_ACCESS:   return "permission denied";
    case GCU_FILE_ERR_NOT_EMPTY: return "directory is not empty";
    case GCU_FILE_RESULT_COUNT: break;
  }
  return "unknown";
}

void gcu_file_free(const GCU_Allocator * allocator, void * data) {
  if (!data) {
    return;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }
  gcu_allocator_free(allocator, data);
}

//////////////////////////////////////////////////////////////////////////////
// Opening
//////////////////////////////////////////////////////////////////////////////

/**
 * Open a path whose bytes are UTF-8.
 *
 * On Windows this goes through the wide entry point.  fopen() there takes the
 * path in the process code page, which cannot represent every name the
 * filesystem accepts, so a path merely passing through it opens a different
 * file or none at all.
 */
static FILE * file_open(const char * path, const char * mode,
    const GCU_Allocator * allocator) {
#ifdef _WIN32
  // The callers report a NULL through errno, so a conversion that failed
  // says why: otherwise errno is whatever it last was, and a refused
  // allocation reads as an I/O error.
  wchar_t * wide_path = gcu_path_internal_to_wide(allocator, path);
  if (!wide_path) {
    errno = ENOMEM;
    return NULL;
  }
  wchar_t * wide_mode = gcu_path_internal_to_wide(allocator, mode);
  if (!wide_mode) {
    gcu_allocator_free(allocator, wide_path);
    errno = ENOMEM;
    return NULL;
  }
  FILE * stream = _wfopen(wide_path, wide_mode);
  gcu_allocator_free(allocator, wide_path);
  gcu_allocator_free(allocator, wide_mode);
  return stream;
#else
  (void)allocator;
  return fopen(path, mode);
#endif
}

/** Remove a path whose bytes are UTF-8. */
static void file_remove(const char * path, const GCU_Allocator * allocator) {
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  wchar_t * wide = gcu_path_internal_to_wide(allocator, path);
  if (wide) {
    _wremove(wide);
    gcu_allocator_free(allocator, wide);
  }
#else
  (void)allocator;
  remove(path);
#endif
}

//////////////////////////////////////////////////////////////////////////////
// Reading
//////////////////////////////////////////////////////////////////////////////

GCU_File_Result gcu_file_read(const char * path, size_t max_bytes,
    const GCU_Allocator * allocator, void ** out_data, size_t * out_len) {
  if (!path || !out_data || !out_len) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  FILE * stream = file_open(path, "rb", allocator);
  if (!stream) {
    // A path that is not there is told apart from a file that would not open,
    // so that a caller does not have to ask the filesystem a second question
    // afterwards to find out which it was - which is a race as well as a
    // duplicated query.
    return gcu_file_internal_from_errno(errno);
  }

  // One byte over the limit is enough to know the file exceeds it, so a
  // limited read never holds more than it is allowed to plus that byte.  The
  // extra byte on top is the NUL, which is not part of the content.
  size_t ceiling = 0;
  if (max_bytes != GCU_FILE_UNLIMITED) {
    ceiling = max_bytes + 2;
  }

  size_t capacity = GCU_FILE_CHUNK;
  if (ceiling && ceiling < capacity) {
    capacity = ceiling;
  }

  char * buffer = (char *)gcu_allocator_malloc(allocator, capacity);
  if (!buffer) {
    fclose(stream);
    return GCU_FILE_ERR_OOM;
  }

  size_t used = 0;
  GCU_File_Result result = GCU_FILE_OK;

  for (;;) {
    // The file is read rather than sized, because a pipe, a character device
    // and anything under /proc all report a size of zero and would come back
    // empty from a seek-and-tell.
    if (used + 1 >= capacity) {
      size_t wanted = capacity * 2;
      if (ceiling && wanted > ceiling) {
        wanted = ceiling;
      }
      if (wanted <= capacity) {
        // Cannot grow, and the limit is what stopped it.
        result = GCU_FILE_ERR_LIMIT;
        break;
      }
      char * grown = (char *)gcu_allocator_realloc(allocator, buffer, wanted);
      if (!grown) {
        result = GCU_FILE_ERR_OOM;
        break;
      }
      buffer = grown;
      capacity = wanted;
    }

    size_t room = capacity - used - 1;
    size_t got = fread(buffer + used, 1, room, stream);
    used += got;

    if (max_bytes != GCU_FILE_UNLIMITED && used > max_bytes) {
      result = GCU_FILE_ERR_LIMIT;
      break;
    }
    if (got < room) {
      if (ferror(stream)) {
        result = GCU_FILE_ERR_IO;
      }
      break;
    }
  }

  fclose(stream);

  if (result != GCU_FILE_OK) {
    gcu_allocator_free(allocator, buffer);
    return result;
  }

  // Not counted in the length: it is there so that a text caller can use the
  // result as a C string without copying it.
  buffer[used] = '\0';
  *out_data = buffer;
  *out_len = used;
  return GCU_FILE_OK;
}

//////////////////////////////////////////////////////////////////////////////
// Temporary files
//////////////////////////////////////////////////////////////////////////////

FILE * gcu_file_temp_stream(const GCU_File_Temp * temp) {
  return temp ? temp->stream : NULL;
}

const char * gcu_file_temp_path(const GCU_File_Temp * temp) {
  return temp ? temp->path : NULL;
}

GCU_File_Result gcu_file_temp_create(GCU_File_Temp * temp, const char * dir,
    const char * prefix, const GCU_Allocator * allocator) {
  if (!temp) {
    return GCU_FILE_ERR_INVALID;
  }
  // Zeroed before anything can fail, so that a caller who aborts after a
  // failed create is aborting a handle that is safe to abort.
  memset(temp, 0, sizeof *temp);
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  char * owned_dir = NULL;
  if (!dir) {
    if (gcu_path_temp_dir(allocator, &owned_dir) != GCU_PATH_OK) {
      return GCU_FILE_ERR_IO;
    }
    dir = owned_dir;
  }
  if (!prefix) {
    prefix = "tmp";
  }

  size_t stem = 0;
  GCU_Path_Result joined =
      gcu_path_join(GCU_PATH_NATIVE, dir, prefix, NULL, 0, &stem);
  if (joined != GCU_PATH_OK) {
    gcu_path_free(allocator, owned_dir);
    return GCU_FILE_ERR_INVALID;
  }

  size_t total = stem + sizeof GCU_FILE_TEMPLATE;
  char * path = (char *)gcu_allocator_malloc(allocator, total);
  if (!path) {
    gcu_path_free(allocator, owned_dir);
    return GCU_FILE_ERR_OOM;
  }
  joined = gcu_path_join(GCU_PATH_NATIVE, dir, prefix, path, stem + 1, NULL);
  gcu_path_free(allocator, owned_dir);
  if (joined != GCU_PATH_OK) {
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
  memcpy(path + stem, GCU_FILE_TEMPLATE, sizeof GCU_FILE_TEMPLATE);

#ifdef _WIN32
  /* _mktemp_s only chooses the name; CREATE_NEW is what makes taking it a
   * single step that fails rather than following something already there.
   *
   * CreateFileW rather than _wopen for the sharing mode.  _wopen shares read
   * and write but not delete, so while the temporary was open nothing could
   * rename over it or remove it - which POSIX allows of any open file, and
   * which a caller does when it writes the final file atomically to a path
   * it is still holding (cjelly's capture test did exactly that, and
   * MoveFileEx failed with a sharing violation). */
  wchar_t * wide = gcu_path_internal_to_wide(allocator, path);
  if (!wide) {
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_OOM;
  }
  // A fresh random name per attempt, retried only when the name was taken:
  // mkstemp()'s loop, which _wmktemp_s did not provide.
  wchar_t * tail = wide + wcslen(wide) - (sizeof GCU_FILE_TEMPLATE - 1);
  HANDLE created = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < GCU_FILE_INTERNAL_TEMP_ATTEMPTS; ++attempt) {
    if (!gcu_file_internal_randomize_tail(tail)) {
      break;
    }
    created = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (created != INVALID_HANDLE_VALUE
        || (GetLastError() != ERROR_FILE_EXISTS
            && GetLastError() != ERROR_ALREADY_EXISTS)) {
      break;
    }
  }
  int fd = created == INVALID_HANDLE_VALUE ? -1
      : _open_osfhandle((intptr_t)created, _O_BINARY | _O_RDWR);
  if (fd < 0) {
    if (created != INVALID_HANDLE_VALUE) {
      CloseHandle(created);
      DeleteFileW(wide);
    }
    gcu_allocator_free(allocator, wide);
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
  char * chosen = gcu_path_internal_from_wide(allocator, wide);
  gcu_allocator_free(allocator, wide);
  if (!chosen) {
    _close(fd);
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_OOM;
  }
  gcu_allocator_free(allocator, path);
  path = chosen;
  FILE * stream = _fdopen(fd, "w+b");
  if (!stream) {
    _close(fd);
    file_remove(path, allocator);
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
#else
  // mkstemp() chooses the name and creates the file in one step that fails if
  // the name is taken, and opens it 0600.  Choosing a name and then opening
  // it is the classic way to follow somebody else's symbolic link.
  int fd = mkstemp(path);
  if (fd < 0) {
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
  FILE * stream = fdopen(fd, "w+b");
  if (!stream) {
    // The file exists by now, so closing the descriptor is not enough.  This
    // is the branch that the two hand-written copies in `text` disagree
    // about, and the reason this is one function rather than several.
    close(fd);
    file_remove(path, allocator);
    gcu_allocator_free(allocator, path);
    return GCU_FILE_ERR_IO;
  }
#endif

  temp->path = path;
  temp->stream = stream;
  temp->allocator = allocator;
  return GCU_FILE_OK;
}

void gcu_file_temp_abort(GCU_File_Temp * temp) {
  if (!temp || !temp->path) {
    return;
  }
  if (temp->stream) {
    fclose(temp->stream);
  }
  file_remove(temp->path, temp->allocator);
  gcu_allocator_free(temp->allocator, temp->path);
  memset(temp, 0, sizeof *temp);
}

/**
 * Ask the operating system to commit a stream's bytes.
 *
 * fflush() only moves them out of stdio and into the kernel.  Without this
 * step the rename can be recorded while the content behind it is not, which
 * leaves a file that exists, has the right name, and is empty.
 */
static bool file_sync_stream(FILE * stream) {
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  return _commit(_fileno(stream)) == 0;
#else
  return fsync(fileno(stream)) == 0;
#endif
}

/**
 * Ask the operating system to commit the directory entry itself.
 *
 * Best effort by design: several filesystems refuse fsync on a directory, and
 * failing a replacement that has otherwise completed would be worse than the
 * weaker promise.  See `documentation/file.md`.
 */
static void file_sync_directory(const char * path,
    const GCU_Allocator * allocator) {
#ifdef _WIN32
  /* Windows has no directory handle to commit; MoveFileEx with
   * MOVEFILE_WRITE_THROUGH already carries the request. */
  (void)path;
  (void)allocator;
#else
  size_t need = 0;
  if (gcu_path_dirname(GCU_PATH_NATIVE, path, NULL, 0, &need)
      != GCU_PATH_OK) {
    return;
  }
  char * dir = (char *)gcu_allocator_malloc(allocator, need + 1);
  if (!dir) {
    return;
  }
  if (gcu_path_dirname(GCU_PATH_NATIVE, path, dir, need + 1, NULL)
      == GCU_PATH_OK) {
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) {
      (void)fsync(fd);
      close(fd);
    }
  }
  gcu_allocator_free(allocator, dir);
#endif
}

/**
 * Ask the operating system what permissions a new file here would get.
 *
 * By creating one and looking, rather than by reading the umask and combining
 * it with the directory's mode.  Two reasons.  The umask is process-global and
 * can only be read by setting it - `umask(0)` followed by putting it back -
 * which is a window in which every other thread creating a file gets 0666.
 * And the answer is not a function of the umask anyway:  a default ACL on the
 * containing directory overrides the umask entirely, so a computed answer is
 * wrong on exactly the systems that went to the trouble of configuring one.
 *
 * The probe is created beside the temporary file, so it lands in the same
 * directory under the same ACL.  O_EXCL because a name in a directory somebody
 * else can write to is not a file until it has been created as one.
 *
 * @return true and the mode, or false if the probe could not be made.
 */
static bool file_probe_new_file_mode(const char * temp_path,
    const GCU_Allocator * allocator, mode_t * out_mode) {
  size_t len = strlen(temp_path);
  char * probe = (char *)gcu_allocator_malloc(allocator, len + 2);
  if (!probe) {
    return false;
  }
  memcpy(probe, temp_path, len);
  // The temporary's own name is unique already, so a suffix of it is a name
  // nothing else in this directory holds.
  probe[len] = 'p';
  probe[len + 1] = '\0';

  bool found = false;
  int fd = open(probe, O_CREAT | O_EXCL | O_WRONLY, 0666);
  if (fd >= 0) {
    struct stat info;
    if (fstat(fd, &info) == 0) {
      *out_mode = info.st_mode & 07777;
      found = true;
    }
    close(fd);
    (void)remove(probe);
  }
  gcu_allocator_free(allocator, probe);
  return found;
}

/**
 * Put the requested permissions on the temporary file, before it is renamed.
 *
 * Before, and not after, because the rename is the moment the file becomes
 * reachable under a name anybody else knows.  Doing it afterwards would leave
 * a window in which the destination exists with the temporary file's own
 * owner-only permissions, which is a different file from the one that was
 * asked for.
 */
static bool file_apply_perms(FILE * stream, const char * temp_path,
    const char * dest, GCU_File_Perms perms, const GCU_Allocator * allocator) {
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows.  _wchmod moves only the
   * read-only attribute, and the access control entries that actually decide
   * this are inherited from the destination's directory at creation time.
   * PRESERVE and DEFAULT therefore both already hold; PRIVATE does not, and
   * making it hold needs SetSecurityInfo and a constructed DACL. */
  (void)stream;
  (void)temp_path;
  (void)dest;
  (void)perms;
  (void)allocator;
  return true;
#else
  if (perms == GCU_FILE_PERMS_PRIVATE) {
    // Which is what mkstemp() already made it.
    return true;
  }
  if (perms != GCU_FILE_PERMS_DEFAULT && perms != GCU_FILE_PERMS_PRESERVE) {
    return false;
  }

  mode_t mode = 0;
  bool known = false;
  if (perms == GCU_FILE_PERMS_PRESERVE) {
    struct stat info;
    if (stat(dest, &info) == 0) {
      mode = info.st_mode & 07777;
      known = true;
    }
    // Otherwise there is no destination to preserve, so this is a creation
    // after all and the answer is the same one DEFAULT wants.
  }
  if (!known && !file_probe_new_file_mode(temp_path, allocator, &mode)) {
    return false;
  }
  return fchmod(fileno(stream), mode) == 0;
#endif
}

#ifdef _WIN32
/**
 * Rename @p source over @p dest with POSIX semantics: the destination may be
 * open, as it may be for rename(2).
 *
 * MoveFileEx refuses to replace a file that anything holds open, even with
 * delete access shared, where POSIX replaces the name and leaves the holder
 * its now-anonymous file.  Windows 10 1709 and later do the POSIX thing when
 * asked through FileRenameInfoEx.  The structure is declared here because
 * mingw-w64 declares its Flags member only for a Windows 10 build target,
 * and its layout is fixed by the kernel: a 4-byte flags word where the older
 * form has a BOOLEAN, then the same three fields.
 */
typedef struct {
  DWORD Flags;
  HANDLE RootDirectory;
  DWORD FileNameLength;
  WCHAR FileName[1];
} GCU_File_Rename_Info;

#define GCU_FILE_RENAME_INFO_EX ((FILE_INFO_BY_HANDLE_CLASS)22)
#define GCU_FILE_RENAME_REPLACE_IF_EXISTS 0x1u
#define GCU_FILE_RENAME_POSIX_SEMANTICS 0x2u

static bool file_replace_posix(const wchar_t * source, const wchar_t * dest,
    const GCU_Allocator * allocator) {
  size_t name_bytes = wcslen(dest) * sizeof(wchar_t);
  size_t size = sizeof(GCU_File_Rename_Info) + name_bytes;
  GCU_File_Rename_Info * info =
      (GCU_File_Rename_Info *)gcu_allocator_malloc(allocator, size);
  if (!info) {
    return false;
  }
  memset(info, 0, size);
  info->Flags = GCU_FILE_RENAME_REPLACE_IF_EXISTS
      | GCU_FILE_RENAME_POSIX_SEMANTICS;
  info->FileNameLength = (DWORD)name_bytes;
  memcpy(info->FileName, dest, name_bytes);

  HANDLE handle = CreateFileW(source, DELETE | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  bool moved = false;
  if (handle != INVALID_HANDLE_VALUE) {
    moved = SetFileInformationByHandle(handle, GCU_FILE_RENAME_INFO_EX, info,
        (DWORD)size) != 0;
    CloseHandle(handle);
  }
  gcu_allocator_free(allocator, info);
  return moved;
}
#endif

/** Move @p source over @p dest, replacing it. */
static bool file_replace(const char * source, const char * dest,
    GCU_File_Sync sync, const GCU_Allocator * allocator) {
#ifdef _WIN32
  /* rename() on Windows refuses an existing destination; MoveFileEx is the
   * call that replaces one, and it is atomic for a same-volume move.  When
   * the destination is open it refuses that too, and the POSIX-semantics
   * rename is tried before giving up. */
  wchar_t * wide_source = gcu_path_internal_to_wide(allocator, source);
  wchar_t * wide_dest = gcu_path_internal_to_wide(allocator, dest);
  bool moved = false;
  if (wide_source && wide_dest) {
    DWORD flags = MOVEFILE_REPLACE_EXISTING;
    if (sync == GCU_FILE_SYNC_FULL) {
      flags |= MOVEFILE_WRITE_THROUGH;
    }
    moved = MoveFileExW(wide_source, wide_dest, flags) != 0;
    if (!moved) {
      DWORD why = GetLastError();
      if (why == ERROR_ACCESS_DENIED || why == ERROR_SHARING_VIOLATION) {
        moved = file_replace_posix(wide_source, wide_dest, allocator);
        if (!moved) {
          SetLastError(why);
        }
      }
    }
  }
  gcu_allocator_free(allocator, wide_source);
  gcu_allocator_free(allocator, wide_dest);
  return moved;
#else
  (void)sync;
  (void)allocator;
  return rename(source, dest) == 0;
#endif
}

GCU_File_Result gcu_file_temp_commit(GCU_File_Temp * temp, const char * dest,
    GCU_File_Sync sync, GCU_File_Perms perms) {
  if (!temp || !temp->path || !temp->stream || !dest) {
    return GCU_FILE_ERR_INVALID;
  }

  const GCU_Allocator * allocator = temp->allocator;
  GCU_File_Result result = GCU_FILE_OK;

  if (fflush(temp->stream) != 0) {
    result = GCU_FILE_ERR_IO;
  }
  // These are the bytes, so a refusal here is reported.
  if (result == GCU_FILE_OK && sync == GCU_FILE_SYNC_FULL
      && !file_sync_stream(temp->stream)) {
    result = GCU_FILE_ERR_IO;
  }
  // While there is still a descriptor to do it through, and before the rename
  // publishes the file under a name somebody else can open.  A file with the
  // wrong permissions is the wrong file, so failing here fails the call.
  if (result == GCU_FILE_OK
      && !file_apply_perms(temp->stream, temp->path, dest, perms, allocator)) {
    result = GCU_FILE_ERR_IO;
  }
  // Closed before the rename: Windows will not move a file that is open.
  if (fclose(temp->stream) != 0) {
    result = GCU_FILE_ERR_IO;
  }
  temp->stream = NULL;

  bool moved = false;
  if (result == GCU_FILE_OK) {
    moved = file_replace(temp->path, dest, sync, allocator);
    if (!moved) {
      result = GCU_FILE_ERR_IO;
    }
  }

  if (moved) {
    if (sync == GCU_FILE_SYNC_FULL) {
      file_sync_directory(dest, allocator);
    }
  }
  else {
    // Nothing reached the destination, so the destination is untouched and
    // the temporary file is litter.
    file_remove(temp->path, allocator);
  }

  gcu_allocator_free(allocator, temp->path);
  memset(temp, 0, sizeof *temp);
  return result;
}

GCU_File_Result gcu_file_open(GCU_File_Handle * handle, const char * path,
    GCU_File_Open_Mode mode, GCU_File_Perms perms,
    const GCU_Allocator * allocator) {
  if (!handle) {
    return GCU_FILE_ERR_INVALID;
  }
  memset(handle, 0, sizeof *handle);
  if (!path) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  int flags;
  const char * stdio_mode;
  switch (mode) {
    case GCU_FILE_OPEN_READ:
      flags = O_RDONLY;
      stdio_mode = "rb";
      break;
    case GCU_FILE_OPEN_WRITE:
      flags = O_WRONLY | O_CREAT | O_TRUNC;
      stdio_mode = "wb";
      break;
    case GCU_FILE_OPEN_APPEND:
      flags = O_WRONLY | O_CREAT | O_APPEND;
      stdio_mode = "ab";
      break;
    case GCU_FILE_OPEN_UPDATE:
      flags = O_RDWR;
      stdio_mode = "r+b";
      break;
    default:
      return GCU_FILE_ERR_INVALID;
  }

  // Unlike the temporary-file path, no probe is needed to honour the umask
  // here: open() applies it at creation, so asking for 0666 gets whatever the
  // platform would have given any other new file, ACL included.  The probe
  // exists over there only because mkstemp() forces 0600 and the mode has to
  // be recovered afterwards.
  int create_mode = (perms == GCU_FILE_PERMS_PRIVATE) ? 0600 : 0666;

#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  (void)flags;
  wchar_t * wide = gcu_path_internal_to_wide(allocator, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  wchar_t wide_mode[8];
  size_t mi = 0;
  for (const char * m = stdio_mode; *m && mi < 7; ++m) {
    wide_mode[mi++] = (wchar_t)*m;
  }
  wide_mode[mi] = L'\0';
  FILE * stream = _wfopen(wide, wide_mode);
  gcu_allocator_free(allocator, wide);
  if (!stream) {
    return gcu_file_internal_from_errno(errno);
  }
  (void)create_mode;
#else
  int fd = open(path, flags, create_mode);
  if (fd < 0) {
    return gcu_file_internal_from_errno(errno);
  }
  FILE * stream = fdopen(fd, stdio_mode);
  if (!stream) {
    close(fd);
    return GCU_FILE_ERR_IO;
  }
#endif

  handle->stream = stream;
  handle->allocator = allocator;
  return GCU_FILE_OK;
}

GCU_File_Result gcu_file_close(GCU_File_Handle * handle) {
  if (!handle || !handle->stream) {
    return GCU_FILE_OK;
  }
  // Reported, because this is where buffered output finally reaches the
  // operating system and therefore where a full disk is discovered.
  int closed = fclose(handle->stream);
  memset(handle, 0, sizeof *handle);
  return closed == 0 ? GCU_FILE_OK : GCU_FILE_ERR_IO;
}

GCU_File_Result gcu_file_read_bytes(GCU_File_Handle * handle, void * buffer,
    size_t size, size_t * out_got) {
  if (!handle || !handle->stream || (!buffer && size) || !out_got) {
    return GCU_FILE_ERR_INVALID;
  }
  *out_got = fread(buffer, 1, size, handle->stream);
  // A short read is the end of the file, not a failure; only ferror says a
  // failure happened.
  if (*out_got != size && ferror(handle->stream)) {
    return GCU_FILE_ERR_IO;
  }
  return GCU_FILE_OK;
}

GCU_File_Result gcu_file_write_bytes(GCU_File_Handle * handle,
    const void * data, size_t len) {
  if (!handle || !handle->stream || (!data && len)) {
    return GCU_FILE_ERR_INVALID;
  }
  if (len && fwrite(data, 1, len, handle->stream) != len) {
    // Unlike a read, there is no benign reason for a short write.
    return GCU_FILE_ERR_IO;
  }
  return GCU_FILE_OK;
}

GCU_File_Result gcu_file_seek(GCU_File_Handle * handle, int64_t offset,
    GCU_File_Seek_From from) {
  if (!handle || !handle->stream) {
    return GCU_FILE_ERR_INVALID;
  }
  int whence;
  switch (from) {
    case GCU_FILE_SEEK_SET: whence = SEEK_SET; break;
    case GCU_FILE_SEEK_CUR: whence = SEEK_CUR; break;
    case GCU_FILE_SEEK_END: whence = SEEK_END; break;
    default: return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows.  fseek takes a long,
   * which is 32 bits here, so a file over 2 GB needs the 64-bit form. */
  if (_fseeki64(handle->stream, offset, whence) != 0) {
    return GCU_FILE_ERR_IO;
  }
#else
  if (fseeko(handle->stream, (off_t)offset, whence) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
#endif
  return GCU_FILE_OK;
}

GCU_File_Result gcu_file_tell(GCU_File_Handle * handle, int64_t * out_offset) {
  if (!handle || !handle->stream || !out_offset) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  __int64 where = _ftelli64(handle->stream);
#else
  off_t where = ftello(handle->stream);
#endif
  if (where < 0) {
    return GCU_FILE_ERR_IO;
  }
  *out_offset = (int64_t)where;
  return GCU_FILE_OK;
}

bool gcu_file_eof(const GCU_File_Handle * handle) {
  return handle && handle->stream && feof(handle->stream);
}

GCU_File_Result gcu_file_flush(GCU_File_Handle * handle) {
  if (!handle || !handle->stream) {
    return GCU_FILE_ERR_INVALID;
  }
  return fflush(handle->stream) == 0 ? GCU_FILE_OK : GCU_FILE_ERR_IO;
}

GCU_File_Result gcu_file_sync(GCU_File_Handle * handle) {
  if (!handle || !handle->stream) {
    return GCU_FILE_ERR_INVALID;
  }
  // Flushed first, because fsync commits what the operating system has and
  // stdio may still be holding the last of it.
  if (fflush(handle->stream) != 0) {
    return GCU_FILE_ERR_IO;
  }
  return file_sync_stream(handle->stream) ? GCU_FILE_OK : GCU_FILE_ERR_IO;
}

/** Fill in @p out from a platform stat buffer. */
#ifdef _WIN32
static void file_info_from_find(GCU_File_Info * out,
    const WIN32_FILE_ATTRIBUTE_DATA * data) {
  /* TODO(windows): never compiled or run on Windows. */
  if (data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    out->type = GCU_FILE_TYPE_SYMLINK;
  }
  else if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    out->type = GCU_FILE_TYPE_DIRECTORY;
  }
  else {
    out->type = GCU_FILE_TYPE_REGULAR;
  }
  out->size = ((uint64_t)data->nFileSizeHigh << 32) | data->nFileSizeLow;
  /* FILETIME counts 100-nanosecond ticks from 1601-01-01; the Unix epoch is
   * 11644473600 seconds later. */
  uint64_t ticks = ((uint64_t)data->ftLastWriteTime.dwHighDateTime << 32)
      | data->ftLastWriteTime.dwLowDateTime;
  out->mtime_ns = (int64_t)(ticks - 116444736000000000ULL) * 100;
}
#else
static void file_info_from_stat(GCU_File_Info * out, const struct stat * info) {
  if (S_ISDIR(info->st_mode)) {
    out->type = GCU_FILE_TYPE_DIRECTORY;
  }
  else if (S_ISLNK(info->st_mode)) {
    out->type = GCU_FILE_TYPE_SYMLINK;
  }
  else if (S_ISREG(info->st_mode)) {
    out->type = GCU_FILE_TYPE_REGULAR;
  }
  else {
    out->type = GCU_FILE_TYPE_OTHER;
  }
  out->size = (uint64_t)info->st_size;
  // st_mtim where it exists, st_mtime where it does not. Whole seconds is a
  // real answer on filesystems that keep nothing finer; the nanoseconds are
  // reported when the platform has them, not invented when it does not.
#if defined(__APPLE__)
  out->mtime_ns = (int64_t)info->st_mtimespec.tv_sec * 1000000000
      + info->st_mtimespec.tv_nsec;
#elif defined(st_mtime) || defined(_POSIX_C_SOURCE)
  out->mtime_ns = (int64_t)info->st_mtim.tv_sec * 1000000000
      + info->st_mtim.tv_nsec;
#else
  out->mtime_ns = (int64_t)info->st_mtime * 1000000000;
#endif
}
#endif

static GCU_File_Result file_stat_common(const char * path, GCU_File_Info * out,
    bool follow) {
  if (!path || !out) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows.  GetFileAttributesExW
   * always follows a reparse point for size and time, so the link-only query
   * reports the type from the attribute bits and the target's size. */
  (void)follow;
  wchar_t * wide = gcu_path_internal_to_wide(NULL, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  WIN32_FILE_ATTRIBUTE_DATA data;
  BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
  gcu_allocator_free(NULL, wide);
  if (!ok) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      return GCU_FILE_ERR_NOT_FOUND;
    }
    return err == ERROR_ACCESS_DENIED ? GCU_FILE_ERR_ACCESS : GCU_FILE_ERR_IO;
  }
  file_info_from_find(out, &data);
  return GCU_FILE_OK;
#else
  struct stat info;
  int rc = follow ? stat(path, &info) : lstat(path, &info);
  if (rc != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  file_info_from_stat(out, &info);
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_file_stat(const char * path, GCU_File_Info * out) {
  return file_stat_common(path, out, true);
}

GCU_File_Result gcu_file_stat_link(const char * path, GCU_File_Info * out) {
  return file_stat_common(path, out, false);
}

bool gcu_file_exists(const char * path) {
  GCU_File_Info info;
  return gcu_file_stat(path, &info) == GCU_FILE_OK;
}

bool gcu_file_is_directory(const char * path) {
  GCU_File_Info info;
  return gcu_file_stat(path, &info) == GCU_FILE_OK
      && info.type == GCU_FILE_TYPE_DIRECTORY;
}

GCU_File_Result gcu_file_remove(const char * path) {
  if (!path) {
    return GCU_FILE_ERR_INVALID;
  }
  // Refused rather than delegated: remove() on POSIX unlinks a file and also
  // removes an empty directory, so a caller who passed the wrong variable
  // would silently get the other operation.
  GCU_File_Info info;
  GCU_File_Result looked = gcu_file_stat_link(path, &info);
  if (looked != GCU_FILE_OK) {
    return looked;
  }
  if (info.type == GCU_FILE_TYPE_DIRECTORY) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  wchar_t * wide = gcu_path_internal_to_wide(NULL, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  BOOL ok = DeleteFileW(wide);
  gcu_allocator_free(NULL, wide);
  return ok ? GCU_FILE_OK : GCU_FILE_ERR_IO;
#else
  if (unlink(path) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_file_rename(const char * from, const char * to) {
  if (!from || !to) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  if (!file_replace(from, to, GCU_FILE_SYNC_NONE, NULL)) {
    return GCU_FILE_ERR_IO;
  }
  return GCU_FILE_OK;
#else
  if (rename(from, to) != 0) {
    // EXDEV is the cross-filesystem case, and it is reported rather than
    // quietly turned into a copy: a caller who asked for a rename is usually
    // relying on it being one operation.
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_file_copy(const char * from, const char * to,
    GCU_File_Sync sync, GCU_File_Perms perms,
    const GCU_Allocator * allocator) {
  if (!from || !to) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  FILE * source = file_open(from, "rb", allocator);
  if (!source) {
    return gcu_file_internal_from_errno(errno);
  }

  size_t need = 0;
  if (gcu_path_dirname(GCU_PATH_NATIVE, to, NULL, 0, &need) != GCU_PATH_OK) {
    fclose(source);
    return GCU_FILE_ERR_INVALID;
  }
  char * dir = (char *)gcu_allocator_malloc(allocator, need + 1);
  if (!dir) {
    fclose(source);
    return GCU_FILE_ERR_OOM;
  }
  if (gcu_path_dirname(GCU_PATH_NATIVE, to, dir, need + 1, NULL)
      != GCU_PATH_OK) {
    gcu_allocator_free(allocator, dir);
    fclose(source);
    return GCU_FILE_ERR_IO;
  }

  GCU_File_Temp temp;
  GCU_File_Result result = gcu_file_temp_create(&temp, dir,
      gcu_path_basename(GCU_PATH_NATIVE, to), allocator);
  gcu_allocator_free(allocator, dir);
  if (result != GCU_FILE_OK) {
    fclose(source);
    return result;
  }

  // Streamed rather than read whole, so that copying a file larger than
  // memory is a copy rather than an out-of-memory error.
  char chunk[GCU_FILE_CHUNK];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, source)) > 0) {
    if (fwrite(chunk, 1, got, gcu_file_temp_stream(&temp)) != got) {
      result = GCU_FILE_ERR_IO;
      break;
    }
  }
  if (result == GCU_FILE_OK && ferror(source)) {
    result = GCU_FILE_ERR_IO;
  }
  fclose(source);

  if (result != GCU_FILE_OK) {
    gcu_file_temp_abort(&temp);
    return result;
  }
  return gcu_file_temp_commit(&temp, to, sync, perms);
}

GCU_File_Result gcu_file_write_atomic(const char * path, const void * data,
    size_t len, GCU_File_Sync sync, GCU_File_Perms perms,
    const GCU_Allocator * allocator) {
  if (!path || (!data && len)) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }

  // The temporary file goes in the destination's own directory, because a
  // rename between filesystems is a copy and a copy is not atomic.
  size_t need = 0;
  if (gcu_path_dirname(GCU_PATH_NATIVE, path, NULL, 0, &need)
      != GCU_PATH_OK) {
    return GCU_FILE_ERR_INVALID;
  }
  char * dir = (char *)gcu_allocator_malloc(allocator, need + 1);
  if (!dir) {
    return GCU_FILE_ERR_OOM;
  }
  if (gcu_path_dirname(GCU_PATH_NATIVE, path, dir, need + 1, NULL)
      != GCU_PATH_OK) {
    gcu_allocator_free(allocator, dir);
    return GCU_FILE_ERR_IO;
  }

  GCU_File_Temp temp;
  GCU_File_Result result =
      gcu_file_temp_create(&temp, dir, gcu_path_basename(GCU_PATH_NATIVE,
          path), allocator);
  gcu_allocator_free(allocator, dir);
  if (result != GCU_FILE_OK) {
    return result;
  }

  if (len && fwrite(data, 1, len, gcu_file_temp_stream(&temp)) != len) {
    gcu_file_temp_abort(&temp);
    return GCU_FILE_ERR_IO;
  }

  return gcu_file_temp_commit(&temp, path, sync, perms);
}

#ifdef _WIN32
/** Map a Win32 error from the link and mode calls onto a file result. */
static GCU_File_Result file_result_from_win32(DWORD err) {
  /* TODO(windows): never compiled or run on Windows. */
  if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
    return GCU_FILE_ERR_NOT_FOUND;
  }
  if (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD) {
    return GCU_FILE_ERR_ACCESS;
  }
  if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
    return GCU_FILE_ERR_EXISTS;
  }
  return GCU_FILE_ERR_IO;
}
#endif

GCU_File_Result gcu_file_identity(const char * path, GCU_File_Identity * out) {
  if (!path || !out) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows.  A Unix mode does not
   * apply, so mode stays 0; the file index is what a hard link is compared
   * by, and that does exist here. */
  wchar_t * wide = gcu_path_internal_to_wide(NULL, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  DWORD open_err = GetLastError();
  gcu_allocator_free(NULL, wide);
  if (handle == INVALID_HANDLE_VALUE) {
    return file_result_from_win32(open_err);
  }
  BY_HANDLE_FILE_INFORMATION info;
  BOOL ok = GetFileInformationByHandle(handle, &info);
  DWORD err = GetLastError();
  CloseHandle(handle);
  if (!ok) {
    return file_result_from_win32(err);
  }
  out->device = info.dwVolumeSerialNumber;
  out->inode = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
  out->links = info.nNumberOfLinks;
  out->mode = 0;
  return GCU_FILE_OK;
#else
  struct stat info;
  if (lstat(path, &info) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  out->device = (uint64_t)info.st_dev;
  out->inode = (uint64_t)info.st_ino;
  out->links = (uint64_t)info.st_nlink;
  out->mode = (uint32_t)(info.st_mode & 07777);
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_file_symlink(const char * target, const char * path) {
  if (!target || !path) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* A symbolic link needs a privilege a normal user does not have. Reporting
   * success, or writing a regular file where the link would have been, would
   * both be lies. The call does not apply, and nothing is created. */
  (void)target;
  (void)path;
  return GCU_FILE_ERR_ACCESS;
#else
  if (symlink(target, path) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_file_read_link(const char * path,
    const GCU_Allocator * allocator, char ** out_target, size_t * out_len) {
  if (!path || !out_target || !out_len) {
    return GCU_FILE_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gcu_allocator_default();
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  wchar_t * wide = gcu_path_internal_to_wide(allocator, path);
  if (!wide) {
    return GCU_FILE_ERR_OOM;
  }
  HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  DWORD open_err = GetLastError();
  gcu_allocator_free(allocator, wide);
  if (handle == INVALID_HANDLE_VALUE) {
    return file_result_from_win32(open_err);
  }
  /* The reparse buffer is not in the headers this file can include portably,
   * so the symlink layout is spelled here. PrintNameOffset is a byte offset
   * from PathBuffer. */
  typedef struct GCU_Reparse_Symlink {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
    ULONG flags;
    WCHAR path_buffer[1];
  } GCU_Reparse_Symlink;
  uint8_t raw[16 * 1024];
  DWORD got = 0;
  BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, raw,
      sizeof raw, &got, NULL);
  DWORD err = GetLastError();
  CloseHandle(handle);
  if (!ok) {
    return file_result_from_win32(err);
  }
  if (got < sizeof(ULONG) + 2 * sizeof(USHORT)) {
    return GCU_FILE_ERR_IO;
  }
  GCU_Reparse_Symlink * link = (GCU_Reparse_Symlink *)raw;
  /* A junction's path sits where a symlink's flags word is. This reads
   * symlinks only. */
  if (link->tag != IO_REPARSE_TAG_SYMLINK) {
    return GCU_FILE_ERR_IO;
  }
  USHORT offset = link->print_length ? link->print_offset
                                      : link->substitute_offset;
  USHORT length = link->print_length ? link->print_length
                                      : link->substitute_length;
  if ((length % (USHORT)sizeof(wchar_t)) != 0) {
    return GCU_FILE_ERR_IO;
  }
  /* Offsets are from path_buffer, not from the start of the reparse buffer. */
  size_t base = offsetof(GCU_Reparse_Symlink, path_buffer);
  if (base > got || (size_t)offset + (size_t)length > got - base) {
    return GCU_FILE_ERR_IO;
  }
  size_t chars = (size_t)length / sizeof(wchar_t);
  wchar_t * text = (wchar_t *)gcu_allocator_malloc(allocator,
      (chars + 1) * sizeof(wchar_t));
  if (!text) {
    return GCU_FILE_ERR_OOM;
  }
  memcpy(text, (uint8_t *)link->path_buffer + offset, length);
  text[chars] = L'\0';
  char * utf8 = gcu_path_internal_from_wide(allocator, text);
  gcu_allocator_free(allocator, text);
  if (!utf8) {
    return GCU_FILE_ERR_OOM;
  }
  *out_target = utf8;
  *out_len = strlen(utf8);
  return GCU_FILE_OK;
#else
  size_t capacity = 128;
  for (;;) {
    char * buffer = (char *)gcu_allocator_malloc(allocator, capacity);
    if (!buffer) {
      return GCU_FILE_ERR_OOM;
    }
    ssize_t got = readlink(path, buffer, capacity);
    if (got < 0) {
      int saved = errno;
      gcu_allocator_free(allocator, buffer);
      return gcu_file_internal_from_errno(saved);
    }
    /* readlink does not say whether the buffer was exactly full or short by
     * one, so a full buffer is read again larger. The extra byte below is
     * the terminator, which is not part of the length. */
    if ((size_t)got < capacity) {
      buffer[got] = '\0';
      *out_target = buffer;
      *out_len = (size_t)got;
      return GCU_FILE_OK;
    }
    gcu_allocator_free(allocator, buffer);
    if (capacity > (SIZE_MAX / 2)) {
      return GCU_FILE_ERR_IO;
    }
    capacity *= 2;
  }
#endif
}

GCU_File_Result gcu_file_hardlink(const char * existing, const char * path) {
  if (!existing || !path) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* TODO(windows): never compiled or run on Windows. */
  wchar_t * from = gcu_path_internal_to_wide(NULL, existing);
  if (!from) {
    return GCU_FILE_ERR_OOM;
  }
  wchar_t * to = gcu_path_internal_to_wide(NULL, path);
  if (!to) {
    gcu_allocator_free(NULL, from);
    return GCU_FILE_ERR_OOM;
  }
  BOOL ok = CreateHardLinkW(to, from, NULL);
  DWORD err = GetLastError();
  gcu_allocator_free(NULL, from);
  gcu_allocator_free(NULL, to);
  if (!ok) {
    return file_result_from_win32(err);
  }
  return GCU_FILE_OK;
#else
  if (link(existing, path) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}

GCU_File_Result gcu_file_set_mode(const char * path, uint32_t mode) {
  if (!path) {
    return GCU_FILE_ERR_INVALID;
  }
#ifdef _WIN32
  /* A Unix mode does not apply. _wchmod would move only the read-only
   * attribute and report success, which is a mode this call did not set.
   * A missing path is still a missing path, so that failure is asked first. */
  (void)mode;
  GCU_File_Info info;
  GCU_File_Result looked = gcu_file_stat_link(path, &info);
  if (looked != GCU_FILE_OK) {
    return looked;
  }
  return GCU_FILE_ERR_ACCESS;
#else
  if (chmod(path, (mode_t)mode) != 0) {
    return gcu_file_internal_from_errno(errno);
  }
  return GCU_FILE_OK;
#endif
}
