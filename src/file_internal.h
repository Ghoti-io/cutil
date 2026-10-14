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
 * One translation from the platform's errno to this library's vocabulary,
 * shared by `file.c` and `dir.c`.
 *
 * It was written twice before it was written once, which is the same mistake
 * `documentation/file.md` section 2 is about, committed inside the module that
 * exists because of it.
 */

#ifndef GHOTI_IO_GCU_FILE_INTERNAL_H
#define GHOTI_IO_GCU_FILE_INTERNAL_H

#include <errno.h>
#include <stdbool.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/macros.h>

#if !defined(_WIN32) && !defined(__cplusplus)
#include <sys/types.h>
/* A 32-bit off_t makes readdir() fail with EOVERFLOW on an ext4 directory and
 * fseeko() truncate an offset of 2 GiB or more.  The Makefile defines
 * _FILE_OFFSET_BITS=64 (LFS_FLAGS); this refuses a build that did not. */
_Static_assert(sizeof(off_t) == 8,
  "build with -D_FILE_OFFSET_BITS=64 (LFS_FLAGS in the Makefile)");
#endif

/**
 * An initialiser for a GCU_File_Info that is about to be filled by
 * gcu_file_stat().  The function sets every member whenever it returns OK, but
 * gcc 14 at -O2 on a 32-bit target cannot see that through the inlined
 * wrapper and reports `type` as maybe-uninitialised, which -Werror turns into
 * a build failure.
 */
#define GCU_FILE_INFO_INIT {GCU_FILE_TYPE_OTHER, 0, 0}

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Turn the platform's own complaint into this library's vocabulary.
 *
 * The distinctions drawn are the ones a caller can act on.  The dividing line
 * is not severity but **determinism**:  `GCU_FILE_ERR_IO` means the filesystem
 * tried and something went wrong, which a caller may reasonably retry or
 * report as a device problem.  A failure to resolve the path is not that - it
 * will never succeed on a retry, and it is a statement about the path rather
 * than about the device - so those become `GCU_FILE_ERR_NOT_FOUND` however
 * they are spelled:  the path is absent, a component of it is not a directory,
 * or it goes round a loop of symbolic links.
 *
 * `ENAMETOOLONG` is the one that looks like it belongs with those and does
 * not.  The three above are statements about the filesystem's current *state*:
 * nothing the caller does to its argument changes them, and they can stop being
 * true while the caller sits still, because somebody else created a file or
 * replaced a link.  `ENAMETOOLONG` is a statement about the *argument*, and
 * only the caller can change it - so "try the same call again later" is
 * coherent for the first three and incoherent for this one.  That is
 * `GCU_FILE_ERR_INVALID`.
 *
 * With one caveat, since somebody will eventually read that code as the wider
 * claim:  `NAME_MAX` and `PATH_MAX` are per-filesystem, so `ENAMETOOLONG` is a
 * property of the argument *and* the filesystem rather than of the argument
 * alone.  The same path can be too long on one mount and fine on another.  It
 * does not change the mapping - the caller still has to change its input - but
 * `ERR_INVALID` here means "not usable against this filesystem", not "malformed
 * string".
 *
 * Everything else stays `GCU_FILE_ERR_IO`.  Nobody can usefully branch on the
 * difference between `EIO` and `ENXIO`.
 */
static inline GCU_File_Result gcu_file_internal_from_errno(int code) {
  switch (code) {
    case ENOENT:
    case ENOTDIR:
#if defined(ELOOP)
    case ELOOP:
#endif
      return GCU_FILE_ERR_NOT_FOUND;
#if defined(ENAMETOOLONG)
    case ENAMETOOLONG:
      return GCU_FILE_ERR_INVALID;
#endif
    case EEXIST:
      return GCU_FILE_ERR_EXISTS;
    case EACCES:
    case EPERM:
    // Reachable only from the writing paths - a read-only filesystem still
    // permits reads - but the table is shared, so it is stated once.
    case EROFS:
      return GCU_FILE_ERR_ACCESS;
    // Guarded on the value, not merely on the name: some platforms define
    // ENOTEMPTY as EEXIST, and a duplicate case label does not compile.
#if defined(ENOTEMPTY) && ENOTEMPTY != EEXIST
    case ENOTEMPTY:
      return GCU_FILE_ERR_NOT_EMPTY;
#endif
    case ENOMEM:
      return GCU_FILE_ERR_OOM;
    default:
      return GCU_FILE_ERR_IO;
  }
}

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wchar.h>

/**
 * How many names a temporary-file or -directory creator tries before giving
 * up.  A clash in 62^6 names only happens by design or by attack, so this is
 * about bounding a pathological case rather than about ordinary contention.
 */
#define GCU_FILE_INTERNAL_TEMP_ATTEMPTS 100

/**
 * Fill the six characters at @p tail - the "XXXXXX" of a template, located by
 * the caller once so that a retry can refill them - with random characters
 * from [A-Za-z0-9], drawn from the system's cryptographic generator.
 *
 * This is what mkstemp() and mkdtemp() do on POSIX, and what the header
 * promises.  _wmktemp_s, which this replaces, fills the six places with one
 * letter and the process id: a name anyone can predict, and only 26 of them
 * per prefix per process, after which it fails.  compress's test helper,
 * holding many temporaries at once, ran out and was handed back a name it
 * was still using.
 *
 * @return false if no randomness was had.
 */
static inline bool gcu_file_internal_randomize_tail(wchar_t * tail) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  unsigned char bytes[6];
  if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, bytes, sizeof(bytes),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    // 256 is not a multiple of 62, so the first 8 characters are very
    // slightly likelier.  Irrelevant to a name that only has to be unguessed
    // and unused.
    tail[i] = (wchar_t)alphabet[bytes[i] % 62];
  }
  return true;
}
#endif

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCU_FILE_INTERNAL_H
