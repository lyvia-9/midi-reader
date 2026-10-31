// MIDI Reader — 看得懂乐队的 MIDI 阅读器。GPL-3.0（链接 PianoRollCore）。
#include "app/MainComponent.h"
#include "app/Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace reader {

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel() : juce::LookAndFeel_V4(juce::LookAndFeel_V4::getDarkColourScheme()) {
        // JUCE 7 不会自动给缺字的字形找后备字体：中文界面必须显式选一个带中文的字体。
        // 名字写错时 JUCE 什么都不画，所以从系统实际有的字体里挑第一个候选。
        const auto installed = juce::Font::findAllTypefaceNames();
        if (juce::SystemStats::getEnvironmentVariable("MIDI_READER_LIST_FONTS", {}).isNotEmpty())
            for (auto& n : installed) std::fprintf(stderr, "font: %s\n", n.toRawUTF8());
        for (auto* name : {"Microsoft YaHei UI", "Microsoft YaHei", "DengXian", "SimHei", "PingFang SC",
                           "Hiragino Sans GB", "Noto Sans CJK SC", "Noto Sans SC", "Source Han Sans SC",
                           "WenQuanYi Micro Hei", "Droid Sans Fallback"}) {
            if (installed.contains(name)) {
                setDefaultSansSerifTypefaceName(name);
                chosenFont = name;
                break;
            }
        }
        using namespace theme;
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::TextButton::buttonColourId, panelAlt);
        setColour(juce::TextButton::textColourOffId, ink);
        setColour(juce::ComboBox::backgroundColourId, panelAlt);
        setColour(juce::ComboBox::outlineColourId, line);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, laneHighlight);
        setColour(juce::ListBox::backgroundColourId, panel);
        setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xf0151718));
        setColour(juce::TooltipWindow::textColourId, ink);
        setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff4a4f52));
        setColour(juce::AlertWindow::backgroundColourId, panel);
    }
    juce::String chosenFont;
};

class MainWindow : public juce::DocumentWindow {
public:
    explicit MainWindow(const juce::String& name)
        : juce::DocumentWindow(name, theme::background, juce::DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        auto* content = new MainComponent();
        content->onTitleChanged = [this, name](const juce::String& file) { setName(file + "  -  " + name); };
        setContentOwned(content, true);
        setResizable(true, true);
        setResizeLimits(900, 500, 10000, 10000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    MainComponent* main() { return dynamic_cast<MainComponent*>(getContentComponent()); }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class Application : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "MIDI Reader"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        laf = std::make_unique<LookAndFeel>();   // 必须在 JUCE 初始化之后建：之前列不出系统字体
        juce::LookAndFeel::setDefaultLookAndFeel(laf.get());
        window = std::make_unique<MainWindow>(getApplicationName());
        openFromCommandLine(commandLine);
    }
    void shutdown() override {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        laf.reset();
    }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String& commandLine) override { openFromCommandLine(commandLine); }

private:
    void openFromCommandLine(const juce::String& commandLine) {
        // 把 MIDI 拖到 exe 上、或“打开方式”关联时，路径在命令行里。路径常带空格和中文：
        // 先把整行当一个路径试，再试按平台规则切开的每个参数。
        juce::StringArray candidates;
        candidates.add(commandLine.trim().unquoted());
        candidates.addArray(getCommandLineParameterArray());
        candidates.addArray(juce::StringArray::fromTokens(commandLine, true));
        const juce::File debugLog(juce::SystemStats::getEnvironmentVariable("MIDI_READER_DEBUG", {}));
        const bool debug = debugLog.getFullPathName().isNotEmpty();
        if (debug) debugLog.appendText("cmdline: [" + commandLine + "]\n");
        for (auto a : candidates) {
            a = a.trim().unquoted();
            if (debug) debugLog.appendText("arg: [" + a + "] exists=" + juce::String((int)juce::File(a).existsAsFile()) + "\n");
            if (a.isEmpty()) continue;
            const juce::File f = juce::File::isAbsolutePath(a) ? juce::File(a) : juce::File::getCurrentWorkingDirectory().getChildFile(a);
            if (f.existsAsFile()) {
                if (auto* m = window->main()) m->openFile(f);
                return;
            }
        }
    }
    std::unique_ptr<LookAndFeel> laf;
    std::unique_ptr<MainWindow> window;
};

} // namespace reader

START_JUCE_APPLICATION(reader::Application)
