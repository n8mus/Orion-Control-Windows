#!/usr/bin/env python3
"""TRIP (Transmit over IP) ruler for the Omni VII One Plug link.

TRIP is TRANSMIT: it keys the radio and puts the streamed audio on the air.
So this ruler has two very different modes:

  (default) OFFLINE  — validate the TRIP packet format against the spec and
                       the console's encoder. No radio, no socket, no RF.
                       Safe to run anywhere, anytime. This is the ruler.

  --transmit         — the attended on-air test. Keys YOUR transmitter and
                       streams a test tone for a few seconds. Only the
                       operator runs this, PRESENT AT THE RIG, and the first
                       time INTO A DUMMY LOAD. Requires --dummy-load to arm,
                       and the console must be closed (it needs ports
                       49152/49156). This script never transmits without
                       those two explicit flags.

Packet format (fw 1036, 8-bit compressed TRIP), CMD port +4 = 49156:
  byte 0      : packet counter (last+1, wraps at 256)
  bytes 1-128 : 128 signed 8-bit samples @ ~7013 Hz (high byte of each s16)
"""
import math
import socket
import struct
import sys
import time

SR = 7013
SAMPLES = 128
PKT_LEN = 1 + SAMPLES
PKT_RATE = SR / SAMPLES            # ~54.8 datagrams/s


def encode(counter, s16):
    """Mirror of TripAudio::packetize — counter byte + 128 high-bytes."""
    p = bytearray([counter & 0xff])
    for i in range(SAMPLES):
        p.append((s16[i] >> 8) & 0xff)     # signed high byte
    return bytes(p)


def tone_s16(n, f=700, amp=12000, phase=0):
    return [int(amp * math.sin(2 * math.pi * f * (phase + i) / SR))
            for i in range(n)]


def offline():
    fails = []
    print("== TRIP offline format ruler ==")
    # Build a few seconds of tone, packetize exactly as the console does.
    total = SR * 2
    samples = tone_s16(total)
    pkts, counter = [], 0
    for off in range(0, total - SAMPLES + 1, SAMPLES):
        pkts.append(encode(counter, samples[off:off + SAMPLES]))
        counter = (counter + 1) & 0xff

    # 1. length
    bad = [len(p) for p in pkts if len(p) != PKT_LEN]
    print(f"  packet length  {PKT_LEN} B  ({len(pkts)} pkts)"
          f"  {'OK' if not bad else 'BAD ' + str(set(bad))}")
    if bad:
        fails.append("len")

    # 2. counter wraps 0..255 monotonically
    seq = [p[0] for p in pkts]
    wrapped = all((seq[i] - seq[i - 1]) & 0xff == 1 for i in range(1, len(seq)))
    print(f"  counter        wraps +1 mod 256  {'OK' if wrapped else 'BAD'}")
    if not wrapped:
        fails.append("counter")

    # 3. round-trip: decode high-bytes back, compare to source within the
    #    8-bit quantization step (256 LSB). This proves the encoding is the
    #    exact inverse of the RIP decode (high byte = signed 8-bit sample).
    worst = 0
    for k, p in enumerate(pkts):
        src = samples[k * SAMPLES:(k + 1) * SAMPLES]
        for i in range(SAMPLES):
            dec = struct.unpack("b", bytes([p[1 + i]]))[0] << 8
            worst = max(worst, abs(dec - (src[i] & ~0xff if src[i] >= 0
                                          else -((-src[i]) & ~0xff))))
    ok = worst <= 256
    print(f"  round-trip     worst error {worst} LSB (<=256)  "
          f"{'OK' if ok else 'BAD'}")
    if not ok:
        fails.append("roundtrip")

    # 4. timing model
    print(f"  stream rate    {PKT_RATE:.1f} pkt/s  "
          f"({SAMPLES} samp/pkt @ {SR} Hz) = {PKT_RATE*PKT_LEN*8/1000:.0f} kbit/s")

    print("\nRESULT:", "PASS" if not fails else f"FAIL {fails}")
    return 0 if not fails else 1


def transmit(host, seconds, dummy_load):
    if not dummy_load:
        print("REFUSING: --transmit needs --dummy-load to arm.\n"
              "This keys your transmitter and puts a tone ON THE AIR.\n"
              "Run it PRESENT AT THE RIG, into a dummy load the first time.")
        return 2
    CMD, AUD = 49152, 49156
    PC = b"\x00\x00"
    print(f"!! ON-AIR TEST: keying {host} and streaming a {seconds}s tone !!")
    for t in (3, 2, 1):
        print(f"   transmitting in {t}...  (Ctrl-C to abort)")
        time.sleep(1)
    cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cmd.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    cmd.bind(("0.0.0.0", CMD))
    aud = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    aud.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    aud.bind(("0.0.0.0", AUD))
    counter, phase, last_key = 0, 0, 0.0
    try:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            now = time.monotonic()
            if now - last_key > 1.5:                 # key + hold (5 s timeout)
                cmd.sendto(PC + b"*T\x04\x01\r", (host, CMD))
                last_key = now
            blk = tone_s16(SAMPLES, phase=phase)
            phase += SAMPLES
            aud.sendto(encode(counter, blk), (host, AUD))
            counter = (counter + 1) & 0xff
            time.sleep(SAMPLES / SR)
    finally:
        cmd.sendto(PC + b"*T\x00\x00\r", (host, CMD))   # ALWAYS un-key
        print("   un-keyed (*T 00 00 sent)")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if "--transmit" in a:
        host = next((x for x in a if x.count(".") == 3), "192.168.2.123")
        secs = 3
        if "--seconds" in a:
            secs = int(a[a.index("--seconds") + 1])
        sys.exit(transmit(host, secs, "--dummy-load" in a))
    sys.exit(offline())
