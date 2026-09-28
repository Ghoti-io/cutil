#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io CUtil.
#
# Ghoti.io CUtil is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Judge gcu_random_java and gcu_random_splitmix64 by OpenJDK.

    random_diff.py <random_words>

random_words is this library. OpenJDK, in the pinned image, is the judge.
java.util.Random.nextInt is the 32-bit word. SplittableRandom.nextLong is
SplitMix64. xoshiro256++ and PCG64 are not Java generators and are not
asked here.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "java", "RandomOracle.java")

# Decimal seeds, so both sides parse the same integer. 32 words is enough
# to cross a 48-bit LCG's mask and a SplitMix step more than once.
CASES = (
    ("java", "nextInt", "0", "32"),
    ("java", "nextInt", "1", "32"),
    ("java", "nextInt", "123456789", "32"),
    ("splitmix", "nextLong", "0", "32"),
    ("splitmix", "nextLong", "1", "32"),
    ("splitmix", "nextLong", "123456789", "32"),
)


def lines_of(finished, what):
    if finished.returncode != 0:
        sys.stderr.write("%s exited %s\n%s\n" % (
            what, finished.returncode, finished.stderr))
        sys.exit(1)
    return [line.strip() for line in finished.stdout.splitlines() if line.strip()]


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: random_diff.py <random_words>\n")
        return 2
    binary = argv[1]
    if not os.path.isfile(SOURCE):
        sys.stderr.write("missing %s\n" % SOURCE)
        return 1

    scratch = tempfile.mkdtemp(prefix="gcu-java-oracle-")
    try:
        compile_run = subprocess.run(
            oracle_env.command("java", ["javac", "-d", scratch, SOURCE], scratch=scratch),
            capture_output=True, text=True)
        if compile_run.returncode != 0:
            sys.stderr.write("javac exited %s\n%s\n" % (
                compile_run.returncode, compile_run.stderr))
            return 1

        for ours, theirs, seed, count in CASES:
            library = subprocess.run(
                [binary, ours, seed, count], capture_output=True, text=True)
            reference = subprocess.run(
                oracle_env.command(
                    "java",
                    ["java", "-cp", scratch, "RandomOracle", theirs, seed, count],
                    scratch=scratch),
                capture_output=True, text=True)
            got = lines_of(library, "%s %s %s" % (binary, ours, seed))
            expect = lines_of(reference, "java %s %s" % (theirs, seed))
            if got != expect:
                sys.stderr.write(
                    "%s seed %s disagrees with OpenJDK %s\n" % (ours, seed, theirs))
                n = min(len(got), len(expect))
                for i in range(n):
                    if got[i] != expect[i]:
                        sys.stderr.write(
                            "  word %s: library %s  openjdk %s\n" % (i, got[i], expect[i]))
                        break
                else:
                    sys.stderr.write(
                        "  length: library %s  openjdk %s\n" % (len(got), len(expect)))
                return 1
            print("%s seed %s: %s words match" % (ours, seed, count))
    finally:
        subprocess.run(["rm", "-rf", scratch], check=False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
