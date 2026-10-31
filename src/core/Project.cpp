#include "core/Project.h"
#include "core/Instruments.h"

#include <algorithm>
#include <map>
#include <set>

namespace reader::core {

namespace {
struct StreamRules {
    std::set<int> ksApplied, ksReview;
    int shift = 0;
    bool shiftReview = false, splitReview = false;
};

std::map<std::string, StreamRules> rulesByStream(const Sidecar& sc, const SmfFile& smf) {
    std::map<std::string, StreamRules> out;
    for (auto& s : smf.streams) out[s.id()] = {};
    for (auto& d : sc.decisions) {
        if (d.rule == "channelSplit") {
            if (d.pending() && d.params.contains("streams"))
                for (auto& sid : d.params["streams"]) {
                    auto it = out.find(sid.get<std::string>());
                    if (it != out.end()) it->second.splitReview = true;
                }
            continue;
        }
        auto it = out.find(d.target);
        if (it == out.end()) continue;
        auto& r = it->second;
        if (d.rule == "keySwitch") {
            for (auto& p : d.params["pitches"]) {
                if (d.applied()) r.ksApplied.insert(p.get<int>());
                else if (d.pending()) r.ksReview.insert(p.get<int>());
            }
        } else if (d.rule == "octaveShift") {
            if (d.applied()) r.shift = d.params["shift"].get<int>();
            else if (d.pending()) r.shiftReview = true;
        }
    }
    return out;
}

ProjectedLane projectLane(const std::string& id, const LaneInfo& info, const std::vector<const Stream*>& streams,
                          const std::map<std::string, StreamRules>& rules,
                          const std::map<std::uint32_t, NoteOverride>& overrides) {
    ProjectedLane lane;
    lane.id = id;
    lane.info = info;
    for (auto* st : streams) {
        lane.sources.push_back(st->id());
        const auto& r = rules.at(st->id());
        for (auto& n : st->notes) {
            bool isKs, reviewKs;
            auto ov = overrides.find(n.sourceNote);
            if (ov != overrides.end() && ov->second.keyswitch) {
                isKs = *ov->second.keyswitch;
                reviewKs = false;
            } else {
                isKs = r.ksApplied.count(n.pitch) > 0;
                reviewKs = !isKs && r.ksReview.count(n.pitch) > 0;
            }
            std::uint8_t flags = noteFlagNone;
            int pitch = n.pitch;
            if (isKs) {
                flags |= noteFlagKeySwitch;
            } else {
                if (r.shift) {
                    const int shifted = n.pitch + r.shift;
                    if (shifted >= 0 && shifted <= 127) pitch = shifted;
                    else flags |= noteFlagReview;
                }
                if (reviewKs) flags |= noteFlagReview;
                if (r.shiftReview && !(info.limitLow <= n.pitch && n.pitch <= info.limitHigh)) flags |= noteFlagReview;
                if (!(info.comfortLow <= pitch && pitch <= info.comfortHigh)) flags |= noteFlagSoftRange;
                if (!(info.limitLow <= pitch && pitch <= info.limitHigh)) flags |= noteFlagHardRange;
            }
            if (r.splitReview) flags |= noteFlagReview;
            lane.notes.push_back({n.start, n.length(), pitch, n.velocity, n.sourceNote, flags});
        }
    }
    std::sort(lane.notes.begin(), lane.notes.end(), noteLess);
    for (auto& n : lane.notes) lane.longestNote = std::max(lane.longestNote, n.length);
    return lane;
}
} // namespace

Projection project(const SmfFile& smf, const Sidecar& sc) {
    Projection p;
    p.unitsPerQuarter = smf.ppq;
    p.documentLength = smf.length;
    p.meterChanges = smf.meters;
    if (p.meterChanges.empty()) p.meterChanges.push_back({0, 4, 4});
    if (sc.source.contains("sha256") && sc.source["sha256"].is_string()) {
        const auto sha = sc.source["sha256"].get<std::string>();
        if (!sha.empty() && sha != smf.sha256)
            p.warnings.push_back("sidecar 是对着另一个版本的 MIDI 写的（sha256 不同），按 stream id 尽量套用");
    }
    const auto rules = rulesByStream(sc, smf);
    std::set<std::string> referenced;
    for (auto& spec : sc.lanes) {
        std::vector<const Stream*> streams;
        std::set<int> chunks;
        for (auto& sid : spec.sources) {
            if (auto* st = smf.stream(sid)) {
                streams.push_back(st);
                chunks.insert(st->chunk);
                referenced.insert(sid);
            } else {
                p.warnings.push_back("声部 " + spec.id + "（" + spec.name + "）引用的 " + sid + " 在文件里不存在");
            }
        }
        LaneInfo info;
        info.name = spec.name;
        info.group = spec.group;
        info.colour = spec.colour;
        info.role = spec.role;
        info.sourceTrackCount = std::max(1, (int)chunks.size());
        info.comfortLow = spec.comfortLow;
        info.comfortHigh = spec.comfortHigh;
        info.limitLow = spec.limitLow;
        info.limitHigh = spec.limitHigh;
        p.lanes.push_back(projectLane(spec.id, info, streams, rules, sc.noteOverrides));
    }
    for (auto& st : smf.streams) {
        if (referenced.count(st.id())) continue;
        LaneInfo info;
        const std::string label = st.trackName.empty() ? "轨 " + std::to_string(st.chunk) : st.trackName;
        info.name = label + " · 通道" + std::to_string(st.channel + 1);
        info.group = unassignedGroup();
        info.colour = "#9a9a9a";
        p.lanes.push_back(projectLane("U:" + st.id(), info, {&st}, rules, sc.noteOverrides));
    }
    return p;
}

Json Projection::toJson() const {
    Json j;
    j["format"] = "midi-reader-lanes";
    j["version"] = 1;
    j["unitsPerQuarter"] = unitsPerQuarter;
    j["documentLength"] = documentLength;
    j["meterChanges"] = Json::array();
    for (auto& m : meterChanges) {
        Json e;
        e["tick"] = m.tick;
        e["numerator"] = m.numerator;
        e["denominator"] = m.denominator;
        j["meterChanges"].push_back(e);
    }
    j["warnings"] = warnings;
    j["lanes"] = Json::array();
    for (auto& l : lanes) {
        Json e, info;
        e["id"] = l.id;
        info["name"] = l.info.name;
        info["group"] = l.info.group;
        info["colour"] = l.info.colour;
        info["role"] = l.info.role;
        info["sourceTrackCount"] = l.info.sourceTrackCount;
        info["comfortLow"] = l.info.comfortLow;
        info["comfortHigh"] = l.info.comfortHigh;
        info["limitLow"] = l.info.limitLow;
        info["limitHigh"] = l.info.limitHigh;
        e["info"] = info;
        e["longestNote"] = l.longestNote;
        Json notes = Json::array();
        for (auto& n : l.notes) notes.push_back(Json::array({n.start, n.length, n.pitch, n.velocity, n.sourceNote, n.flags}));
        e["notes"] = notes;
        j["lanes"].push_back(e);
    }
    return j;
}

} // namespace reader::core
