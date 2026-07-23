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

args = [a for a in sys.argv[1:] if a != "rip"]
HOST = args[0] if len(args) > 0 else "192.168.2.123"
PORT = int(args[1]) if len(args) > 1 else 49152
PASS = int(args[2]) if len(args) > 2 else 0
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

# --- 4. RIP audio stream (only with "rip" on the command line) ---------
# Enable = *T <d1> <d0> sent TO the audio port (CMD+2) so the radio learns
# the return address; d1 bit0 = RIP on, bit2 = TRANSMIT (never set here!),
# d0 = compression flags (0 = 16-bit). 5 s timeout — resend every 2 s.
if "rip" in sys.argv:
    print("== RIP audio (10 s capture on port %d) ==" % (PORT + 2))
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    a.bind(("0.0.0.0", PORT + 2))
    a.settimeout(0.5)
    # d0=0x02 requests 8-bit RIP compression: live-found REQUIRED on
    # firmware 1036 — plain 16-bit (d0=0) is silently refused (ripping
    # stays 0), the 8-bit request streams immediately.
    RIP_ON, RIP_OFF = b"*T\x01\x02", b"*T\x00\x00"   # bit2 (TX) never set
    assert not (RIP_ON[2] & 0x04) and not (RIP_OFF[2] & 0x04)
    pkts = []
    t0 = time.monotonic()
    last_ka = 0.0
    while time.monotonic() - t0 < 10.0:
        now = time.monotonic()
        if now - last_ka > 2.0:
            a.sendto(PC + RIP_ON + b"\r", (HOST, PORT + 2))
            last_ka = now
        try:
            d, _ = a.recvfrom(2048)
        except socket.timeout:
            continue
        if len(d) >= 12:
            pkts.append((now, d))
    a.sendto(PC + RIP_OFF + b"\r", (HOST, PORT + 2))
    # drain stragglers, confirm the stream actually stops
    time.sleep(1.0)
    strag = 0
    a.settimeout(0.1)
    try:
        while True:
            a.recvfrom(2048)
            strag += 1
    except socket.timeout:
        pass
    if not pkts:
        fails.append("rip:no-packets")
        print("  no RIP packets received")
    else:
        n = len(pkts)
        span = pkts[-1][0] - pkts[0][0]
        seqs = [int.from_bytes(p[2:4], "big") for _, p in pkts]
        ts = [int.from_bytes(p[4:8], "big") for _, p in pkts]
        sizes = {len(p) for _, p in pkts}
        hdr0 = {p[0] for _, p in pkts}
        gaps = sum(1 for i in range(1, n)
                   if (seqs[i] - seqs[i - 1]) & 0xffff != 1)
        rate = n / span if span > 0 else 0.0
        srate = (ts[-1] - ts[0]) / span if span > 0 else 0.0
        # encoding sniff on the 8-bit stream: true audio is smooth
        # sample-to-sample — compare candidate decodings.
        payload = b"".join(p[12:] for _, p in pkts[n // 2:n // 2 + 4])
        def smooth(vals):
            return sum(abs(vals[i] - vals[i - 1])
                       for i in range(1, len(vals))) / max(len(vals) - 1, 1)
        def ulaw(b):
            b = ~b & 0xff
            seg, man = (b >> 4) & 7, b & 0x0f
            v = ((man << 3) + 0x84) << seg
            return -(v - 0x84) if b & 0x80 == 0 else (v - 0x84)
        cands = {
            "s8-linear": [int.from_bytes(bytes([x]), "big", signed=True) << 8
                          for x in payload],
            "u8-linear": [(x - 128) << 8 for x in payload],
            "ulaw":      [ulaw(x) for x in payload],
        }
        # normalize smoothness by dynamic range so scales compare fairly
        scores = {k: smooth(v) / (max(max(v) - min(v), 1)) for k, v in cands.items()}
        enc = min(scores, key=scores.get)
        print(f"  {n} pkts in {span:.1f}s = {rate:.1f} pkt/s   sizes {sorted(sizes)}"
              f"   hdr0 {sorted(hex(h) for h in hdr0)}")
        print(f"  seq gaps {gaps}   sample rate {srate:.0f} Hz (from RTP ts)"
              f"   encoding ~{enc} {dict((k, round(v,3)) for k,v in scores.items())}"
              f"   stragglers-after-stop {strag}")
        if gaps > n * 0.01:
            fails.append(f"rip:gaps{gaps}")
        if not (0.9 * rate * span <= n):
            fails.append("rip:rate")
        if strag > 20:
            fails.append("rip:did-not-stop")
    a.close()

print("\nRESULT:", "PASS" if not fails else f"FAIL {fails}")
sys.exit(0 if not fails else 1)
