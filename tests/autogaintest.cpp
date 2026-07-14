// SPDX-License-Identifier: GPL-2.0-or-later
// AutoGain policy state machine, driven with synthetic peak/time
// sequences (attack speed, release patience, ping-pong lockout).
//   cmake --build build --target autogaintest && ./build/autogaintest
#include <cstdio>

#include "sdr/AutoGain.h"

using ttc::AutoGain;

static int fails = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++fails;
    }
}

int main() {
    {   // Attack: near-clipping steps up fast, but honors the hold time.
        AutoGain g;
        g.reset(3, 0);
        auto d = g.tick(-2.0, 3000);
        check(d.step && d.lna == 4, "clipping steps 3->4");
        d = g.tick(-2.0, 4000);
        check(!d.step, "hold time blocks an immediate second step");
        d = g.tick(-2.0, 6000);
        check(d.step && d.lna == 5, "persistent clipping keeps stepping");
        d = g.tick(-8.0, 9000);
        check(!d.step, "sweet spot: no step");
    }
    {   // Release: 30 s of quiet earns ONE step; each step earns its own.
        AutoGain g;
        g.reset(4, 0);
        qint64 t = 3000;
        AutoGain::Decision d;
        for (; t < 32000; t += 1000) {
            d = g.tick(-20.0, t);
            check(!d.step || t >= 33000, "no release before 30 s");
        }
        d = g.tick(-20.0, 34000);
        check(d.step && d.lna == 3, "30 s quiet releases 4->3");
        d = g.tick(-20.0, 40000);
        check(!d.step, "next release needs its own 30 s");
        d = g.tick(-20.0, 64500);
        check(d.step && d.lna == 2, "another 30 s releases 3->2");
    }
    {   // Mid-zone resets the quiet clock.
        AutoGain g;
        g.reset(4, 0);
        for (qint64 t = 3000; t < 25000; t += 1000) g.tick(-20.0, t);
        g.tick(-10.0, 25000);              // one sweet-spot reading
        auto d = g.tick(-20.0, 34000);
        check(!d.step, "sweet-spot reading resets release patience");
    }
    {   // Ping-pong lockout: after an attack, don't release below the
        // clipping level again for 10 minutes.
        AutoGain g;
        g.reset(2, 0);
        auto d = g.tick(-1.0, 3000);
        check(d.step && d.lna == 3, "attack 2->3");
        qint64 t = 4000;
        for (; t < 40000; t += 1000) {
            d = g.tick(-20.0, t);
            check(!d.step, "lockout holds despite quiet");
        }
        // ... but far past the lockout, releases work again (one at expiry,
        // a second earned by its own 30 s of quiet).
        for (t = 610000; t < 645000; t += 1000) g.tick(-20.0, t);
        check(g.lna() == 1, "releases resume after lockout expires");
    }
    printf(fails ? "AUTOGAINTEST: %d FAILURE(S)\n" : "AUTOGAINTEST: PASS\n",
           fails);
    return fails ? 1 : 0;
}
