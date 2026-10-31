// tick ↔ 秒。播放引擎吃秒，画布用 tick；换算在阅读器主程序里做（接口设计文档“还没定的问题”最后一条）。
#pragma once
#include "core/Model.h"

#include <vector>

namespace reader::core {

class TempoMap {
public:
    TempoMap() = default;
    TempoMap(const std::vector<TempoChange>&, int ppq);
    double secondsAt(double tick) const;
    double tickAt(double seconds) const;
private:
    struct Seg { double tick, seconds, secondsPerTick; };
    std::vector<Seg> segs{{0, 0, 0.5 / 480}};
};

} // namespace reader::core
