#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Omni VII One Plug link ruler.

Measures the UDP command channel to an Omni VII in REMOTE mode the same way
zaptest rules the CW peak finder: fixed probes, hard pass criteria, numbers
you can diff run-to-run. Run BEFORE integrating a transport change and after;
the link (not the console) sets the ceiling, so regressions in the console
transport show up as deltas against this baseline.

Usage:  tools/omni7_ruler.py [host] [port] [passcode]
Defaults: 192.168.2.123 49152 0

Checks:
  1. shape  — ?V / ?A / ?B / ?M / ?S replies match the Programmer's Reference
  2. latency — 100x ?A round-trips: min / median / p95 / max
  3. soak   — 30 s at the console's poll cadence (5 Hz): loss count

PASS = all shapes good, soak loss <= 1%, median latency < 100 ms.
"""
import socket, statistics, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.123"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 49152
PASS = int(sys.argv[3]) if len(sys.argv) > 3 else 0
PC = PASS.to_bytes(2, "big")

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))            # symmetric ports: replies come back here
s.settimeout(0.5)

def rt(cmd: bytes):
    """One round-trip; returns (reply|None, seconds)."""
    t0 = time.monotonic()
    s.sendto(PC + cmd + b"\r", (HOST, PORT))
    try:
        data, _ = s.recvfrom(4096)
        return data, time.monotonic() - t0
    except socket.timeout:
        return None, time.monotonic() - t0

fails = []

# --- 1. shapes ---------------------------------------------------------
print(f"== shapes ({HOST}:{PORT}, passcode {PASS}) ==")
shapes = [
    (b"?V", "version", lambda d: d.startswith(b"VER") and b"REMOTE" in d),
    (b"?A", "mainfreq", lambda d: d[:1] == b"A" and len(d) >= 5),
    (b"?B", "subfreq",  lambda d: d[:1] == b"B" and len(d) >= 5),
    (b"?M", "mode",     lambda d: d[:1] == b"M" and len(d) >= 3),
    (b"?S", "smeter",   lambda d: d[:1] == b"S" and len(d) >= 2),
]
freq_a = None
for cmd, name, ok in shapes:
    d, dt = rt(cmd)
    good = d is not None and ok(d)
    if not good:
        fails.append(f"shape:{name}")
    if d and cmd == b"?A" and good:
        freq_a = int.from_bytes(d[1:5], "big")
    print(f"  {name:9} {'OK ' if good else 'FAIL'}  {d[:40]!r}")
if freq_a:
    print(f"  VFO A = {freq_a/1e6:.6f} MHz")

# --- 2. latency --------------------------------------------------------
lat = []
lost = 0
for _ in range(100):
    d, dt = rt(b"?A")
    if d is None:
        lost += 1
    else:
        lat.append(dt * 1000.0)
if lat:
    lat.sort()
    med = statistics.median(lat)
    p95 = lat[int(len(lat) * 0.95) - 1]
    print(f"== latency (100x ?A) ==\n  min {lat[0]:.1f}  med {med:.1f}  "
          f"p95 {p95:.1f}  max {lat[-1]:.1f} ms   lost {lost}")
    if med >= 100.0:
        fails.append("latency:median>=100ms")
else:
    fails.append("latency:all-lost")
    med = None

# --- 3. soak at console cadence ---------------------------------------
print("== soak (30 s @ 5 Hz, rotating ?A/?S/?M) ==")
cmds = [b"?A", b"?S", b"?A", b"?S", b"?M"]     # mimics the poll rotation
n = loss = 0
end = time.monotonic() + 30
while time.monotonic() < end:
    d, _ = rt(cmds[n % len(cmds)])
    n += 1
    if d is None:
        loss += 1
    time.sleep(max(0.0, 0.2 - 0.05))           # ~5 Hz including the round-trip
pct = 100.0 * loss / max(n, 1)
print(f"  {n} queries, {loss} lost ({pct:.1f}%)")
if pct > 1.0:
    fails.append(f"soak:loss{pct:.1f}%")

print("\nRESULT:", "PASS" if not fails else f"FAIL {fails}")
sys.exit(0 if not fails else 1)
