#!/bin/sh
#
# One arm of `make check-fiber-defects`: run the fiber tests against a library
# the gate built with a defect planted in it, and require them to notice.
#
#   check-fiber-defects.sh NAME BINARY GOODDIR PLANTDIR EXPECT [VAR=VALUE ...]
#
# NAME      what is planted, for the message.
# BINARY    the test-fiber executable for this tree.
# GOODDIR   directory holding the real library, for the control run.
# PLANTDIR  directory holding the library with the defect.
# EXPECT    an extended regular expression the planted run's output must
#           match: the failing test's name in a FAILED line, or the
#           sanitizer's own report.
# VAR=VALUE environment for both runs (sanitizer options, LD_PRELOAD).
#
# Two runs, because a gate that has only ever been seen to find a defect has
# not been seen to stay quiet without one:
#
#   control  against the real library: must exit 0 and must NOT match EXPECT;
#   planted  against the defective one: must exit non-zero (or, with
#            GCU_GATE_MAY_EXIT_ZERO=1, for a sanitizer that only warns)
#            and must match EXPECT.
#
# Exits 1, naming the arm, if either run does the wrong thing.  Both runs'
# output is kept in a temporary file and shown when an arm fails.

name=$1
binary=$2
gooddir=$3
plantdir=$4
expect=$5
shift 5

out=$(mktemp) || exit 2
trap 'rm -f "$out"' EXIT

# $1 = library directory; the rest is the caller's environment.
run_tests() {
  dir=$1
  shift
  env "$@" LD_LIBRARY_PATH="$dir" "$binary" --gtest_brief=1 >"$out" 2>&1
}

printf '  %-38s ' "$name"

run_tests "$gooddir" "$@"
status=$?
if [ "$status" -ne 0 ]; then
  printf 'FAIL\n    the control run (real library) exited %s\n' "$status" >&2
  cat "$out" >&2
  exit 1
fi
if grep -Eq "$expect" "$out"; then
  printf 'FAIL\n    the control run matched /%s/ with nothing planted\n' \
    "$expect" >&2
  cat "$out" >&2
  exit 1
fi

run_tests "$plantdir" "$@"
status=$?
if [ "$status" -eq 0 ] && [ "${GCU_GATE_MAY_EXIT_ZERO:-0}" != 1 ]; then
  printf 'FAIL\n    the planted defect was not caught: exit 0\n' >&2
  cat "$out" >&2
  exit 1
fi
if ! grep -Eq "$expect" "$out"; then
  printf 'FAIL\n    exit %s, but the output never matched /%s/\n' \
    "$status" "$expect" >&2
  cat "$out" >&2
  exit 1
fi
printf 'caught (exit %s)\n' "$status"
exit 0
