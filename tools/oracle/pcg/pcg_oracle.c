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

#include <pcg_variants.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void die(const char * what) {
  fprintf(stderr, "%s\n", what);
  exit(1);
}

int main(int argc, char ** argv) {
  unsigned long long seed;
  unsigned long count;
  unsigned long i;
  char * end = NULL;
  pcg64_random_t rng;

  if (argc != 3) {
    die("usage: pcg_oracle seed count");
  }
  seed = strtoull(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0') {
    die("seed is not a decimal integer");
  }
  count = strtoul(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0') {
    die("count is not a decimal integer");
  }

  pcg64_srandom_r(&rng, (pcg128_t)seed, (pcg128_t)0);
  for (i = 0; i < count; ++i) {
    printf("%016" PRIx64 "\n", (uint64_t)pcg64_random_r(&rng));
  }
  return 0;
}
