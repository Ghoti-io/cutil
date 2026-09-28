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
"""Judge the seeded generators by the references that define them.

    random_diff.py <random_words>

random_words is this library. OpenJDK judges java.util.Random.nextInt and
SplittableRandom.nextLong. Rust's Xoshiro256PlusPlus::seed_from_u64 judges
xoshiro256++. O'Neill's pcg64_srandom_r(seed, 0) then pcg64_random_r judges
PCG64. The Mersenne Twister is judged by the C++ standard library in the
unit test, not here.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
JAVA_SOURCE = os.path.join(HERE, "java", "RandomOracle.java")
PCG_SOURCE = os.path.join(HERE, "pcg", "pcg_oracle.c")
RUST_DIR = os.path.join(HERE, "rust")
PCG_INCLUDE = "/usr/local/src/pcg-c/include"
PCG_ARCHIVE = "/usr/local/src/pcg-c/src/libpcg_random.a"

# Decimal seeds, so both sides parse the same integer. 32 words is enough
# to cross a 48-bit LCG's mask and more than one step of the others.
JAVA_CASES = (
    ("java", "nextInt", "0", "32"),
    ("java", "nextInt", "1", "32"),
    ("java", "nextInt", "123456789", "32"),
    ("splitmix", "nextLong", "0", "32"),
    ("splitmix", "nextLong", "1", "32"),
    ("splitmix", "nextLong", "123456789", "32"),
)
WORD_CASES = (
    ("xoshiro", "0", "32"),
    ("xoshiro", "1", "32"),
    ("xoshiro", "123456789", "32"),
    ("pcg", "0", "32"),
    ("pcg", "1", "32"),
    ("pcg", "123456789", "32"),
)


def lines_of(finished, what):
    if finished.returncode != 0:
        sys.stderr.write("%s exited %s\n%s\n" % (
            what, finished.returncode, finished.stderr))
        sys.exit(1)
    return [line.strip() for line in finished.stdout.splitlines() if line.strip()]


def disagree(ours, seed, judge, got, expect):
    sys.stderr.write("%s seed %s disagrees with %s\n" % (ours, seed, judge))
    n = min(len(got), len(expect))
    for i in range(n):
        if got[i] != expect[i]:
            sys.stderr.write(
                "  word %s: library %s  reference %s\n" % (i, got[i], expect[i]))
            return
    sys.stderr.write(
        "  length: library %s  reference %s\n" % (len(got), len(expect)))


def compile_in(name, argv, scratch):
    finished = subprocess.run(
        oracle_env.command(name, argv, scratch=scratch),
        capture_output=True, text=True)
    if finished.returncode != 0:
        sys.stderr.write("%s\n%s\n" % (" ".join(argv[:3]), finished.stderr))
        sys.exit(1)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: random_diff.py <random_words>\n")
        return 2
    binary = argv[1]
    for path in (JAVA_SOURCE, PCG_SOURCE, os.path.join(RUST_DIR, "src", "main.rs")):
        if not os.path.isfile(path):
            sys.stderr.write("missing %s\n" % path)
            return 1

    scratch = tempfile.mkdtemp(prefix="gcu-random-oracle-")
    try:
        compile_in("java", ["javac", "-d", scratch, JAVA_SOURCE], scratch)
        compile_in("pcg", [
            "gcc", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", PCG_INCLUDE,
            "-o", os.path.join(scratch, "pcg-oracle"),
            PCG_SOURCE, PCG_ARCHIVE,
        ], scratch)
        compile_in("xoshiro", [
            "cargo", "build", "--offline", "--locked", "--release",
            "--manifest-path", os.path.join(RUST_DIR, "Cargo.toml"),
            "--target-dir", os.path.join(scratch, "xoshiro-target"),
        ], scratch)
        xoshiro_bin = os.path.join(
            scratch, "xoshiro-target", "release", "xoshiro-oracle")
        pcg_bin = os.path.join(scratch, "pcg-oracle")

        for ours, theirs, seed, count in JAVA_CASES:
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
                disagree(ours, seed, "OpenJDK %s" % theirs, got, expect)
                return 1
            print("%s seed %s: %s words match" % (ours, seed, count))

        for ours, seed, count in WORD_CASES:
            library = subprocess.run(
                [binary, ours, seed, count], capture_output=True, text=True)
            judge = "xoshiro" if ours == "xoshiro" else "pcg"
            program = xoshiro_bin if ours == "xoshiro" else pcg_bin
            reference = subprocess.run(
                oracle_env.command(judge, [program, seed, count], scratch=scratch),
                capture_output=True, text=True)
            got = lines_of(library, "%s %s %s" % (binary, ours, seed))
            expect = lines_of(reference, "%s %s" % (judge, seed))
            if got != expect:
                disagree(ours, seed, judge, got, expect)
                return 1
            print("%s seed %s: %s words match" % (ours, seed, count))
    finally:
        subprocess.run(["rm", "-rf", scratch], check=False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
