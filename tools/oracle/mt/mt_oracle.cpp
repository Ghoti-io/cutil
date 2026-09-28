/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
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

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

static void die(const char * what) {
  std::fprintf(stderr, "%s\n", what);
  std::exit(1);
}

int main(int argc, char ** argv) {
  unsigned long long seed;
  unsigned long count;
  char * end = nullptr;

  if (argc != 4) {
    die("usage: mt_oracle mt32|mt64 seed count");
  }
  seed = std::strtoull(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0') {
    die("seed is not a decimal integer");
  }
  count = std::strtoul(argv[3], &end, 10);
  if (end == argv[3] || *end != '\0') {
    die("count is not a decimal integer");
  }

  if (std::strcmp(argv[1], "mt32") == 0) {
    if (seed > UINT32_MAX) {
      die("mt32 seed does not fit in 32 bits");
    }
    std::mt19937 mt(static_cast<std::uint32_t>(seed));
    for (unsigned long i = 0; i < count; ++i) {
      std::printf("%08" PRIx32 "\n", static_cast<std::uint32_t>(mt()));
    }
    return 0;
  }
  if (std::strcmp(argv[1], "mt64") == 0) {
    std::mt19937_64 mt(static_cast<std::uint64_t>(seed));
    for (unsigned long i = 0; i < count; ++i) {
      std::printf("%016" PRIx64 "\n", static_cast<std::uint64_t>(mt()));
    }
    return 0;
  }
  die("unknown generator");
  return 1;
}
