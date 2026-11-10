/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "android_pacing.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace lsfgvk::layer;

namespace {
    void expect(int64_t got, int64_t want, const std::string& name) {
        if (got == want)
            return;
        std::cerr << name << ": got " << got << ", want " << want << '\n';
        std::exit(1);
    }
}

int main() {
    expect(nextPresentDeadline(0, 5000000000, 33333333), 5000000000, "first present starts at now");
    expect(nextPresentDeadline(1000, 1000, 100), 1100, "on time adds one interval");
    expect(nextPresentDeadline(1000, 1050, 100), 1100, "late within the interval keeps the cadence");
    expect(nextPresentDeadline(1000, 1150, 100), 1100, "past deadline within one interval is kept");
    expect(nextPresentDeadline(1000, 1200, 100), 1100, "exactly one interval behind is kept");
    expect(nextPresentDeadline(1000, 1201, 100), 1201, "more than one interval behind resets to now");
    expect(nextPresentDeadline(1000, 900, 100), 1100, "early present waits a full interval");
    expect(nextPresentDeadline(1000, 1000, 200), 1200, "a new interval applies at once");

    expect(fpsInterval("60"), 16666666, "60 fps");
    expect(fpsInterval("30"), 33333333, "30 fps");
    expect(fpsInterval("45"), 22222222, "45 fps");
    expect(fpsInterval("60.0"), 0, "a decimal is no limit");
    expect(fpsInterval(" 60"), 0, "leading space is no limit");
    expect(fpsInterval(""), 0, "empty is no limit");
    expect(fpsInterval("0"), 0, "0 is no limit");
    expect(fpsInterval("junk"), 0, "junk is no limit");
    expect(fpsInterval("4294967296"), 0, "out of range is no limit");
    expect(fpsInterval("1000000001"), 0, "above 1e9 fps is no limit");

    std::cerr << "android pacing cases passed\n";
    return 0;
}
