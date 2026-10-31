#include "core/Analyze.h"
#include "core/Instruments.h"
#include "core/Rules.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

namespace reader::core {

namespace {
constexpr int maxRounds = 3;

std::string S(long long v) { return std::to_string(v); }

std::string fileNameOf(const std::string& path) {
#if defined(__cpp_char8_t)
    const auto u = std::filesystem::path(std::u8string(path.begin(), path.end())).filename().u8string();
    return std::string(u.begin(), u.end());
#else
    return std::filesystem::u8path(path).filename().u8string();
#endif
}

void setInstrument(LaneSpec& spec, const Instrument* inst, const std::string& src) {
    spec.instrument = inst ? inst->id : "";
    spec.instrumentSource = src;
    spec.comfortLow = 0; spec.comfortHigh = 127; spec.limitLow = 0; spec.limitHigh = 127;
    if (inst) {
        spec.group = groupName(inst->group);
        spec.colour = groupColour(inst->group);
        if (inst->hasRange) {
            spec.comfortLow = inst->comfortLow; spec.comfortHigh = inst->comfortHigh;
            spec.limitLow = inst->limitLow; spec.limitHigh = inst->limitHigh;
        }
    } else {
        spec.group = "";
        spec.colour = groupColour("other");
    }
}

LaneSpec laneFor(const std::vector<const Stream*>& streams, bool split) {
    const Stream& st = *streams.front();
    std::pair<const Instrument*, std::string> g;
    if (split) {
        g = guessInstrument("", st.program, st.isDrum());
        if (!g.first) g = guessInstrument(st.trackName, st.program, st.isDrum());
    } else {
        const bool allDrum = std::all_of(streams.begin(), streams.end(), [](const Stream* s) { return s->isDrum(); });
        g = guessInstrument(st.trackName, st.program, allDrum);
    }
    std::string name;
    if (split) {
        const std::string base = st.trackName.empty() ? "轨 " + S(st.chunk) : st.trackName;
        name = g.first ? base + " · " + g.first->nameZh : base + " · 通道" + S(st.channel + 1);
    } else {
        name = !st.trackName.empty() ? st.trackName : (g.first ? g.first->nameZh : "轨 " + S(st.chunk));
    }
    LaneSpec spec;
    spec.name = name;
    for (auto* s : streams) spec.sources.push_back(s->id());
    setInstrument(spec, g.first, g.second);
    return spec;
}

std::vector<LaneSpec> buildLanes(const SmfFile& smf, const std::map<std::string, Decision>& splits,
                                 const std::set<std::string>* only) {
    struct Built { std::pair<int, int> key; int order; LaneSpec spec; };
    std::vector<Built> built;
    int order = 0;
    for (auto& chunk : smf.chunks) {
        std::vector<const Stream*> streams;
        for (auto* s : smf.chunkStreams(chunk.index))
            if (!only || only->count(s->id())) streams.push_back(s);
        if (streams.empty()) continue;
        auto it = splits.find("split:t" + S(chunk.index));
        std::vector<std::vector<const Stream*>> groups;
        if (it != splits.end() && it->second.applied())
            for (auto* s : streams) groups.push_back({s});
        else
            groups.push_back(streams);
        for (auto& g : groups) {
            auto spec = laneFor(g, groups.size() > 1);
            const Instrument* inst = spec.instrument.empty() ? nullptr : instrumentById(spec.instrument);
            built.push_back({scoreOrderKey(inst), order++, spec});
        }
    }
    std::stable_sort(built.begin(), built.end(), [](const Built& a, const Built& b) {
        return std::tie(a.key, a.order) < std::tie(b.key, b.order);
    });
    std::vector<LaneSpec> out;
    for (auto& b : built) out.push_back(b.spec);
    return out;
}

bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

void assignIdsAndColours(std::vector<LaneSpec>& lanes, const std::vector<LaneSpec>& existing) {
    std::set<std::string> used;
    long long n = 0;
    std::map<std::string, int> perGroup;
    for (auto& l : existing) {
        used.insert(l.id);
        if (l.id.size() > 1 && isDigits(l.id.substr(1))) n = std::max(n, std::stoll(l.id.substr(1)));
        ++perGroup[l.group];
    }
    for (auto& l : lanes) {
        ++n;
        while (used.count("L" + S(n))) ++n;
        l.id = "L" + S(n);
        used.insert(l.id);
        const int i = perGroup[l.group]++;
        l.colour = shadeColour(l.colour, i);
    }
}

std::vector<Decision> runStreamRules(const SmfFile& smf, const Stream& stream, const LaneSpec* lane,
                                     const std::map<std::string, Decision>& kept,
                                     const std::map<std::uint32_t, bool>& forced) {
    const Instrument* inst = (lane && !lane->instrument.empty()) ? instrumentById(lane->instrument) : nullptr;
    const std::string src = lane ? lane->instrumentSource : "";
    std::map<std::uint32_t, bool> forcedHere;
    for (auto& n : stream.notes)
        if (auto it = forced.find(n.sourceNote); it != forced.end()) forcedHere[n.sourceNote] = it->second;
    const std::string octId = "oct:" + stream.id();
    int shift = 0;
    if (auto it = kept.find(octId); it != kept.end() && it->second.applied()) shift = it->second.params.value("shift", 0);
    std::vector<Decision> ksDecs;
    std::optional<Decision> octDec;
    for (int round = 0; round < maxRounds; ++round) {
        const auto computed = rules::keySwitch(stream, smf.ppq, inst, src, shift, forcedHere);
        ksDecs.clear();
        for (const char* side : {"low", "high"}) {
            const std::string kid = "ks:" + stream.id() + ":" + side;
            if (auto it = kept.find(kid); it != kept.end()) { ksDecs.push_back(it->second); continue; }
            for (auto& d : computed)
                if (d.params["side"] == side) ksDecs.push_back(d);
        }
        std::set<int> excluded;
        for (auto& d : ksDecs)
            if (d.status != statusRejected)
                for (auto& p : d.params["pitches"]) excluded.insert(p.get<int>());
        std::vector<RawNote> playing;
        for (auto& n : stream.notes) {
            auto f = forcedHere.find(n.sourceNote);
            if (f != forcedHere.end() && f->second) continue;
            if (f != forcedHere.end() || !excluded.count(n.pitch)) playing.push_back(n);
        }
        if (auto it = kept.find(octId); it != kept.end()) octDec = it->second;
        else octDec = rules::octaveShift(stream, inst, src, playing);
        const int newShift = (octDec && octDec->applied()) ? octDec->params["shift"].get<int>() : 0;
        if (newShift == shift) break;
        shift = newShift;
    }
    if (octDec) ksDecs.push_back(*octDec);
    return ksDecs;
}

std::map<std::uint32_t, bool> forcedKeySwitch(const Sidecar& sc) {
    std::map<std::uint32_t, bool> out;
    for (auto& [k, v] : sc.noteOverrides)
        if (v.keyswitch) out[k] = *v.keyswitch;
    return out;
}
} // namespace

std::string shadeColour(const std::string& hex, int i) {
    if (i == 0 || hex.size() != 7) return hex;
    const int k = ((i + 1) / 2 - 1) % 3 + 1;
    const int amount = 12 * k;
    int rgb[3];
    for (int c = 0; c < 3; ++c) {
        int v = std::stoi(hex.substr(1 + c * 2, 2), nullptr, 16);
        if (i % 2) v = v + (255 - v) * amount / 100;
        else v = v * (100 - amount) / 100;
        rgb[c] = v;
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    return buf;
}

Sidecar analyze(const SmfFile& smf, const Sidecar* existing, bool reset) {
    std::map<std::string, Decision> kept;
    std::vector<std::string> keptOrder;
    Sidecar out;
    if (existing) {
        for (auto& d : existing->decisions)
            if (d.userSet()) { kept[d.id] = d; keptOrder.push_back(d.id); }
        out.noteOverrides = existing->noteOverrides;
    }
    std::map<std::string, Decision> splits;
    for (auto& d : rules::channelSplit(smf)) {
        auto it = kept.find(d.id);
        splits[d.id] = it != kept.end() ? it->second : d;
    }

    if (existing && !reset) {
        out.lanes = existing->lanes;
        std::set<std::string> referenced, missing;
        for (auto& l : out.lanes) referenced.insert(l.sources.begin(), l.sources.end());
        for (auto& s : smf.streams)
            if (!referenced.count(s.id())) missing.insert(s.id());
        if (!missing.empty()) {
            auto added = buildLanes(smf, splits, &missing);
            assignIdsAndColours(added, out.lanes);
            out.lanes.insert(out.lanes.end(), added.begin(), added.end());
        }
    } else {
        out.lanes = buildLanes(smf, splits, nullptr);
        assignIdsAndColours(out.lanes, {});
    }

    std::map<std::string, const LaneSpec*> owner;
    for (auto& l : out.lanes)
        for (auto& s : l.sources) owner[s] = &l;
    const auto forced = forcedKeySwitch(out);
    std::set<std::string> produced;
    for (auto& chunk : smf.chunks) {
        if (auto it = splits.find("split:t" + S(chunk.index)); it != splits.end()) {
            out.decisions.push_back(it->second);
            produced.insert(it->first);
        }
        for (auto* st : smf.chunkStreams(chunk.index)) {
            auto ow = owner.find(st->id());
            for (auto& d : runStreamRules(smf, *st, ow == owner.end() ? nullptr : ow->second, kept, forced)) {
                produced.insert(d.id);
                out.decisions.push_back(d);
            }
        }
    }
    // Python 按 kept 字典的插入顺序（= 原 sidecar 里的顺序）追加目标已不存在的用户结论
    for (auto& id : keptOrder)
        if (!produced.count(id)) out.decisions.push_back(kept[id]);

    out.source = Json::object();
    out.source["fileName"] = fileNameOf(smf.path);
    out.source["sha256"] = smf.sha256;
    out.source["format"] = smf.format;
    out.source["ppq"] = smf.ppq;
    out.source["noteCount"] = smf.noteCount();
    out.streams = Json::array();
    for (auto& s : smf.streams) {
        Json j;
        j["id"] = s.id();
        j["chunk"] = s.chunk;
        j["channel"] = s.channel;
        j["trackName"] = s.trackName;
        j["program"] = s.program >= 0 ? Json(s.program) : Json(nullptr);
        j["noteCount"] = s.notes.size();
        out.streams.push_back(j);
    }
    return out;
}

Sidecar applyUserDecision(const SmfFile& smf, Sidecar sc, const std::string& id, const std::string& verdict) {
    auto* d = sc.decision(id);
    if (!d) throw std::out_of_range(id);
    d->status = verdict;
    if (d->rule == "channelSplit" && verdict == statusConfirmed) {
        const auto targetsVec = d->params["streams"].get<std::vector<std::string>>();
        const std::set<std::string> targets(targetsVec.begin(), targetsVec.end());
        for (std::size_t i = 0; i < sc.lanes.size(); ++i) {
            const std::set<std::string> src(sc.lanes[i].sources.begin(), sc.lanes[i].sources.end());
            if (src != targets) continue;
            std::map<std::string, Decision> one{{d->id, *d}};
            auto added = buildLanes(smf, one, &targets);
            std::vector<LaneSpec> others;
            for (std::size_t j = 0; j < sc.lanes.size(); ++j)
                if (j != i) others.push_back(sc.lanes[j]);
            assignIdsAndColours(added, others);
            sc.lanes.erase(sc.lanes.begin() + (long)i);
            sc.lanes.insert(sc.lanes.begin() + (long)i, added.begin(), added.end());
            break;
        }
    }
    return analyze(smf, &sc);
}

Sidecar resetUserDecision(const SmfFile& smf, Sidecar sc, const std::string& id) {
    auto it = std::find_if(sc.decisions.begin(), sc.decisions.end(), [&](const Decision& d) { return d.id == id; });
    if (it == sc.decisions.end()) throw std::out_of_range(id);
    sc.decisions.erase(it);
    return analyze(smf, &sc);
}

Sidecar setLaneInstrument(const SmfFile& smf, Sidecar sc, const std::string& laneId, const std::string& instId) {
    auto* lane = sc.lane(laneId);
    if (!lane) throw std::out_of_range(laneId);
    const Instrument* inst = instId.empty() ? nullptr : instrumentById(instId);
    if (!instId.empty() && !inst) throw std::out_of_range(instId);
    setInstrument(*lane, inst, inst ? "user" : "");
    const std::set<std::string> src(lane->sources.begin(), lane->sources.end());
    sc.decisions.erase(std::remove_if(sc.decisions.begin(), sc.decisions.end(),
                                      [&](const Decision& d) { return src.count(d.target) && !d.userSet(); }),
                       sc.decisions.end());
    return analyze(smf, &sc);
}

Sidecar mergeLanes(const SmfFile& smf, Sidecar sc, const std::vector<std::string>& ids, const std::string& name) {
    const std::set<std::string> idset(ids.begin(), ids.end());
    LaneSpec* first = nullptr;
    int count = 0;
    for (auto& l : sc.lanes) {
        if (!idset.count(l.id)) continue;
        ++count;
        if (!first) first = &l;
        else first->sources.insert(first->sources.end(), l.sources.begin(), l.sources.end());
    }
    if (count < 2) throw std::out_of_range("need at least two existing lane ids");
    if (!name.empty()) first->name = name;
    const std::string keep = first->id;
    sc.lanes.erase(std::remove_if(sc.lanes.begin(), sc.lanes.end(),
                                  [&](const LaneSpec& l) { return l.id != keep && idset.count(l.id); }),
                   sc.lanes.end());
    return analyze(smf, &sc);
}

Sidecar renameLane(const SmfFile& smf, Sidecar sc, const std::string& laneId, const std::string& name) {
    auto* lane = sc.lane(laneId);
    if (!lane) throw std::out_of_range(laneId);
    lane->name = name;
    return analyze(smf, &sc);
}

Sidecar splitOutStream(const SmfFile& smf, Sidecar sc, const std::string& streamId) {
    for (std::size_t i = 0; i < sc.lanes.size(); ++i) {
        auto& src = sc.lanes[i].sources;
        auto it = std::find(src.begin(), src.end(), streamId);
        if (it == src.end()) continue;
        if (src.size() < 2) return sc;   // 已经单独一条
        src.erase(it);
        const auto* st = smf.stream(streamId);
        LaneSpec spec;
        if (st) {
            spec = laneFor({st}, true);
        } else {
            spec.name = streamId;
            spec.sources = {streamId};
            setInstrument(spec, nullptr, "");
        }
        std::vector<LaneSpec> added{spec};
        assignIdsAndColours(added, sc.lanes);
        sc.lanes.insert(sc.lanes.begin() + (long)i + 1, added.front());
        return analyze(smf, &sc);
    }
    throw std::out_of_range(streamId);
}

Sidecar moveStream(const SmfFile& smf, Sidecar sc, const std::string& streamId, const std::string& toLaneId) {
    auto* to = sc.lane(toLaneId);
    if (!to) throw std::out_of_range(toLaneId);
    for (auto& l : sc.lanes) {
        auto it = std::find(l.sources.begin(), l.sources.end(), streamId);
        if (it != l.sources.end()) l.sources.erase(it);
    }
    to = sc.lane(toLaneId);
    to->sources.push_back(streamId);
    // 空了的声部去掉
    sc.lanes.erase(std::remove_if(sc.lanes.begin(), sc.lanes.end(), [](const LaneSpec& l) { return l.sources.empty(); }),
                   sc.lanes.end());
    return analyze(smf, &sc);
}

Sidecar moveLane(const SmfFile& smf, Sidecar sc, const std::string& laneId, int newIndex) {
    auto it = std::find_if(sc.lanes.begin(), sc.lanes.end(), [&](const LaneSpec& l) { return l.id == laneId; });
    if (it == sc.lanes.end()) throw std::out_of_range(laneId);
    LaneSpec l = *it;
    sc.lanes.erase(it);
    newIndex = std::clamp(newIndex, 0, (int)sc.lanes.size());
    sc.lanes.insert(sc.lanes.begin() + newIndex, l);
    return analyze(smf, &sc);
}

Sidecar setNoteKeySwitch(const SmfFile& smf, Sidecar sc, const std::vector<std::uint32_t>& notes, std::optional<bool> ks) {
    for (auto n : notes) {
        if (ks) sc.noteOverrides[n].keyswitch = ks;
        else sc.noteOverrides.erase(n);
    }
    return analyze(smf, &sc);
}

} // namespace reader::core
