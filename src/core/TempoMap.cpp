#include "core/TempoMap.h"

#include <algorithm>

namespace reader::core {

TempoMap::TempoMap(const std::vector<TempoChange>& tempos, int ppq) {
    const double q = ppq > 0 ? ppq : 480;
    segs.clear();
    segs.push_back({0, 0, 0.5 / q});   // 没有速度事件时默认 120 BPM
    for (auto& t : tempos) {
        const double spt = (t.microsPerQuarter > 0 ? t.microsPerQuarter : 500000) / 1e6 / q;
        auto& last = segs.back();
        const double tick = (double)t.tick;
        if (tick <= last.tick) {
            last.secondsPerTick = spt;
            continue;
        }
        const double sec = last.seconds + (tick - last.tick) * last.secondsPerTick;
        segs.push_back({tick, sec, spt});
    }
}

double TempoMap::secondsAt(double tick) const {
    auto it = std::upper_bound(segs.begin(), segs.end(), tick, [](double t, const Seg& s) { return t < s.tick; });
    const auto& s = it == segs.begin() ? segs.front() : *(it - 1);
    return s.seconds + (tick - s.tick) * s.secondsPerTick;
}

double TempoMap::tickAt(double seconds) const {
    auto it = std::upper_bound(segs.begin(), segs.end(), seconds, [](double t, const Seg& s) { return t < s.seconds; });
    const auto& s = it == segs.begin() ? segs.front() : *(it - 1);
    return s.tick + (seconds - s.seconds) / s.secondsPerTick;
}

} // namespace reader::core
