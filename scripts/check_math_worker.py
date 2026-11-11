#!/usr/bin/env python3
"""Checks that a built or packaged app can draw formulas.

The app draws formulas in a second process of itself, "visnotepad
--math-worker" (see src/markdown/MathRenderer.h). This starts that worker the
way the app does, asks it for one formula and then for one that crashes
MicroTeX, and fails unless the first comes back as shapes and the second
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

MAGIC = 0x564E4D33  # "VNM3", the first bytes of every answer.


def request(tex, display=False):
    """One request, as Worker::layout() in MathRenderer.cpp writes it."""
    utf8 = tex.encode("utf-8")
    # QDataStream: big-endian; a QByteArray is its length and its bytes.
    body = struct.pack(">I", len(utf8)) + utf8 + struct.pack(">B", 1 if display else 0)
    return struct.pack("=I", len(body)) + body  # The size as the machine has it.


def read_answer(pipe):
    """(status, shapes bytes), or None when the worker ended first."""
    header = read_exactly(pipe, 12)
    if header is None:
        return None
    magic, status, size = struct.unpack("=Iii", header)
    if magic != MAGIC:
        sys.exit("not an answer: %r" % (header,))
    if status != 1:
        return status, b""
    if not 0 < size <= 8 * 1024 * 1024:
        sys.exit("an answer of %d bytes" % size)
    shapes = read_exactly(pipe, size)
    if shapes is None:
        sys.exit("the answer of %d bytes is cut short" % size)
    return status, shapes


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
        answer = read_answer(worker.stdout)
        if answer is None:
            sys.exit("the worker ended on an ordinary formula (exit code %s)" % worker.wait())
        status, shapes = answer
        if status != 1 or len(shapes) < 16:
            sys.exit("no shapes for an ordinary formula (answer %d)" % status)
        # Its size (QDataStream: big-endian, single precision), then shapes.
        width, ascent, depth, count = struct.unpack(">fffI", shapes[:16])
        if not (0 < width < 50000 and 0 < ascent < 50000 and 0 <= depth < 50000):
            sys.exit("a formula of %r x %r + %r" % (width, ascent, depth))
        # Five glyphs (x, 2, +, a, b) and the fraction's bar; without fonts,
        # only the bar.
        if count < 6:
            sys.exit("the formula has %d shapes: no fonts?" % count)
        print("formula drawn: %d shapes, %.0f wide at 100 pixels" % (count, width))

        # MicroTeX writes to memory that is not its own for this one. The
        # worker says so (Linux: a copy of it drew the formula) or ends.
        worker.stdin.write(request(r"\multicolumn{2}{c}{x}"))
        worker.stdin.flush()
        answer = read_answer(worker.stdout)
        if answer is None:
            print("the formula that breaks MicroTeX ended the worker (exit code %s)" % worker.wait())
        else:
            print("the formula that breaks MicroTeX was answered with %d" % answer[0])
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
