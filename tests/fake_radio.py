#!/usr/bin/env python3
"""Fake Ten-Tec radio on a pty, for the Classic-layout guard.

Opens a pseudo-terminal, symlinks its slave to the path given, and records
every byte the console writes. It never answers (like TTC_RADIO_DEV=/dev/null,
but the writes are kept). On SIGTERM it writes the SET commands — '*'-led
frames — one per line, hex for anything non-printable, to the output file;
'?' queries (polling) are dropped because their timing is not deterministic.

    fake_radio.py <link-path> <out-file>

No real radio is ever involved: nothing here can key a transmitter.
"""
import os
import pty
import select
import signal
import sys
import tty

link, out = sys.argv[1], sys.argv[2]
master, slave = pty.openpty()
tty.setraw(slave)
name = os.ttyname(slave)
try:
    os.unlink(link)
except FileNotFoundError:
    pass
os.symlink(name, link)
buf = bytearray()
done = False


def stop(*_):
    global done
    done = True


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
print(name, flush=True)
while not done:
    r, _, _ = select.select([master], [], [], 0.2)
    if r:
        try:
            buf += os.read(master, 4096)
        except OSError:
            pass

# Split into CR-terminated frames; keep the SET ones.
lines = []
for frame in bytes(buf).split(b"\r"):
    if not frame.startswith(b"*"):
        continue
    if all(32 <= c < 127 for c in frame):
        lines.append(frame.decode())
    else:
        lines.append("*" + frame[1:].hex())
with open(out, "w") as f:
    f.write("\n".join(lines) + ("\n" if lines else ""))
os.unlink(link)
