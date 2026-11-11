#!/usr/bin/env python3
"""Checks that a built or packaged app can draw formulas.

The app draws formulas in a second process of itself, "visnotepad
--math-worker" (see src/markdown/MathRenderer.h). This starts that worker the
way the app does, asks it for one formula and then for one that crashes
MicroTeX, and fails unless the first comes back as a picture and the second
does no worse than end the worker. A package that lacks what the worker needs
(the offscreen platform plugin, MicroTeX's fonts) fails here, not on a user's
machine with every formula shown as source text.

Usage: check_math_worker.py <path to the visnotepad executable>
"""

import os
import struct
import subprocess
import sys
import threading

MAGIC = 0x564E4D31  # "VNM1", the first bytes of every answer.


def request(tex, pixel_size=16.0, rgba=0xFF000000, ratio=1.0, display=False):
    """One request, as Worker::render() in MathRenderer.cpp writes it."""
    utf8 = tex.encode("utf-8")
    # QDataStream: big-endian; a QByteArray is its length and its bytes.
    body = struct.pack(">I", len(utf8)) + utf8
    body += struct.pack(">dIdB", pixel_size, rgba, ratio, 1 if display else 0)
    return struct.pack("=I", len(body)) + body  # The size as the machine has it.


def read_exactly(pipe, size):
    data = b""
    while len(data) < size:
        chunk = pipe.read(size - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    exe = sys.argv[1]
    env = dict(os.environ)
    if os.name != "nt":
        env["QT_QPA_PLATFORM"] = "offscreen"  # As the app sets it there.
    worker = subprocess.Popen([exe, "--math-worker"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, env=env)
    # Nothing here may wait for ever on a worker that hangs.
    timed_out = []

    def give_up():
        timed_out.append(True)
        worker.kill()

    watchdog = threading.Timer(60, give_up)
    watchdog.start()
    try:
        hello = read_exactly(worker.stdout, 8)
        if hello is None:
            sys.exit("the worker ended before it said hello (exit code %s)" % worker.wait())
        magic, ready = struct.unpack("=Ii", hello)
        if magic != MAGIC:
            sys.exit("not a math worker: its first bytes are %r" % hello)
        if ready != 1:
            sys.exit("the worker runs, but MicroTeX did not start (its resources are missing?)")

        worker.stdin.write(request(r"x^2+\frac{a}{b}"))
        worker.stdin.flush()
        header = read_exactly(worker.stdout, 12)
        if header is None:
            sys.exit("the worker ended on an ordinary formula (exit code %s)" % worker.wait())
        magic, width, height = struct.unpack("=Iii", header)
        if magic != MAGIC or not (0 < width <= 16384 and 0 < height <= 16384):
            sys.exit("no picture for an ordinary formula: %r" % (header,))
        pixels = read_exactly(worker.stdout, width * height * 4)
        if pixels is None:
            sys.exit("the picture of %dx%d is cut short" % (width, height))
        if not any(pixels[3::4]):
            sys.exit("the picture of %dx%d is empty: no fonts?" % (width, height))
        print("formula drawn: %dx%d pixels" % (width, height))

        # MicroTeX writes to memory that is not its own for this one. The
        # worker says so (Linux: a copy of it drew the formula) or ends.
        worker.stdin.write(request(r"\multicolumn{2}{c}{x}"))
        worker.stdin.flush()
        header = read_exactly(worker.stdout, 12)
        if header is None:
            print("the formula that breaks MicroTeX ended the worker (exit code %s)" % worker.wait())
        else:
            magic, width, height = struct.unpack("=Iii", header)
            if magic != MAGIC:
                sys.exit("nonsense after the formula that breaks MicroTeX: %r" % (header,))
            print("the formula that breaks MicroTeX was answered with %d, %d" % (width, height))
        worker.stdin.close()
        worker.wait()
    finally:
        watchdog.cancel()
        if worker.poll() is None:
            worker.kill()
    if timed_out:
        sys.exit("the worker did not answer, or did not end, within a minute")


if __name__ == "__main__":
    main()
