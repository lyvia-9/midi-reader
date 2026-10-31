// 投影层：原 MIDI + sidecar → 逻辑声部。对照 proto/reader_proto/project.py。
#pragma once
#include "core/Model.h"
#include "core/Sidecar.h"
#include "core/Smf.h"

#include <string>
#include <vector>

namespace reader::core {

struct ProjectedLane {
    std::string id;                    // "L3"；未归类为 "U:t3c0"
    LaneInfo info;
    std::vector<LaneNote> notes;       // 按 (start, pitch, sourceNote) 升序
    std::vector<std::string> sources;  // 实际找到的 stream id
    std::int64_t longestNote = 0;
    bool unassigned() const { return id.rfind("U:", 0) == 0; }
};

struct Projection {
    int unitsPerQuarter = 480;
    std::int64_t documentLength = 0;
    std::vector<MeterChange> meterChanges;
    std::vector<ProjectedLane> lanes;
    std::vector<std::string> warnings;
    Json toJson() const;   // *.lanes.json 的内容
};

Projection project(const SmfFile&, const Sidecar&);

} // namespace reader::core
