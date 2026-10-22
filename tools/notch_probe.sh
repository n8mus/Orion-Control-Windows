#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Probe the Orion v3 firmware for undocumented manual-notch CAT commands.
# Run while NO console instance holds /dev/orion (TIOCEXCL).
#
# Background: the V3 user-manual addendum (TT 74474) documents *RMNS<0/1> /
# ?RMNS for SAF and says SAF "uses the same values for NOTCH Center Frequency
# and Width parameters. So no new commands are required" — i.e. CF/width CAT
# commands already existed (V2 firmware era) but appear in no public document.
# Candidates below extend the *RMN<x> group, whose known letters are
# A(uto-notch) B(lanker) N(oise reduction) S(AF). Queries only — harmless.
set -e
DEV=${1:-/dev/orion}
PROBE=$(dirname "$0")/../build/cat_probe
[ -x "$PROBE" ] || PROBE=/tmp/cat_probe

# Documented control first (proves the group answers queries at all), then
# candidate letters for notch Center frequency / Width / manual notch state,
# then the NR/NB/auto-notch levels (also undocumented as queries).
"$PROBE" "$DEV" -burst \
    "?RMNS" \
    "?RMNC" "?RMNW" "?RMNF" "?RMNM" "?RMND" \
    "?RMNN" "?RMNB" "?RMNA" \
    "?RMN"
