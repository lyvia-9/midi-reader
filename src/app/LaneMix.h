// 静音 / 独奏。只影响播放，不写进 sidecar（关掉程序就没了）。
// 按声部 id 记，所以重新整理、撤销、挪动顺序之后还对得上；消失的声部在 prune 时去掉，
// 免得“独奏了一条已经不存在的声部”让全曲都不出声。
#pragma once
#include "app/LaneModel.h"

#include <cstdint>
#include <set>
#include <vector>

namespace reader {

enum LaneMixFlag : std::uint8_t { mixMuted = 1, mixSoloed = 2, mixSilent = 4 };

struct LaneMix {
    std::set<juce::String> muted, soloed;

    bool any() const { return !muted.empty() || !soloed.empty(); }
    bool audible(const juce::String& id) const {
        if (muted.count(id)) return false;
        return soloed.empty() || soloed.count(id) > 0;
    }
    void toggleMute(const juce::String& id) {
        if (!muted.erase(id)) muted.insert(id);
    }
    // exclusive：只听这一条（已经是唯一独奏时再按一次 = 取消）
    void toggleSolo(const juce::String& id, bool exclusive) {
        if (exclusive) {
            const bool onlyThis = soloed.size() == 1 && soloed.count(id);
            soloed.clear();
            if (!onlyThis) soloed.insert(id);
            muted.erase(id);
            return;
        }
        if (!soloed.erase(id)) soloed.insert(id);
    }
    void clear() { muted.clear(); soloed.clear(); }
    void prune(const LaneModel& m) {
        std::set<juce::String> ids;
        for (int i = 0; i < m.laneCount(); ++i) ids.insert(m.laneId(i));
        for (auto* s : {&muted, &soloed})
            for (auto it = s->begin(); it != s->end();) it = ids.count(*it) ? std::next(it) : s->erase(it);
    }
    // 每条声部一个 LaneMixFlag 组合，画布和左栏照着画
    std::vector<std::uint8_t> flags(const LaneModel& m) const {
        std::vector<std::uint8_t> out((size_t)m.laneCount(), 0);
        for (int i = 0; i < m.laneCount(); ++i) {
            const auto id = m.laneId(i);
            auto& f = out[(size_t)i];
            if (muted.count(id)) f |= mixMuted;
            if (soloed.count(id)) f |= mixSoloed;
            if (!audible(id)) f |= mixSilent;
        }
        return out;
    }
};

} // namespace reader
