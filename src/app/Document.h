// 一个打开的 MIDI：原文件（只读）+ sidecar + 投影快照。所有用户修正都经过这里，改完立即写回 sidecar。
#pragma once
#include "app/LaneModel.h"
#include "core/Analyze.h"
#include "core/Project.h"
#include "core/Sidecar.h"
#include "core/Smf.h"
#include "core/TempoMap.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <optional>

namespace reader {

// 投影结果的不可变快照，实现 LaneModel。
class ProjectionModel final : public LaneModel {
public:
    ProjectionModel() = default;
    ProjectionModel(const core::Projection&, std::uint64_t revision);
    int unitsPerQuarter() const override { return ppq; }
    double documentLength() const override { return length; }
    std::vector<pianoroll::MeterChange> meterChanges() const override { return meters; }
    int laneCount() const override { return (int)lanes.size(); }
    LaneInfo laneInfo(int lane) const override { return lanes[(size_t)lane].info; }
    juce::String laneId(int lane) const override { return lanes[(size_t)lane].id; }
    const std::vector<LaneNote>& laneNotes(int lane) const override { return lanes[(size_t)lane].notes; }
    double longestNote(int lane) const override { return lanes[(size_t)lane].longest; }
    std::uint64_t revision() const override { return rev; }
    int indexOf(const juce::String& id) const;
private:
    struct Lane { juce::String id; LaneInfo info; std::vector<LaneNote> notes; double longest = 0; };
    int ppq = 480;
    double length = 0;
    std::vector<pianoroll::MeterChange> meters{{0, 4, 4}};
    std::vector<Lane> lanes;
    std::uint64_t rev = 0;
};

// LaneModel 的转发壳：画布持有它的引用，Document 换快照时画布不用换引用。
class LaneModelHandle final : public LaneModel {
public:
    void set(std::shared_ptr<const ProjectionModel> m) { model = std::move(m); }
    const ProjectionModel& current() const { return *model; }
    int unitsPerQuarter() const override { return model->unitsPerQuarter(); }
    double documentLength() const override { return model->documentLength(); }
    std::vector<pianoroll::MeterChange> meterChanges() const override { return model->meterChanges(); }
    int laneCount() const override { return model->laneCount(); }
    LaneInfo laneInfo(int lane) const override { return model->laneInfo(lane); }
    juce::String laneId(int lane) const override { return model->laneId(lane); }
    const std::vector<LaneNote>& laneNotes(int lane) const override { return model->laneNotes(lane); }
    double longestNote(int lane) const override { return model->longestNote(lane); }
    std::uint64_t revision() const override { return model->revision(); }
private:
    std::shared_ptr<const ProjectionModel> model = std::make_shared<ProjectionModel>();
};

class Document {
public:
    Document();
    bool hasFile() const { return smf.has_value(); }
    // 打开 MIDI；返回错误文字，成功返回空
    juce::String open(const juce::File& midi);
    void close();

    const juce::File& file() const { return midiFile; }
    const core::SmfFile& smfFile() const { return *smf; }
    const core::Sidecar& sidecar() const { return sc; }
    const core::Projection& projection() const { return proj; }
    const core::TempoMap& tempoMap() const { return tempo; }
    LaneModelHandle& laneModel() { return handle; }
    juce::String sidecarStatus() const { return saveStatus; }

    // 用户操作。失败时返回错误文字。
    juce::String decide(const juce::String& decisionId, const juce::String& verdict);  // confirmed / rejected / reset
    juce::String setInstrument(const juce::String& laneId, const juce::String& instrumentId);
    juce::String merge(const juce::StringArray& laneIds, const juce::String& name);
    juce::String rename(const juce::String& laneId, const juce::String& name);
    juce::String splitOut(const juce::String& streamId);
    juce::String moveStream(const juce::String& streamId, const juce::String& toLaneId);
    juce::String moveLane(const juce::String& laneId, int newIndex);
    juce::String setNoteKeySwitch(std::vector<std::uint32_t> sourceNotes, std::optional<bool> ks);
    juce::String reanalyzeFromScratch();
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    juce::String undo();
    juce::String redo();

    std::function<void()> onChanged;   // 投影换了（消息线程）

private:
    juce::String apply(const std::function<core::Sidecar(const core::Sidecar&)>&);
    void reproject();
    void save();

    juce::File midiFile;
    std::optional<core::SmfFile> smf;
    core::Sidecar sc;
    core::Projection proj;
    core::TempoMap tempo;
    LaneModelHandle handle;
    std::uint64_t revision = 0;
    juce::String saveStatus;
    std::vector<core::Sidecar> undoStack, redoStack;   // 每次用户操作前的 sidecar
};

juce::String toJuce(const std::string& utf8);
std::string toStd(const juce::String&);

} // namespace reader
