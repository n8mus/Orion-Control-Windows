#!/usr/bin/env bash
# Classic layout guard — proves a change left today's console untouched.
#
#   tests/classic_guard.sh run <console-binary> <out-dir>
#   tests/classic_guard.sh noise <base1> <base2>     (two runs of the same binary)
#   tests/classic_guard.sh compare <baseline-dir> <new-dir>
#
# "run" drives the console headless (Xvfb, pinned font, clean config, no SDR,
# a fake radio on a pty that records every SET command) through
# tests/uiscript/classic.txt for a matrix of cases, and keeps per case:
# the radio transcript, the [uiscript] log (MISSING lines included) and a
# full-window screenshot. "compare" fails on ANY transcript or script-log
# difference, and on screenshots differing beyond anti-aliasing noise
# (writes <case>.diff.png to the new dir for a look).
#
# Safe beside the operator's running console: TTC_NOSDR never touches the
# SDRplay service, the radio is a pty, rigctld uses a test port, and the
# selftest skips the single-instance lock. Nothing can key a transmitter.
set -u
here=$(cd "$(dirname "$0")" && pwd)
mode=${1:-}

run_case() {   # name, extra env..., then the binary/out come from globals
    local name=$1; shift
    local cfg; cfg=$(mktemp -d)
    mkdir -p "$cfg/n8mus"
    # No live spots (cluster / POTA / FreeDV): their content changes run to
    # run (a contest-deck call hint once differed by a fresh spot).
    printf '[display]\nclock=false\nsolar=false\n%s\n[spots]\nenabled=false\ndx=false\npota=false\nfreedv=false\n' \
        "${CASE_CONF:-}" > "$cfg/n8mus/tentec-console.conf"
    local link="$cfg/radio"
    python3 -I "$here/fake_radio.py" "$link" "$out/$name.cat" > /dev/null &
    local fpid=$!
    for _ in $(seq 50); do [ -e "$link" ] && break; sleep 0.1; done
    env "$@" XDG_CONFIG_HOME="$cfg" TTC_NOSDR=1 TTC_SELFTEST=24 TTC_MAXIMIZED=1 TTC_STALLDBG=3000 \
        TTC_RIGCTLD_PORT=14597 TTC_RADIO_DEV="$link" \
        TTC_UISCRIPT="$here/uiscript/classic.txt" TTC_SCREENSHOT="$out/$name.png" \
        timeout 90 xvfb-run -a -s "-screen 0 1920x1080x24" "$bin" \
        > "$out/$name.log" 2>&1
    kill "$fpid" 2>/dev/null; wait "$fpid" 2>/dev/null
    grep '^\[uiscript\]' "$out/$name.log" > "$out/$name.steps"
    grep -q '^\[stall\]' "$out/$name.log" && echo "  $name: GUI STALL — see $out/$name.log"
    rm -rf "$cfg"
    echo "  $name: $(wc -l < "$out/$name.cat") SET commands, $(grep -c MISSING "$out/$name.steps") missing steps"
}

if [ "$mode" = run ]; then
    bin=$(realpath "$2"); out=$(realpath -m "$3"); mkdir -p "$out"
    echo "classic guard: $bin -> $out"
    CASE_CONF=""           run_case orion-gpu   TTC_RADIO=orion TTC_GPUPAN=1
    CASE_CONF=""           run_case orion-cpu   TTC_RADIO=orion TTC_GPUPAN=0
    CASE_CONF=""           run_case omni        TTC_RADIO=omni8 TTC_GPUPAN=1
    CASE_CONF=""           run_case contest     TTC_RADIO=orion TTC_GPUPAN=1 TTC_CONTEST=1
    CASE_CONF="meterStyle=3" run_case meter-cross TTC_RADIO=orion TTC_GPUPAN=1
    CASE_CONF="meterStyle=7" run_case meter-smart TTC_RADIO=orion TTC_GPUPAN=1
    exit 0
fi

if [ "$mode" = noise ]; then
    # Two runs of the SAME binary: pixels that differ between them are
    # clocks/counters, not layout — saved as <case>.noise.png in the first
    # dir and ignored by later compares against it.
    a=$2; b=$3
    for p in "$a"/*.png; do
        case "$p" in *.diff.png|*.noise.png) continue;; esac
        n=$(basename "$p" .png)
        python3 -I - "$p" "$b/$n.png" "$a/$n.noise.png" <<'PY'
import sys
from PIL import Image, ImageChops, ImageFilter
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 40 else 0)
# Each changing spot (a clock, a counter) masked as its whole box plus a
# margin: the digits that change next run sit a few pixels further along.
from PIL import ImageDraw
m = Image.new("L", d.size, 0)
blobs = d.filter(ImageFilter.MaxFilter(31))
bb = blobs.getbbox()
if bb:
    # walk the grown blobs row by row is overkill; their union box is small
    x0, y0, x1, y1 = bb
    ImageDraw.Draw(m).rectangle((x0 - 20, y0 - 6, x1 + 20, y1 + 6), fill=255)
m.save(sys.argv[3]); print(f"  noise mask {sys.argv[3].rsplit('/',1)[-1]}: {m.getbbox()}")
PY
    done
    exit 0
fi

if [ "$mode" = compare ]; then
    base=$2; new=$3; fail=0
    for c in "$base"/*.cat; do
        n=$(basename "$c" .cat)
        if ! diff -q "$c" "$new/$n.cat" > /dev/null; then
            echo "FAIL $n: radio commands differ"; diff "$c" "$new/$n.cat" | head -20; fail=1
        fi
        # The script's own path (which checkout ran it) is not a step.
        steps() { sed 's#^\[uiscript\] .*/\([^/]*\): \([0-9]* lines\)$#[uiscript] \1: \2#' "$1"; }
        if ! diff -q <(steps "$base/$n.steps") <(steps "$new/$n.steps") > /dev/null; then
            echo "FAIL $n: script steps differ"; diff <(steps "$base/$n.steps") <(steps "$new/$n.steps") | head -10; fail=1
        fi
        python3 -I - "$base/$n.png" "$new/$n.png" "$new/$n.diff.png" "$base/$n.noise.png" <<'PY' || fail=1
import os, sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
name = sys.argv[2].rsplit("/", 1)[-1]
if a.size != b.size:
    print(f"FAIL {name}: size {a.size} -> {b.size}"); sys.exit(1)
w, h = a.size
box = (0, 0, w, h - 24)                       # status bar text is timing-dependent
d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 40 else 0)
if os.path.exists(sys.argv[4]):               # known clocks/counters
    m = Image.open(sys.argv[4]).convert("L")
    if m.size == d.size:
        d = ImageChops.subtract(d, m)
d = d.crop(box)
n = sum(1 for v in d.get_flattened_data() if v) if hasattr(d, "get_flattened_data") else sum(1 for v in d.getdata() if v)
if n > 20:                                    # repeat runs measure 0 once clocks are masked
    d.save(sys.argv[3]); print(f"FAIL {name}: {n} pixels differ (see {sys.argv[3]})"); sys.exit(1)
print(f"ok   {name}: {n} pixels differ")
PY
    done
    [ $fail = 0 ] && echo "CLASSIC GUARD: PASS" || echo "CLASSIC GUARD: FAIL"
    exit $fail
fi

echo "usage: $0 run <binary> <out-dir> | noise <base1> <base2> | compare <baseline-dir> <new-dir>"; exit 2
