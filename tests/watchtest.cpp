// SPDX-License-Identifier: GPL-2.0-or-later
// WatchList pattern-matching verification (pure logic, no I/O):
//   cmake --build build --target watchtest && ./build/watchtest
#include <cstdio>

#include "util/WatchList.h"

using ttc::WatchList;

static int fails = 0;

static void check(bool got, bool want, const char* what) {
    if (got != want) {
        printf("FAIL: %s (got %d, wanted %d)\n", what, got, want);
        ++fails;
    }
}

int main() {
    WatchList w;
    w.setPatterns("W1AW, ja*\nUS-1234 NEW vp8*");
    check(w.count() == 5, true, "pattern count");

    check(w.matches("W1AW", {}, '?'), true, "exact call");
    check(w.matches("w1aw", {}, '?'), true, "exact call, lower case");
    check(w.matches("W1AWX", {}, '?'), false, "exact must not prefix-match");
    check(w.matches("JA3XYZ", {}, 'W'), true, "JA* prefix");
    check(w.matches("VP8ABC", {}, 'C'), true, "VP8* prefix");
    check(w.matches("K1JA", {}, '?'), false, "prefix anchors at the start");
    check(w.matches("N0XYZ", "US-1234", 'W'), true, "park reference");
    check(w.matches("N0XYZ", "US-9999", 'W'), false, "other park");
    check(w.matches("ZL1AAA", {}, 'N'), true, "NEW matches never-worked");
    check(w.matches("ZL1AAA", {}, 'B'), false, "NEW ignores worked");

    WatchList all;
    all.setPatterns("*");
    check(all.matches("ANY1XYZ", {}, '?'), true, "star matches everything");

    WatchList none;
    check(none.matches("W1AW", {}, 'N'), false, "empty list never matches");

    printf(fails ? "WATCHTEST: %d FAILURE(S)\n" : "WATCHTEST: PASS\n", fails);
    return fails ? 1 : 0;
}
