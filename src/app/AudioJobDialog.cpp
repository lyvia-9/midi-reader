#include "app/AudioJobDialog.h"
#include "app/Theme.h"

namespace reader {

using namespace theme;
static juce::String U(const char* s) { return juce::String::fromUTF8(s); }

static constexpr int dialogWidth = 620;
static constexpr int labelWidth = 84;

AudioJobDialog::AudioJobDialog(juce::File a, juce::PropertySet* s, std::function<void(AudioJobRequest)> cb)
    : audio(std::move(a)), settings(s), onStart(std::move(cb)) {
    auto setupLabel = [this](juce::Label& l, const juce::String& text, juce::Colour c, float size, bool bold = false) {
        addAndMakeVisible(l);
        l.setText(text, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, c);
        l.setFont(font(size, bold));
        l.setJustificationType(juce::Justification::centredLeft);
    };
    setupLabel(heading, U("把音频扒成 MIDI"), ink, 18, true);
    setupLabel(fileLabel, audio.getFullPathName(), muted, 12);
    setupLabel(whatLabel, U("做什么"), ink, 14);
    setupLabel(presetLabel, U("编制预设"), ink, 14);
    setupLabel(methodLabel, U("扒谱方式"), ink, 14);
    setupLabel(styleLabel, U("翻唱风格"), ink, 14);
    setupLabel(titleLabel, U("标题"), ink, 14);
    setupLabel(presetInfo, {}, muted, 12);
    presetInfo.setJustificationType(juce::Justification::topLeft);
    setupLabel(status, U("正在读取预设和模型状态…"), muted, 13);
    status.setJustificationType(juce::Justification::topLeft);
    setupLabel(footer, {}, muted, 11);
    footer.setJustificationType(juce::Justification::topLeft);

    what.addItem(U("扒成 MIDI"), 1);
    what.addItem(U("先用 Suno 翻唱成别的风格，再扒成 MIDI"), 2);
    what.setSelectedId(settings && settings->getIntValue("audioWhat", 1) == 2 ? 2 : 1, juce::dontSendNotification);
    for (auto* c : {&what, &preset, &method}) {
        addAndMakeVisible(*c);
        c->onChange = [this] { refresh(); };
    }
    for (auto* e : {&style, &title}) {
        addAndMakeVisible(*e);
        e->setColour(juce::TextEditor::backgroundColourId, panelAlt);
        e->setColour(juce::TextEditor::textColourId, ink);
        e->setColour(juce::TextEditor::outlineColourId, line);
        e->setFont(font(14));
        e->onTextChange = [this] { refresh(); };
    }
    style.setTextToShowWhenEmpty(U("比如 lofi jazz, rhodes, soft drums"), muted);
    title.setTextToShowWhenEmpty(U("可以不填"), muted);
    if (settings) style.setText(settings->getValue("audioCoverStyle"), false);
    addAndMakeVisible(instrumental);
    instrumental.setButtonText(U("纯音乐（不要人声）"));
    instrumental.setColour(juce::ToggleButton::textColourId, ink);

    startButton.setButtonText(U("开始"));
    cancelButton.setButtonText(U("取消"));
    configButton.setButtonText(U("打开配置文件"));
    configButton.setTooltip(U("HF token、Suno API key 都填在这里（providers.json）。改完点“重新检查”"));
    pipelineButton.setButtonText(U("更改管线位置…"));
    pipelineButton.setTooltip(U("选仓库或 proto 文件夹：阅读器从那里启动 Python 管线"));
    recheckButton.setButtonText(U("重新检查"));
    for (auto* b : {&startButton, &cancelButton, &configButton, &pipelineButton, &recheckButton}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, panelAlt);
    }
    startButton.setColour(juce::TextButton::buttonColourId, accent.darker(.35f));
    startButton.onClick = [this] { start(); };
    cancelButton.onClick = [this] { close(); };
    configButton.onClick = [this] { openConfig(); };
    pipelineButton.onClick = [this] { choosePipelineDir(); };
    recheckButton.onClick = [this] { loadCatalog(); };

    setSize(dialogWidth, 460);
    loadCatalog();
}

AudioJobDialog::~AudioJobDialog() { catalogJob.reset(); }

void AudioJobDialog::show(const juce::File& audio, juce::PropertySet* settings, std::function<void(AudioJobRequest)> onStart) {
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = U("扒谱：") + audio.getFileName();
    o.dialogBackgroundColour = panel;
    o.content.setOwned(new AudioJobDialog(audio, settings, std::move(onStart)));
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void AudioJobDialog::close() {
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

// ---------------------------------------------------------------------------

void AudioJobDialog::loadCatalog() {
    loaded = false;
    catalogJob.reset();
    loc = PipelineLocation::find(settings);
    if (!loc.ok()) {
        refresh();
        return;
    }
    status.setText(U("正在读取预设和模型状态…"), juce::dontSendNotification);
    catalogJob = std::make_unique<PipelineJob>(loc, "catalog", juce::var(), PipelineJob::Callbacks{
        [this](const juce::var& msg) {
            if (msg.hasProperty("result")) catalogArrived(msg["result"]);
            else {
                loc.problem = U("读不出预设：") + msg["error"]["message"].toString();
                refresh();
            }
        },
        {}});
    if (!catalogJob->start()) {
        loc.problem = catalogJob->startError();
        refresh();
    }
}

void AudioJobDialog::catalogArrived(const juce::var& r) {
    presets.clear();
    problems.clear();
    if (auto* arr = r["presets"].getArray())
        for (auto& p : *arr) {
            Preset x;
            x.id = p["id"].toString();
            x.name = p["name"].toString();
            x.route = p["route"].toString();
            x.description = p["description"].toString();
            x.transcriber = p["transcriber"].toString();
            x.separator = p["separator"].toString();
            if (auto* l = p["lanes"].getArray()) for (auto& v : *l) x.lanes.add(v.toString());
            if (auto* l = p["stemTranscribers"].getArray()) {
                x.stemsOk = true;
                for (auto& v : *l) x.stemTranscribers.add(v.toString());
            }
            presets.push_back(x);
        }
    if (auto* arr = r["providers"].getArray())
        for (auto& p : *arr) {
            juce::StringArray probs;
            if (auto* l = p["problems"].getArray()) for (auto& v : *l) probs.add(v.toString());
            problems[p["id"].toString()] = probs;
        }
    configFile = juce::File(r["config"].toString());

    const auto remembered = settings ? settings->getValue("audioPreset", "pop-band") : juce::String("pop-band");
    preset.clear(juce::dontSendNotification);
    int pick = 1;
    for (size_t i = 0; i < presets.size(); ++i) {
        preset.addItem(presets[i].name + "  (" + presets[i].id + ")", (int)i + 1);
        if (presets[i].id == remembered) pick = (int)i + 1;
    }
    preset.setSelectedId(pick, juce::dontSendNotification);
    loaded = true;
    refresh();
}

juce::String AudioJobDialog::providerName(const juce::String& id) {
    if (id == "muscriptor") return "MuScriptor";
    if (id == "demucs") return "Demucs";
    if (id == "basic-pitch") return "Basic Pitch";
    if (id == "sunoapi") return "Suno";
    return id;
}

juce::String AudioJobDialog::providerProblem(const juce::String& id) const {
    auto it = problems.find(id);
    if (it == problems.end()) return U("管线里没有这个模型");
    return it->second.joinIntoString(U("；"));
}

std::vector<AudioJobDialog::Method> AudioJobDialog::methodsFor(const Preset& p) const {
    auto names = [](const juce::StringArray& ids) {
        juce::StringArray out;
        for (auto& i : ids) out.add(providerName(i));
        return out.joinIntoString(U("、"));
    };
    juce::StringArray stemsNeeds{p.separator};
    stemsNeeds.addArray(p.stemTranscribers);
    stemsNeeds.removeDuplicates(false);
    const auto stemsLabel = U("分轨：") + providerName(p.separator) + U(" 分离，再用 ") + names(p.stemTranscribers) + U(" 逐轨扒");
    const auto fullLabel = [](const juce::String& t) { return U("全曲：") + providerName(t) + U(" 一次扒完"); };

    std::vector<Method> out;
    if (p.route == "stems") out.push_back({U("按预设 — ") + stemsLabel, {}, {}, stemsNeeds});
    else out.push_back({U("按预设 — ") + fullLabel(p.transcriber), {}, {}, {p.transcriber}});
    if (!(p.route == "full" && p.transcriber == "muscriptor"))
        out.push_back({fullLabel("muscriptor"), "full", "muscriptor", {"muscriptor"}});
    if (p.stemsOk && p.route != "stems") out.push_back({stemsLabel, "stems", {}, stemsNeeds});
    out.push_back({U("Suno：上传后分离并出 MIDI（每次调用都扣点数）"), "full", "sunoapi", {"sunoapi"}});
    return out;
}

void AudioJobDialog::refresh() {
    const bool cover = what.getSelectedId() == 2;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&styleLabel, &style, &titleLabel, &title, &instrumental})
        c->setVisible(cover);
    heading.setText(cover ? U("翻唱，再扒成 MIDI") : U("把音频扒成 MIDI"), juce::dontSendNotification);

    footer.setText(U("管线：") + (loc.protoDir.exists() ? loc.protoDir.getFullPathName() : U("（还没找到）"))
                       + (configFile != juce::File() ? U("\n配置：") + configFile.getFullPathName() : juce::String()),
                   juce::dontSendNotification);
    configButton.setEnabled(configFile != juce::File());

    juce::String problemText;
    juce::Colour statusColour = muted;
    const int pi = preset.getSelectedId() - 1;
    if (!loc.ok()) {
        problemText = loc.problem;
        statusColour = warn;
    } else if (!loaded) {
        problemText = U("正在读取预设和模型状态…");
    } else if (pi >= 0 && pi < (int)presets.size()) {
        const auto& p = presets[(size_t)pi];
        presetInfo.setText(p.description + U("\n声部：") + p.lanes.joinIntoString(" / "), juce::dontSendNotification);

        // 换预设时尽量保住同一种方式（按 route + transcriber 认）
        juce::String keep = settings ? settings->getValue("audioMethod") : juce::String();
        if (method.getNumItems() > 0 && method.getSelectedId() > 0 && (size_t)method.getSelectedId() <= methods.size()) {
            const auto& m = methods[(size_t)method.getSelectedId() - 1];
            keep = m.route + "|" + m.transcriber;
        }
        methods = methodsFor(p);
        method.clear(juce::dontSendNotification);
        int pick = 1;
        for (size_t i = 0; i < methods.size(); ++i) {
            method.addItem(methods[i].label, (int)i + 1);
            if (methods[i].route + "|" + methods[i].transcriber == keep) pick = (int)i + 1;
        }
        method.setSelectedId(pick, juce::dontSendNotification);

        juce::StringArray needs = methods[(size_t)pick - 1].needs;
        if (cover) needs.insert(0, "sunoapi");
        needs.removeDuplicates(false);
        juce::StringArray bad, good;
        for (auto& n : needs) {
            const auto prob = providerProblem(n);
            if (prob.isEmpty()) good.add(providerName(n));
            else bad.add(providerName(n) + U("：") + prob);
        }
        if (!bad.isEmpty()) {
            problemText = U("还不能开始：\n") + bad.joinIntoString("\n");
            statusColour = warn;
        } else {
            problemText = U("可以开始。要用到：") + good.joinIntoString(U("、"))
                          + U("。\n第一次跑会下载模型环境和权重（几个 GB），会比较久；同一首再跑会直接用缓存。");
            if (needs.contains("sunoapi")) problemText << U("\nSuno 每次调用都扣点数，跑过的结果会缓存，不会重复扣。");
            statusColour = ink;
        }
        if (cover && style.getText().trim().isEmpty()) {
            problemText = U("填一下翻唱风格。\n") + problemText;
            statusColour = warn;
        }
    }
    status.setText(problemText, juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, statusColour);
    startButton.setEnabled(loc.ok() && loaded && statusColour != warn);
    for (auto* c : {&preset, &method, &what}) c->setEnabled(loaded);
    pipelineButton.setButtonText(loc.ok() ? U("更改管线位置…") : U("选择仓库或 proto 文件夹…"));

    setSize(dialogWidth, cover ? 560 : 460);
    resized();
}

void AudioJobDialog::start() {
    const int pi = preset.getSelectedId() - 1;
    const int mi = method.getSelectedId() - 1;
    if (pi < 0 || mi < 0 || (size_t)pi >= presets.size() || (size_t)mi >= methods.size()) return;
    const auto& p = presets[(size_t)pi];
    const auto& m = methods[(size_t)mi];
    const bool cover = what.getSelectedId() == 2;

    auto* t = new juce::DynamicObject();   // 扒谱参数（直接扒，或翻唱后接着扒）
    t->setProperty("preset", p.id);
    if (m.route.isNotEmpty()) t->setProperty("route", m.route);
    if (m.transcriber.isNotEmpty()) t->setProperty("transcriber", m.transcriber);

    AudioJobRequest r;
    if (!cover) {
        t->setProperty("audio", audio.getFullPathName());
        r.method = "transcribe";
        r.params = juce::var(t);
        r.title = U("扒谱：") + audio.getFileName() + U("（") + p.name + U("）");
    } else {
        auto* opts = new juce::DynamicObject();
        opts->setProperty("style", style.getText().trim());
        if (title.getText().trim().isNotEmpty()) opts->setProperty("title", title.getText().trim());
        if (instrumental.getToggleState()) opts->setProperty("instrumental", true);
        auto* c = new juce::DynamicObject();
        c->setProperty("audio", audio.getFullPathName());
        c->setProperty("provider", "sunoapi");
        c->setProperty("options", juce::var(opts));
        c->setProperty("thenTranscribe", juce::var(t));
        r.method = "cover";
        r.params = juce::var(c);
        r.title = U("翻唱并扒谱：") + audio.getFileName() + U("（") + p.name + U("）");
    }
    if (settings) {
        settings->setValue("audioPreset", p.id);
        settings->setValue("audioMethod", m.route + "|" + m.transcriber);
        settings->setValue("audioWhat", cover ? 2 : 1);
        if (cover) settings->setValue("audioCoverStyle", style.getText().trim());
    }
    auto cb = onStart;
    close();   // 关窗会删掉自己，先把回调拷出来
    if (cb) cb(r);
}

void AudioJobDialog::choosePipelineDir() {
    auto chooser = std::make_shared<juce::FileChooser>(U("选仓库或 proto 文件夹"), loc.protoDir.exists() ? loc.protoDir : juce::File());
    juce::Component::SafePointer<AudioJobDialog> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [safe, chooser](const juce::FileChooser& c) {
                             if (safe == nullptr) return;
                             const auto dir = c.getResult();
                             if (!dir.isDirectory()) return;
                             if (safe->settings) safe->settings->setValue("pipelineDir", dir.getFullPathName());
                             safe->loadCatalog();
                         });
}

void AudioJobDialog::openConfig() {
    if (configFile == juce::File()) return;
    if (!configFile.existsAsFile()) {
        configFile.getParentDirectory().createDirectory();
        configFile.replaceWithText(
            "{\n"
            "  \"providers\": {\n"
            "    \"muscriptor\": { \"hfToken\": \"\" },\n"
            "    \"sunoapi\": { \"apiKey\": \"\" }\n"
            "  }\n"
            "}\n", false, false, "\n");
    }
    if (!configFile.startAsProcess()) configFile.revealToUser();
}

// ---------------------------------------------------------------------------

void AudioJobDialog::paint(juce::Graphics& g) {
    g.fillAll(panel);
    g.setColour(line);
    g.drawHorizontalLine(status.getY() - 8, 20.f, (float)getWidth() - 20.f);
    g.drawHorizontalLine(footer.getY() - 8, 20.f, (float)getWidth() - 20.f);
}

void AudioJobDialog::resized() {
    auto r = getLocalBounds().reduced(20, 16);
    heading.setBounds(r.removeFromTop(28));
    fileLabel.setBounds(r.removeFromTop(18));
    r.removeFromTop(12);
    auto row = [&](juce::Label& l, juce::Component& c, int h = 28) {
        auto x = r.removeFromTop(h);
        l.setBounds(x.removeFromLeft(labelWidth));
        c.setBounds(x);
        r.removeFromTop(8);
    };
    row(whatLabel, what);
    row(presetLabel, preset);
    presetInfo.setBounds(r.removeFromTop(36).withTrimmedLeft(labelWidth));
    r.removeFromTop(6);
    row(methodLabel, method);
    if (style.isVisible()) {
        row(styleLabel, style);
        row(titleLabel, title);
        instrumental.setBounds(r.removeFromTop(24).withTrimmedLeft(labelWidth - 4));
        r.removeFromTop(8);
    }
    r.removeFromTop(10);
    auto buttons = r.removeFromBottom(30);
    cancelButton.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(8);
    startButton.setBounds(buttons.removeFromRight(100));
    r.removeFromBottom(14);
    auto foot = r.removeFromBottom(54);
    auto footButtons = foot.removeFromBottom(24);
    configButton.setBounds(footButtons.removeFromLeft(110));
    footButtons.removeFromLeft(8);
    pipelineButton.setBounds(footButtons.removeFromLeft(170));
    footer.setBounds(foot);
    r.removeFromBottom(16);
    auto st = r;
    recheckButton.setBounds(st.removeFromRight(80).removeFromTop(24));
    status.setBounds(st.withTrimmedRight(8));
}

} // namespace reader
