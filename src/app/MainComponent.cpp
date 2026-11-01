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
    openButton.setTooltip(U("打开 MIDI 或音频（Ctrl+O）。也可以把文件直接拖进窗口；音频会先扒成 MIDI"));
    audioButton.setButtonText(U("扒谱…"));
    audioButton.setTooltip(U("选一个音频（wav / mp3 / flac…），按编制预设扒成 MIDI，或先用 Suno 翻唱再扒"));
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
    for (auto* b : {&openButton, &audioButton, &playButton, &stopButton, &fitButton, &reanalyzeButton}) {
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
    audioButton.onClick = [this] { chooseAudio(); };
    addChildComponent(jobPanel);
    jobPanel.onCancel = [this] {
        if (job == nullptr) return;
        job->cancel();
        jobPanel.setCancelling();
    };
    jobPanel.onShowLog = [this] {
        if (job == nullptr) return;
        const auto log = job->logFile();
        if (!log.existsAsFile() || !log.startAsProcess()) log.getParentDirectory().revealToUser();
    };
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
    job.reset();   // 还在跑就取消，管线会停掉它起的模型进程
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
    g.drawText(dragAudio ? U("松手把这个音频扒成 MIDI") : U("松手打开这个 MIDI"), getLocalBounds(), juce::Justification::centred);
}

void MainComponent::resized() {
    auto r = getLocalBounds();
    auto bar = r.removeFromTop(40).reduced(8, 6);
    r.removeFromTop(1);
    openButton.setBounds(bar.removeFromLeft(60));
    bar.removeFromLeft(4);
    audioButton.setBounds(bar.removeFromLeft(64));
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
    jobPanel.setVisible(jobPanel.active());
    if (jobPanel.active()) {
        jobPanel.setBounds(r.removeFromBottom(JobPanel::preferredHeight));
        r.removeFromBottom(1);
    }
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
    if (isAudioFile(f)) {
        openAudio(f);
        return;
    }
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
    chooser = std::make_unique<juce::FileChooser>(U("打开 MIDI 或音频"), start, "*.mid;*.midi;*.rmi;" + audioFileWildcard());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& c) {
                             const auto f = c.getResult();
                             if (f.existsAsFile()) openFile(f);
                         });
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files)
        if (f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi") || f.endsWithIgnoreCase(".rmi")
            || isAudioFile(juce::File(f)))
            return true;
    return false;
}

void MainComponent::fileDragEnter(const juce::StringArray& files, int, int) {
    dragOver = true;
    dragAudio = false;
    for (auto& f : files)
        if (isInterestedInFileDrag({f})) {
            dragAudio = isAudioFile(juce::File(f));
            break;
        }
    repaint();
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

// ---- 音频：扒谱 / 翻唱 ----

void MainComponent::chooseAudio() {
    juce::File start;
    if (auto* p = props.getUserSettings()) start = juce::File(p->getValue("lastAudioDir", p->getValue("lastDir")));
    chooser = std::make_unique<juce::FileChooser>(U("选一个要扒谱的音频"), start, audioFileWildcard());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& c) {
                             const auto f = c.getResult();
                             if (f.existsAsFile()) openAudio(f);
                         });
}

void MainComponent::openAudio(const juce::File& f) {
    if (job != nullptr) {
        showMessage(U("已经有一个扒谱任务在跑：等它完成，或点底下的“取消”"));
        return;
    }
    auto* settings = props.getUserSettings();
    if (settings) settings->setValue("lastAudioDir", f.getParentDirectory().getFullPathName());
    juce::Component::SafePointer<MainComponent> safe(this);
    AudioJobDialog::show(f, settings, [safe](AudioJobRequest r) {
        if (safe != nullptr) safe->startAudioJob(r);
    });
}

void MainComponent::startAudioJob(const AudioJobRequest& r) {
    jobResult = juce::var();
    jobError = juce::var();
    job = std::make_unique<PipelineJob>(PipelineLocation::find(props.getUserSettings()), r.method, r.params,
                                        PipelineJob::Callbacks{[this](const juce::var& m) { audioJobMessage(m); },
                                                               [this] { audioJobFinished(); }});
    if (!job->start()) {
        const auto err = job->startError();
        job.reset();
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, U("扒谱没能开始"), err);
        return;
    }
    jobPanel.begin(r.title);
    resized();
}

static juce::String stepText(const juce::var& e) {
    // step 是 "02-transcribe-bass" 这种：去掉序号，认出做什么、哪一轨
    auto label = e["step"].toString().fromFirstOccurrenceOf("-", false, false);
    const auto what = label.upToFirstOccurrenceOf("-", false, false);
    const auto stem = label.fromFirstOccurrenceOf("-", false, false);
    static const std::map<juce::String, const char*> whats{
        {"separate", "分离音轨"}, {"transcribe", "扒谱"}, {"cover", "翻唱"}};
    static const std::map<juce::String, const char*> stems{
        {"vocals", "人声"}, {"drums", "鼓"}, {"bass", "贝斯"}, {"guitar", "吉他"}, {"piano", "键盘"}, {"other", "其他"}};
    static const std::map<juce::String, const char*> providers{
        {"muscriptor", "MuScriptor"}, {"demucs", "Demucs"}, {"basic-pitch", "Basic Pitch"}, {"sunoapi", "Suno"}};
    auto pick = [](const auto& m, const juce::String& k) {
        auto it = m.find(k);
        return it == m.end() ? k : juce::String::fromUTF8(it->second);
    };
    auto out = pick(whats, what);
    if (stem.isNotEmpty()) out << U(" · ") << pick(stems, stem);
    out << U(" · ") << pick(providers, e["provider"].toString());
    return out;
}

void MainComponent::audioJobMessage(const juce::var& m) {
    const auto ev = m["event"].toString();
    if (ev == "step") jobPanel.setStep(stepText(m));
    else if (ev == "progress") {
        const auto text = m["message"].toString();
        double f = m["fraction"].isVoid() || m["fraction"].isUndefined() ? -1.0 : (double)m["fraction"];
        if (f < 0) f = progressFromText(text);
        jobPanel.setProgress(f, text);
    } else if (m.hasProperty("result")) jobResult = m["result"];
    else if (m.hasProperty("error")) jobError = m["error"];
}

void MainComponent::audioJobFinished() {
    jobPanel.end();
    resized();
    const auto result = jobResult, error = jobError;
    const auto log = job ? job->logFile() : juce::File();
    // PipelineJob 正在回调里，下一轮消息循环再删
    juce::Component::SafePointer<MainComponent> safe(this);
    juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->job.reset(); });

    if (result.isObject()) {
        const juce::File midi(result["midi"].toString());
        if (midi.existsAsFile()) {
            openFile(midi);
            showMessage(U("扒好了：") + midi.getParentDirectory().getFileName());
            return;
        }
        // 只翻唱不扒（现在界面上不会出现，留着兜底）：告诉用户新音频在哪
        juce::StringArray audios;
        if (auto* a = result["audio"].getArray()) for (auto& v : *a) audios.add(v.toString());
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, U("翻唱好了"), audios.joinIntoString("\n"));
        return;
    }
    const auto type = error["type"].toString();
    if (type == "cancelled") {
        showMessage(U("已取消"));
        return;
    }
    auto text = error["message"].toString();
    if (type == "unavailable") text << U("\n\n缺的东西在扒谱对话框里会列出来；HF token、Suno API key 填在 providers.json，对话框里有“打开配置文件”。");
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, U("扒谱失败"), text, U("看日志"), U("关闭"), this,
                                       juce::ModalCallbackFunction::create([log](int r) {
                                           if (r == 1 && log.existsAsFile() && !log.startAsProcess())
                                               log.getParentDirectory().revealToUser();
                                       }));
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
