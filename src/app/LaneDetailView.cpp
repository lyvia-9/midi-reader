#include "app/LaneDetailView.h"
#include "app/Theme.h"

namespace reader {

using namespace pianoroll;

class LaneDetailWindow::Host final : public PianoRollHost {
public:
    Host(Document& d, Transport& t) : doc(d), transport(t) {}
    std::vector<LaneNote> notes;
    juce::Colour colour;
    bool showKs = false;
    int ppq = 480;
    double length = 0;
    PianoRollView* view = nullptr;

    void load(int lane) {
        auto& m = doc.laneModel();
        notes.clear();
        for (auto& n : m.laneNotes(lane))
            if (showKs || !(n.flags & noteFlagKeySwitch)) notes.push_back(n);
        colour = m.laneInfo(lane).colour;
        ppq = m.unitsPerQuarter();
        length = m.documentLength();
    }

    // 文档与时间
    bool hasDocument() const override { return true; }
    bool busy() const override { return false; }
    int unitsPerQuarter() const override { return ppq; }
    double documentLength() const override { return length; }
    double maximumTime() const override { return length + ppq * 8.0; }
    double roundTime(double t) const override { return std::round(t); }
    std::vector<std::size_t> noteIndicesInRange(double a, double b) const override {
        std::vector<std::size_t> r;
        for (std::size_t i = 0; i < notes.size(); ++i)
            if (notes[i].start <= b && notes[i].start + notes[i].length >= a) r.push_back(i);
        return r;
    }
    ViewLayout viewLayout(int w, int h, bool v, bool c, float nf, float vf) const override {
        auto r = defaultViewLayout(w, h, v, c, nf, vf);
        r.undo = r.redo = r.pencil = r.pointer = r.snap = r.edit = {};   // 只读：去掉编辑工具
        r.fit = r.expand = r.editTrack = r.channel = r.allTracks = r.controller = r.drag = {};
        return r;
    }

    // 音符
    std::size_t noteCount() const override { return notes.size(); }
    ViewNote note(std::size_t i) const override {
        ViewNote n;
        n.start = notes[i].start;
        n.length = notes[i].length;
        n.pitch = notes[i].pitch;
        n.velocity = notes[i].velocity;
        n.editable = false;
        return n;
    }
    bool noteSelected(std::size_t) const override { return false; }
    bool hasSelection() const override { return false; }
    std::function<bool(std::size_t, juce::String&)> noteLabels() const override {
        return [](std::size_t, juce::String&) { return false; };
    }
    std::function<bool(std::size_t)> keySwitches() const override {
        return [this](std::size_t i) { return (notes[i].flags & noteFlagKeySwitch) != 0; };
    }
    bool groupAlive(int) const override { return true; }
    bool groupInScope(int) const override { return true; }
    bool pitchesFolded() const override { return false; }
    bool rowHighlighted(int) const override { return false; }
    bool paintNote(juce::Graphics& g, std::size_t i, const ViewNote&, juce::Rectangle<float> r, bool, bool) const override {
        const auto f = notes[i].flags;
        auto c = (f & noteFlagKeySwitch) ? theme::keySwitch
               : (f & noteFlagHardRange) ? theme::hard
               : (f & (noteFlagReview | noteFlagSoftRange)) ? theme::warn : colour;
        g.setColour(c.withMultipliedAlpha(.35f + .65f * notes[i].velocity / 127.f));
        g.fillRoundedRectangle(r, 2);
        g.setColour(c.brighter(.4f));
        g.drawRoundedRectangle(r, 2, 1);
        return true;
    }
    juce::String emptyDocumentMessage() const override { return juce::String::fromUTF8("这条声部没有音符"); }

    // 控制器栏：不显示内容
    int controller() const override { return -1; }
    int channel() const override { return 0; }
    std::vector<LanePoint> controllerLane() const override { return {}; }

    // 播放
    double playheadTime() const override { return transport.positionTick(); }
    bool loopRange(double&, double&) const override { return false; }
    void seek(double t) override { transport.seekTick(t); }
    void setLoop(double, double) override {}
    void clearLoop() override {}
    bool canUndo() const override { return false; }
    bool canRedo() const override { return false; }

    // 编辑：只读，一律空实现
    void gestureStarted(double) override {}
    void beginEdit() override {}
    void abandonEdit() override {}
    void editChanged() override {}
    void commitEdit() override {}
    void baselineCaptured() override {}
    void startSelectionCaptured() override {}
    bool startSelected(std::size_t) const override { return false; }
    void focusGroupOf(std::size_t) override {}
    void moveNote(std::size_t, double, int) override {}
    void setNoteLength(std::size_t, double) override {}
    void setNoteTiming(std::size_t, double, double) override {}
    void setNoteVelocity(std::size_t, int) override {}
    void eraseNote(std::size_t) override {}
    bool canCreateNote() const override { return false; }
    std::size_t createNote(double, int, double, int) override { return noIndex; }
    void appendBaselineCopy(std::size_t) override {}
    void clearSelection() override {}
    void selectNote(std::size_t) override {}
    void deselectNote(std::size_t) override {}
    void restoreStartSelection(bool) override {}
    bool canDrawController() const override { return false; }
    void beginControllerStroke(double, bool) override {}
    bool controllerStrokeOpen() const override { return false; }
    void updateControllerStroke(double, double, const std::map<double, int>&, bool) override {}
    bool followMovedNotes(bool) override { return false; }
    bool linkedNoteEnds() const override { return false; }

    // 产品动作
    bool auditionNote(int, int) override { return false; }
    void stopAudition() override {}
    void pianoKeyDown(int) override {}
    bool pianoKeyUp(int) override { return false; }
    void exportSelectedNotes() override {}
    void laneLayoutChanged() override {}
    void reportMessage(const juce::String&) override {}
    void invokeCommand(const std::string& cmd) override {
        if (cmd == "transport.playPause") transport.togglePlay();
        else if (cmd == "transport.stop") transport.stop();
    }
    void addEditMenuItems(juce::PopupMenu&) override {}
    void viewWillResize() override {}
    void placeProductControls(const ViewLayout&) override {}

private:
    Document& doc;
    Transport& transport;
};

// 细看窗用 pianoroll-core 的 PianoRollView，它有自己的一套调色板：从 Theme.h 拼一份给它，换皮肤时跟着变
static ViewPalette detailPalette() {
    using namespace theme;
    ViewPalette p;
    p.background = background;
    p.panel = panel;
    p.line = line;
    p.ink = ink;
    p.muted = muted;
    p.accent = accent;
    p.velocity = velocityBar;
    p.scaleRow = pianoScaleRow;
    p.blackRow = pianoBlackRow;
    p.whiteRow = pianoWhiteRow;
    p.beatLine = pianoBeatLine;
    p.subdivisionLine = pianoSubdivisionLine;
    p.blackKey = pianoBlackKey;
    p.whiteKey = pianoWhiteKey;
    p.font = [](float size) { return font(size); };
    return p;
}

LaneDetailWindow::LaneDetailWindow(Document& d, Transport& t, const juce::String& laneId, bool showKs)
    : juce::DocumentWindow("", theme::panel, juce::DocumentWindow::allButtons), doc(d), transport(t), id(laneId) {
    host = std::make_unique<Host>(doc, transport);
    host->showKs = showKs;
    view = std::make_unique<PianoRollView>(*host);
    view->setPalette(detailPalette());
    host->view = view.get();
    view->setLaneLayout(true, false, .82f, .18f);
    view->setFollowEnabled(true);
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setContentNonOwned(view.get(), false);
    centreWithSize(1000, 560);
    modelChanged();
    view->fit();
    setVisible(true);
    startTimerHz(30);
}

LaneDetailWindow::~LaneDetailWindow() {
    stopTimer();
    clearContentComponent();
}

void LaneDetailWindow::closeButtonPressed() {
    if (onClose) onClose(this);
}

void LaneDetailWindow::modelChanged() {
    auto& m = doc.laneModel();
    int lane = -1;
    for (int i = 0; i < m.laneCount(); ++i)
        if (m.laneId(i) == id) lane = i;
    if (lane < 0) {
        juce::MessageManager::callAsync([w = juce::Component::SafePointer<LaneDetailWindow>(this)] {
            if (w && w->onClose) w->onClose(w.getComponent());
        });
        return;
    }
    host->load(lane);
    view->setMeter(m.meterChanges(), m.unitsPerQuarter());
    setName(m.laneInfo(lane).name + juce::String::fromUTF8(" — 细看（只读）"));
    view->repaint();
}

void LaneDetailWindow::timerCallback() {
    view->setTransportState(transport.isPlaying(), true, {}, {});
    if (transport.isPlaying()) view->followPlayback(transport.positionTick());
    view->repaint();
}

} // namespace reader
