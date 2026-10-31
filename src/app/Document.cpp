#include "app/Document.h"

namespace reader {

juce::String toJuce(const std::string& s) { return juce::String::fromUTF8(s.data(), (int)s.size()); }
std::string toStd(const juce::String& s) { return s.toStdString(); }

ProjectionModel::ProjectionModel(const core::Projection& p, std::uint64_t revision)
    : ppq(p.unitsPerQuarter), length((double)p.documentLength), rev(revision) {
    meters.clear();
    for (auto& m : p.meterChanges) meters.push_back({(double)m.tick, m.numerator, m.denominator});
    for (auto& l : p.lanes) {
        Lane lane;
        lane.id = toJuce(l.id);
        lane.info.name = toJuce(l.info.name);
        lane.info.group = toJuce(l.info.group);
        lane.info.colour = juce::Colour::fromString("ff" + toJuce(l.info.colour).substring(1));
        lane.info.role = LaneRole::unknown;
        lane.info.sourceTrackCount = l.info.sourceTrackCount;
        lane.info.comfortLow = l.info.comfortLow;
        lane.info.comfortHigh = l.info.comfortHigh;
        lane.info.limitLow = l.info.limitLow;
        lane.info.limitHigh = l.info.limitHigh;
        lane.notes.reserve(l.notes.size());
        for (auto& n : l.notes)
            lane.notes.push_back({(double)n.start, (double)n.length, n.pitch, n.velocity, n.sourceNote, n.flags});
        lane.longest = (double)l.longestNote;
        lanes.push_back(std::move(lane));
    }
}

int ProjectionModel::indexOf(const juce::String& id) const {
    for (size_t i = 0; i < lanes.size(); ++i)
        if (lanes[i].id == id) return (int)i;
    return -1;
}

Document::Document() = default;

juce::String Document::open(const juce::File& midi) {
    try {
        auto parsed = core::readSmf(toStd(midi.getFullPathName()));
        const auto scPath = core::sidecarPathFor(toStd(midi.getFullPathName()));
        core::Sidecar next;
        saveStatus = {};
        if (core::fileExists(scPath)) {
            try {
                const auto existing = core::loadSidecar(scPath);
                next = core::analyze(parsed, &existing);
            } catch (const std::exception& e) {
                // 坏 sidecar：不覆盖，另存一份备份后重新分析
                juce::File(toJuce(scPath)).copyFileTo(juce::File(toJuce(scPath + ".broken")));
                next = core::analyze(parsed);
                saveStatus = juce::String::fromUTF8("原 sidecar 读不了（已备份为 .broken）：") + e.what();
            }
        } else {
            next = core::analyze(parsed);
        }
        midiFile = midi;
        undoStack.clear();
        redoStack.clear();
        smf = std::move(parsed);
        sc = std::move(next);
        tempo = core::TempoMap(smf->tempos, smf->ppq);
        save();
        reproject();
        return {};
    } catch (const std::exception& e) {
        return juce::String::fromUTF8("打不开这个 MIDI：") + e.what();
    }
}

void Document::close() {
    smf.reset();
    sc = {};
    proj = {};
    handle.set(std::make_shared<ProjectionModel>());
    if (onChanged) onChanged();
}

void Document::reproject() {
    proj = core::project(*smf, sc);
    handle.set(std::make_shared<ProjectionModel>(proj, ++revision));
    if (onChanged) onChanged();
}

void Document::save() {
    try {
        core::saveSidecar(sc, core::sidecarPathFor(toStd(midiFile.getFullPathName())));
        if (saveStatus.startsWith(juce::String::fromUTF8("保存失败"))) saveStatus = {};
    } catch (const std::exception& e) {
        saveStatus = juce::String::fromUTF8("保存失败：") + e.what();
    }
}

juce::String Document::apply(const std::function<core::Sidecar(const core::Sidecar&)>& op) {
    if (!smf) return juce::String::fromUTF8("没有打开文件");
    try {
        auto next = op(sc);
        undoStack.push_back(std::move(sc));
        if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
        redoStack.clear();
        sc = std::move(next);
    } catch (const std::exception& e) {
        return juce::String::fromUTF8("操作失败：") + e.what();
    }
    save();
    reproject();
    return {};
}

juce::String Document::undo() {
    if (!smf || undoStack.empty()) return juce::String::fromUTF8("没有可撤销的操作");
    redoStack.push_back(std::move(sc));
    sc = std::move(undoStack.back());
    undoStack.pop_back();
    save();
    reproject();
    return juce::String::fromUTF8("已撤销");
}

juce::String Document::redo() {
    if (!smf || redoStack.empty()) return juce::String::fromUTF8("没有可重做的操作");
    undoStack.push_back(std::move(sc));
    sc = std::move(redoStack.back());
    redoStack.pop_back();
    save();
    reproject();
    return juce::String::fromUTF8("已重做");
}

juce::String Document::decide(const juce::String& id, const juce::String& verdict) {
    const auto did = toStd(id);
    if (verdict == "reset") return apply([&](const core::Sidecar& s) { return core::resetUserDecision(*smf, s, did); });
    return apply([&](const core::Sidecar& s) { return core::applyUserDecision(*smf, s, did, toStd(verdict)); });
}
juce::String Document::setInstrument(const juce::String& laneId, const juce::String& inst) {
    return apply([&](const core::Sidecar& s) { return core::setLaneInstrument(*smf, s, toStd(laneId), toStd(inst)); });
}
juce::String Document::merge(const juce::StringArray& ids, const juce::String& name) {
    std::vector<std::string> v;
    for (auto& i : ids) v.push_back(toStd(i));
    return apply([&](const core::Sidecar& s) { return core::mergeLanes(*smf, s, v, toStd(name)); });
}
juce::String Document::rename(const juce::String& laneId, const juce::String& name) {
    return apply([&](const core::Sidecar& s) { return core::renameLane(*smf, s, toStd(laneId), toStd(name)); });
}
juce::String Document::splitOut(const juce::String& streamId) {
    return apply([&](const core::Sidecar& s) { return core::splitOutStream(*smf, s, toStd(streamId)); });
}
juce::String Document::moveStream(const juce::String& streamId, const juce::String& to) {
    return apply([&](const core::Sidecar& s) { return core::moveStream(*smf, s, toStd(streamId), toStd(to)); });
}
juce::String Document::moveLane(const juce::String& laneId, int newIndex) {
    return apply([&](const core::Sidecar& s) { return core::moveLane(*smf, s, toStd(laneId), newIndex); });
}
juce::String Document::setNoteKeySwitch(std::vector<std::uint32_t> notes, std::optional<bool> ks) {
    return apply([&](const core::Sidecar& s) { return core::setNoteKeySwitch(*smf, s, notes, ks); });
}
juce::String Document::reanalyzeFromScratch() {
    return apply([&](const core::Sidecar& s) { return core::analyze(*smf, &s, true); });
}

} // namespace reader
