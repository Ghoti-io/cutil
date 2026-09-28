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

#include <ghoti.io/cutil/macros.h>
#include <ghoti.io/cutil/random.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char * what) {
  fprintf(stderr, "%s\n", what);
  exit(1);
}

int main(int argc, char ** argv) {
  unsigned long long seed;
  unsigned long count;
  unsigned long i;
  char * end = NULL;
  GCU_Random * r;

  if (argc != 4) {
    die("usage: random_words java|mt32|splitmix|xoshiro|pcg|mt64 seed count");
  }
  seed = strtoull(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0') {
    die("seed is not a decimal integer");
  }
  count = strtoul(argv[3], &end, 10);
  if (end == argv[3] || *end != '\0') {
    die("count is not a decimal integer");
  }

  if (strcmp(argv[1], "java") == 0) {
    r = gcu_random_java((uint64_t)seed);
    if (r == NULL) {
      die("gcu_random_java failed");
    }
  } else if (strcmp(argv[1], "mt32") == 0) {
    if (seed > UINT32_MAX) {
      die("mt32 seed does not fit in 32 bits");
    }
    r = gcu_random_mt32((uint32_t)seed);
    if (r == NULL) {
      die("gcu_random_mt32 failed");
    }
  } else if (strcmp(argv[1], "splitmix") == 0) {
    r = gcu_random_splitmix64((uint64_t)seed);
    if (r == NULL) {
      die("gcu_random_splitmix64 failed");
    }
  } else if (strcmp(argv[1], "xoshiro") == 0) {
    r = gcu_random_xoshiro256pp((uint64_t)seed);
    if (r == NULL) {
      die("gcu_random_xoshiro256pp failed");
    }
  } else if (strcmp(argv[1], "pcg") == 0) {
    r = gcu_random_pcg64((uint64_t)seed);
    if (r == NULL) {
      die("gcu_random_pcg64 failed");
    }
  } else if (strcmp(argv[1], "mt64") == 0) {
    r = gcu_random_mt64((uint64_t)seed);
    if (r == NULL) {
      die("gcu_random_mt64 failed");
    }
  } else {
    die("unknown generator");
  }
  if (strcmp(argv[1], "java") == 0 || strcmp(argv[1], "mt32") == 0) {
    for (i = 0; i < count; ++i) {
      uint32_t word = 0;
      if (gcu_random_u32(r, &word) != 0) {
        die("gcu_random_u32 failed");
      }
      printf("%08" PRIx32 "\n", word);
    }
  } else {
    for (i = 0; i < count; ++i) {
      uint64_t word = 0;
      if (gcu_random_u64(r, &word) != 0) {
        die("gcu_random_u64 failed");
      }
      printf("%016" PRIx64 "\n", word);
    }
  }
  gcu_random_free(r);
  return 0;
}
