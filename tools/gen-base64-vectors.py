#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io CUtil.
"""Write test/data/base64-vectors.txt: encodings made by Python's base64 module.

    gen-base64-vectors.py [--check]

Each line is `<alphabet> <padding> <input in hex or -> <encoding or ->`, where
alphabet is `standard` or `url` and padding is `pad` or `nopad`. The module
shares no code with the C codec, so agreement is evidence. The generator is
seeded, so the file is the same every time; --check fails when the committed
file is not what this writes.
"""
import base64
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "test", "data", "base64-vectors.txt")


def lines():
    rng = random.Random(0x4234)
    samples = [b""]
    samples += [bytes([b]) for b in (0, 1, 0x3e, 0x3f, 0xfb, 0xff)]
    for n in range(2, 40):
        samples.append(bytes(rng.randrange(256) for _ in range(n)))
    for n in (63, 64, 65, 255, 256, 257, 1000, 4095, 4096, 4097):
        samples.append(bytes(rng.randrange(256) for _ in range(n)))
    # Bytes whose encodings contain every character of both alphabets.
    samples.append(bytes([0xfb, 0xef, 0xbe, 0xff, 0xff, 0xff, 0xfc, 0x00, 0x3f]))
    for data in samples:
        for alphabet in ("standard", "url"):
            std = base64.b64encode(data) if alphabet == "standard" else base64.urlsafe_b64encode(data)
            for padding in ("pad", "nopad"):
                enc = std if padding == "pad" else std.rstrip(b"=")
                yield "%s %s %s %s" % (alphabet, padding, data.hex() or "-", enc.decode() or "-")


def main(argv):
    text = "\n".join(lines()) + "\n"
    if "--check" in argv:
        with open(OUT, encoding="utf-8") as handle:
            if handle.read() != text:
                sys.stderr.write("base64-vectors.txt is not what gen-base64-vectors.py writes\n")
                return 1
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
