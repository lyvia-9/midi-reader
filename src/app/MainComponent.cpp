#include "app/MainComponent.h"
#include "app/Theme.h"

namespace reader {

using namespace theme;
static juce::String U(const char* s) { return juce::String::fromUTF8(s); }

MainComponent::MainComponent() {
    juce::PropertiesFile::Options o;
    o.applicationName = "MIDI Reader";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    o.folderName = "MIDI Reader";
    props.setStorageParameters(o);

    setOpaque(true);
    setWantsKeyboardFocus(true);
    addAndMakeVisible(canvas);
    addAndMakeVisible(side);
    addAndMakeVisible(resizer);
    layoutManager.setItemLayout(0, 240, 700, 340);
    layoutManager.setItemLayout(1, 5, 5, 5);
    layoutManager.setItemLayout(2, 300, -1.0, -0.75);

    openButton.setButtonText(U("打开"));
    openButton.setTooltip(U("打开 MIDI（Ctrl+O）。也可以把文件直接拖进窗口"));
    playButton.setButtonText(U("播放"));
    playButton.setTooltip(U("空格：播放/暂停"));
    stopButton.setButtonText(U("停止"));
    stopButton.setTooltip(U("停止回定位点，再按一次回开头（Home 直接回开头）"));
    fitButton.setButtonText(U("看全曲"));
    fitButton.setTooltip(U("F：缩放到整首曲子"));
    reanalyzeButton.setButtonText(U("重新整理…"));
    reanalyzeButton.setTooltip(U("按规则重建声部映射（保留你确认/否决过的结论和逐音修正）"));
    followToggle.setButtonText(U("跟随"));
    followToggle.setToggleState(true, juce::dontSendNotification);
    followToggle.setTooltip(U("播放时自动翻页；手动滚动后暂停跟随，点标尺或按播放恢复"));
    ksToggle.setButtonText(U("显示 keyswitch"));
    ksToggle.setTooltip(U("默认不画被判为 keyswitch 的音；打开后用灰色画出来"));
    pitchFit.addItem(U("音高：按实际用到的音"), 1);
    pitchFit.addItem(U("音高：按乐器音域"), 2);
    pitchFit.setSelectedId(1, juce::dontSendNotification);
    pitchFit.setTooltip(U("按乐器音域时，超出音域的音压在每条的上下边缘"));
    for (auto* b : {&openButton, &playButton, &stopButton, &fitButton, &reanalyzeButton}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, panelAlt);
        b->setWantsKeyboardFocus(false);
    }
    for (auto* t : {&followToggle, &ksToggle}) {
        addAndMakeVisible(*t);
        t->setColour(juce::ToggleButton::textColourId, ink);
        t->setWantsKeyboardFocus(false);
    }
    addAndMakeVisible(pitchFit);
    pitchFit.setWantsKeyboardFocus(false);
    addAndMakeVisible(clock);
    clock.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 14.f, juce::Font::plain));
    clock.setColour(juce::Label::textColourId, ink);
    clock.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(message);
    message.setColour(juce::Label::textColourId, accent);
    message.setFont(font(12));
    message.setJustificationType(juce::Justification::centredRight);

    openButton.onClick = [this] { chooseFile(); };
    playButton.onClick = [this] { transport.togglePlay(); canvas.setFollowEnabled(followToggle.getToggleState()); };
    stopButton.onClick = [this] { transport.stop(); };
    fitButton.onClick = [this] { canvas.fitAll(); };
    followToggle.onClick = [this] { canvas.setFollowEnabled(followToggle.getToggleState()); };
    ksToggle.onClick = [this] {
        auto l = canvas.getLayout();
        l.showKeySwitches = ksToggle.getToggleState();
        canvas.setLayout(l);
    };
    pitchFit.onChange = [this] {
        auto l = canvas.getLayout();
        l.pitchFit = pitchFit.getSelectedId() == 2 ? LanePitchFit::instrumentLimits : LanePitchFit::usedPitches;
        canvas.setLayout(l);
    };
    reanalyzeButton.onClick = [this] {
        if (!doc.hasFile()) return;
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, U("重新整理"),
            U("按规则重建声部映射：你手动做的合并、拆分、改名、排序和指定的乐器会丢掉；\n你确认/否决过的规则结论和逐音修正会保留。"),
            U("重新整理"), U("取消"), this, juce::ModalCallbackFunction::create([this](int r) {
                if (r == 1) showMessage(doc.reanalyzeFromScratch());
            }));
    };

    doc.onChanged = [this] { documentChanged(); };
    canvas.onSeek = [this](double t) { transport.seekTick(t); };
    canvas.onLaneClicked = [this](int lane) { side.setHighlightedLane(lane); };
    canvas.onLaneExpandRequested = [this](int lane) { openDetail(lane); };
    canvas.onLaneRightClicked = [this](int lane, juce::Point<int>) { side.setHighlightedLane(lane); side.showLaneMenu(lane); };
    canvas.onNoteRightClicked = [this](int lane, const LaneNote& n, juce::Point<int>) { showNoteMenu(lane, n); };
    side.onLaneSelected = [this](int lane) { canvas.setHighlightedLane(lane); };
    side.onReveal = [this](int lane, double tick) { canvas.revealNote(lane, tick); };
    side.onExpand = [this](int lane) { openDetail(lane); };
    side.onSoloToggled = [this](int lane) {
        transport.setSolo(lane);
        side.setSoloLane(lane);
    };
    side.onMessage = [this](const juce::String& m) { showMessage(m); };

    if (transport.audioStatus().isNotEmpty()) showMessage(transport.audioStatus());
    setSize(1400, 860);
    startTimerHz(60);
}

MainComponent::~MainComponent() {
    stopTimer();
    details.clear();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(background);
    g.setColour(panel);
    g.fillRect(getLocalBounds().removeFromTop(40));
    g.setColour(line);
    g.drawHorizontalLine(40, 0, (float)getWidth());
}

void MainComponent::paintOverChildren(juce::Graphics& g) {
    if (!dragOver) return;
    g.setColour(accent.withAlpha(.15f));
    g.fillAll();
    g.setColour(accent);
    g.drawRect(getLocalBounds(), 3);
    g.setFont(font(20, true));
    g.drawText(U("松手打开这个 MIDI"), getLocalBounds(), juce::Justification::centred);
}

void MainComponent::resized() {
    auto r = getLocalBounds();
    auto bar = r.removeFromTop(40).reduced(8, 6);
    r.removeFromTop(1);
    openButton.setBounds(bar.removeFromLeft(60));
    bar.removeFromLeft(14);
    playButton.setBounds(bar.removeFromLeft(56));
    bar.removeFromLeft(4);
    stopButton.setBounds(bar.removeFromLeft(56));
    bar.removeFromLeft(10);
    clock.setBounds(bar.removeFromLeft(190));
    followToggle.setBounds(bar.removeFromLeft(70));
    ksToggle.setBounds(bar.removeFromLeft(130));
    pitchFit.setBounds(bar.removeFromLeft(190).reduced(0, 1));
    bar.removeFromLeft(8);
    fitButton.setBounds(bar.removeFromLeft(66));
    bar.removeFromLeft(4);
    reanalyzeButton.setBounds(bar.removeFromLeft(90));
    bar.removeFromLeft(10);
    message.setBounds(bar);
    juce::Component* comps[] = {&side, &resizer, &canvas};
    layoutManager.layOutComponents(comps, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);
}

void MainComponent::timerCallback() {
    const double tick = transport.positionTick();
    canvas.setPlayhead(tick, transport.isPlaying());
    if (tick != lastClockTick && doc.hasFile()) {
        lastClockTick = tick;
        pianoroll::MeterMap mm;
        mm.assign(doc.laneModel().meterChanges(), doc.laneModel().unitsPerQuarter());
        const auto pos = mm.positionAt(tick);
        const double sec = transport.positionSeconds();
        clock.setText(juce::String(pos.bar) + "." + juce::String((int)std::floor(pos.beat)) + "   "
                          + juce::String((int)(sec / 60)) + ":" + juce::String(std::fmod(sec, 60.0), 1).paddedLeft('0', 4),
                      juce::dontSendNotification);
    }
    updateTransportButtons();
    if (messageTicks > 0 && --messageTicks == 0) message.setText({}, juce::dontSendNotification);
}

void MainComponent::updateTransportButtons() {
    const auto t = transport.isPlaying() ? U("暂停") : U("播放");
    if (playButton.getButtonText() != t) playButton.setButtonText(t);
}

void MainComponent::showMessage(const juce::String& m) {
    if (m.isEmpty()) return;
    message.setText(m, juce::dontSendNotification);
    messageTicks = 60 * 8;
}

void MainComponent::documentChanged() {
    canvas.modelChanged();
    side.refresh();
    if (doc.hasFile()) transport.setSchedule(doc.projection(), doc.tempoMap());
    else transport.clear();
    for (auto& d : details) d->modelChanged();
    if (doc.sidecarStatus().isNotEmpty()) showMessage(doc.sidecarStatus());
}

void MainComponent::openFile(const juce::File& f) {
    transport.stop();
    transport.home();
    details.clear();
    const auto err = doc.open(f);
    if (err.isNotEmpty()) {
        showMessage(err);
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, U("打不开"), err);
        return;
    }
    canvas.fitAll();
    grabKeyboardFocus();
    transport.setSolo(-1);
    side.setSoloLane(-1);
    if (auto* p = props.getUserSettings()) p->setValue("lastDir", f.getParentDirectory().getFullPathName());
    if (onTitleChanged) onTitleChanged(f.getFileName());
    int pending = 0;
    for (auto& d : doc.sidecar().decisions) pending += d.pending();
    showMessage(pending ? juce::String(pending) + U(" 条规则结论待确认，见左下") : U("已打开"));
}

void MainComponent::chooseFile() {
    juce::File start;
    if (auto* p = props.getUserSettings()) start = juce::File(p->getValue("lastDir"));
    chooser = std::make_unique<juce::FileChooser>(U("打开 MIDI"), start, "*.mid;*.midi;*.rmi");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& c) {
                             const auto f = c.getResult();
                             if (f.existsAsFile()) openFile(f);
                         });
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files)
        if (f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi") || f.endsWithIgnoreCase(".rmi")) return true;
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int) {
    dragOver = false;
    repaint();
    for (auto& f : files)
        if (isInterestedInFileDrag({f})) {
            openFile(juce::File(f));
            return;
        }
}

bool MainComponent::keyPressed(const juce::KeyPress& k) {
    if (k == juce::KeyPress::spaceKey || k == juce::KeyPress::returnKey) {
        transport.togglePlay();
        if (transport.isPlaying()) canvas.setFollowEnabled(followToggle.getToggleState());
        return true;
    }
    if (k == juce::KeyPress::homeKey) { transport.home(); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) { showMessage(doc.undo()); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
        || k == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) { showMessage(doc.redo()); return true; }
    if (k == juce::KeyPress('o', juce::ModifierKeys::commandModifier, 0)) { chooseFile(); return true; }
    if (k.getTextCharacter() == 'f' || k.getTextCharacter() == 'F') { canvas.fitAll(); return true; }
    return false;
}

void MainComponent::openDetail(int lane) {
    if (!doc.hasFile() || lane < 0 || lane >= doc.laneModel().laneCount()) return;
    const auto id = doc.laneModel().laneId(lane);
    for (auto& d : details)
        if (d->laneId() == id) { d->toFront(true); return; }
    auto w = std::make_unique<LaneDetailWindow>(doc, transport, id, ksToggle.getToggleState());
    w->onClose = [this](LaneDetailWindow* win) {
        details.erase(std::remove_if(details.begin(), details.end(), [win](auto& p) { return p.get() == win; }), details.end());
    };
    details.push_back(std::move(w));
}

void MainComponent::showNoteMenu(int lane, const LaneNote& n) {
    juce::PopupMenu m;
    const bool isKs = n.flags & noteFlagKeySwitch;
    const auto& notes = doc.laneModel().laneNotes(lane);
    std::vector<std::uint32_t> samePitch;
    for (auto& o : notes)
        if (o.pitch == n.pitch) samePitch.push_back(o.sourceNote);
    const auto name = juce::String(pianoroll::pitchName(n.pitch, 5));
    m.addSectionHeader(name + U("  原始音符 #") + juce::String((int)n.sourceNote));
    m.addItem(U("这个音是 keyswitch"), !isKs, false, [this, s = n.sourceNote] { showMessage(doc.setNoteKeySwitch({s}, true)); });
    m.addItem(U("这个音不是 keyswitch"), true, false, [this, s = n.sourceNote] { showMessage(doc.setNoteKeySwitch({s}, false)); });
    m.addItem(U("这条声部里所有 ") + name + U(" 都是 keyswitch"), true, false,
              [this, samePitch] { showMessage(doc.setNoteKeySwitch(samePitch, true)); });
    m.addItem(U("这条声部里所有 ") + name + U(" 都不是 keyswitch"), true, false,
              [this, samePitch] { showMessage(doc.setNoteKeySwitch(samePitch, false)); });
    const bool hasOverride = doc.sidecar().noteOverrides.count(n.sourceNote) > 0;
    m.addItem(U("清除我对这个音的设置"), hasOverride, false,
              [this, s = n.sourceNote] { showMessage(doc.setNoteKeySwitch({s}, std::nullopt)); });
    m.addSeparator();
    m.addItem(U("从这里播放"), [this, t = n.start] { transport.seekTick(t); transport.play(); });
    m.addItem(U("声部菜单…"), [this, lane] { side.showLaneMenu(lane); });
    m.showMenuAsync({});
}

} // namespace reader
