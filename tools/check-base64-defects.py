#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io CUtil.
"""Plant each way the base64 codec can be wrong and require the tests to notice.

    check-base64-defects.py

Environment: B64_CC, B64_CFLAGS (C compile of src/base64.c), B64_CXX,
B64_CXXFLAGS, B64_LIBS (gtest), B64_WORK (a scratch directory).

Each plant is an exact replacement in a copy of src/base64.c whose anchor is
asserted to match once. The copy is built with test/test-base64.cpp into one
program; the plant is caught when that program fails. A plant whose copy does
not build is an error, never a catch. The control (no plant) must pass.
"""
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PLANTS = [
    ("data after padding is accepted", "if (group_done_padded || pads) {", "if (pads) {"),
    ("the other alphabet is accepted", "return c == '-' ? 62 : c == '_' ? 63 : -1;",
     "return (c == '-' || c == '+') ? 62 : (c == '_' || c == '/') ? 63 : -1;"),
    ("the standard alphabet takes the URL characters", "return c == '+' ? 62 : c == '/' ? 63 : -1;",
     "return (c == '+' || c == '-') ? 62 : (c == '/' || c == '_') ? 63 : -1;"),
    ("trailing bits are ignored when they must be zero", "o->trailing_bits == GCU_BASE64_TRAILING_BITS_MUST_BE_ZERO",
     "0"),
    ("whitespace is skipped inside an unfinished padding", "      if (pads) {\n        FAIL(GCU_BASE64_ERR_PADDING, i); /* whitespace",
     "      if (0) {\n        FAIL(GCU_BASE64_ERR_PADDING, i); /* whitespace"),
    ("padding is accepted when forbidden", "o->padding == GCU_BASE64_PADDING_FORBIDDEN || ", ""),
    ("missing padding is accepted when required", "if (o->padding == GCU_BASE64_PADDING_REQUIRED) {", "if (0) {"),
    ("a short output buffer is not noticed", "if (count > cap) {", "if (count > cap && !(cap + 1)) {"),
    ("the encoded size wraps", "if (!gcu_safe_mul_size(groups, 4, &chars)) {", "if ((chars = groups * 4), 0) {"),
    ("a lone final character is accepted", "if (have == 1) {", "if (0) {"),
    ("padding is accepted with fewer than two characters", "group_done_padded || have < 2", "group_done_padded"),
    ("the decoded size bound is too small", "return (n / 4) * 3 + 3;", "return (n / 4) * 3;"),
    ("vertical tab is skipped as whitespace", "c == '\\n' || c == '\\f'", "c == '\\n' || c == '\\f' || c == '\\v'"),
    ("the error offset is off by one", "FAIL(GCU_BASE64_ERR_CHARACTER, i);", "FAIL(GCU_BASE64_ERR_CHARACTER, i + 1);"),
    ("padding is always written", "if (padding == GCU_BASE64_ENCODE_PADDING_ALWAYS) {\n      out[o++] = '=';\n      out[o++] = '=';",
     "if (1) {\n      out[o++] = '=';\n      out[o++] = '=';"),
    ("the last group's bytes are miscounted", "produced += have - 1;", "produced += have;"),
]


def build_and_run(source_text, work, tag):
    src = os.path.join(work, "base64-%s.c" % tag)
    obj = os.path.join(work, "base64-%s.o" % tag)
    exe = os.path.join(work, "test-base64-%s" % tag)
    with open(src, "w", encoding="utf-8") as handle:
        handle.write(source_text)
    cc = shlex.split(os.environ["B64_CC"]) + shlex.split(os.environ["B64_CFLAGS"]) + ["-I", os.path.join(ROOT, "include"),
        "-I", os.path.join(ROOT, "build/linux/release/include"), "-c", src, "-o", obj]
    done = subprocess.run(cc, capture_output=True, text=True)
    if done.returncode != 0:
        return None, "the plant did not compile:\n" + done.stderr[-500:]
    cxx = " ".join([os.environ["B64_CXX"], os.environ["B64_CXXFLAGS"], "-I", shlex.quote(os.path.join(ROOT, "include")),
        "-I", shlex.quote(os.path.join(ROOT, "build/linux/release/include")),
        shlex.quote(os.path.join(ROOT, "test/test-base64.cpp")), shlex.quote(obj), "-o", shlex.quote(exe),
        os.environ["B64_LIBS"]])
    # B64_LIBS is the Makefile's TESTFLAGS, a backquoted pkg-config call: it needs a shell.
    done = subprocess.run(cxx, shell=True, capture_output=True, text=True, errors="replace")
    if done.returncode != 0:
        return None, "the test program did not link:\n" + done.stderr[-500:]
    run = subprocess.run([exe], cwd=ROOT, capture_output=True, text=True, errors="replace")
    return run.returncode, ""


def main():
    work = os.environ["B64_WORK"]
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(ROOT, "src/base64.c"), encoding="utf-8") as handle:
        original = handle.read()
    status, why = build_and_run(original, work, "control")
    if status != 0:
        sys.stderr.write("check-base64-defects: the control must pass (%s)\n" % (why or "tests failed"))
        return 1
    bad = []
    caught = 0
    for index, (name, anchor, replacement) in enumerate(PLANTS):
        if original.count(anchor) < 1:
            bad.append("%s: the anchor matches nowhere" % name)
            continue
        status, why = build_and_run(original.replace(anchor, replacement), work, "p%d" % index)
        if status is None:
            bad.append("%s: %s" % (name, why))
        elif status == 0:
            bad.append("%s: NOT CAUGHT" % name)
        else:
            caught += 1
    for line in bad:
        sys.stderr.write("check-base64-defects: %s\n" % line)
    print("check-base64-defects: %d of %d planted defects caught; the control passes" % (caught, len(PLANTS)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
