// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/OrionKeyer.h"
#include "radio/RadioController.h"

#include <QHash>
#include <QTimer>
#include <algorithm>

namespace ttc {

namespace {
// Char -> pattern, the inverse of the decoder's table. Only what the
// Orion's own CW table can send (prg guide p34 lists the procedural
// symbols it maps); anything else is dropped rather than keyed as noise.
const QHash<QChar, QString>& patterns() {
    static const QHash<QChar, QString> t = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
        {'E', "."},    {'F', "..-."}, {'G', "--."},  {'H', "...."},
        {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
        {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"}, {'Z', "--.."},
        {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
        {'8', "---.."}, {'9', "----."},
        {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
        {'=', "-...-"},  {'+', ".-.-."},  {'-', "-....-"}, {':', "---..."},
        {';', "-.-.-."}, {'"', ".-..-."}, {'(', "-.--."},  {')', "-.--.-"},
    };
    return t;
}
} // namespace

// PARIS timing: dit 1 unit, dah 3, 1 unit between elements, 3 between
// characters. A space is not sent to the radio at all (its table has no
// space) — it is only a longer wait, 7 units total for the word gap,
// which is 4 more than the inter-character gap already counted.
int OrionKeyer::charMs(QChar c, int wpm) {
    const double unitMs = 1200.0 / std::clamp(wpm, 5, 99);
    const QChar u = c.toUpper();
    if (u == ' ') return int(4 * unitMs);
    const QString p = patterns().value(u);
    if (p.isEmpty()) return 0;
    int units = 0;
    for (QChar e : p) units += (e == '-') ? 3 : 1;
    units += p.size() - 1;                       // gaps between elements
    units += 3;                                  // gap after the character
    return int(units * unitMs);
}

OrionKeyer::OrionKeyer(RadioController* radio, QObject* parent)
    : CwKeyer(parent), radio_(radio) {
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &OrionKeyer::releaseNext);
}

bool OrionKeyer::open() {
    if (!radio_ || !radio_->connected()) {
        err_ = "no radio connected";
        return false;
    }
    if (!radio_->caps().catCwKeying) {
        err_ = "this radio cannot key CW over CAT";
        return false;
    }
    // Turning the internal keyer on re-reads the KEY jack as a paddle. A
    // WinKeyer left plugged in there then looks like a held dit and runs
    // away. Whoever selects this backend must have closed the WinKeyer
    // first — see CwWindow's backend switch.
    radio_->setCwKeyerEnabled(true);
    radio_->setCwKeyerSpeed(wpm_);
    open_ = true;
    return true;
}

void OrionKeyer::close() {
    stop();
    if (radio_ && open_) radio_->setCwKeyerEnabled(false);
    open_ = false;
}

void OrionKeyer::setSpeed(int wpm) {
    wpm_ = std::clamp(wpm, kCaps.wpmMin, kCaps.wpmMax);
    if (open_ && radio_) radio_->setCwKeyerSpeed(wpm_);
}

void OrionKeyer::send(const QString& text) {
    if (!open_) return;
    for (QChar c : text.toUpper())
        if (c == ' ' || patterns().contains(c)) pending_.enqueue(c);
    if (pending_.isEmpty()) return;
    if (!busy_) { busy_ = true; emit busyChanged(true); }
    if (!timer_->isActive()) releaseNext();      // start immediately
}

void OrionKeyer::releaseNext() {
    if (pending_.isEmpty()) {
        if (busy_) { busy_ = false; emit busyChanged(false); }
        return;
    }
    const QChar c = pending_.dequeue();
    const int ms = charMs(c, wpm_);
    if (c != ' ' && radio_) radio_->sendCwChar(c.toLatin1());
    // Early by kLeadMs so the radio's buffer is never empty mid-word and
    // ITS keyer owns the gap; floor keeps a fast character from
    // free-running the queue.
    timer_->start(std::max(ms - kLeadMs, 20));
}

void OrionKeyer::stop() {
    pending_.clear();
    timer_->stop();
    // Whatever the radio already holds still goes out — *TU will not stop
    // it (measured). Pacing keeps that to about one character.
    if (busy_) { busy_ = false; emit busyChanged(false); }
}

void OrionKeyer::tune(bool on) {
    if (open_ && radio_) radio_->setPtt(on);     // *TK/*TU: steady carrier
}

void OrionKeyer::backspace() {
    if (!pending_.isEmpty()) pending_.removeLast();
}

} // namespace ttc
