#include "app/JobPanel.h"
#include "app/Theme.h"

namespace reader {

using namespace theme;
static juce::String U(const char* s) { return juce::String::fromUTF8(s); }

double progressFromText(const juce::String& s) {
    const int bar = s.indexOf("%|");
    if (bar <= 0) return -1;
    int i = bar;
    while (i > 0 && juce::CharacterFunctions::isDigit(s[i - 1])) --i;
    if (i == bar) return -1;
    const int pct = s.substring(i, bar).getIntValue();
    return pct >= 0 && pct <= 100 ? pct / 100.0 : -1;
}

JobPanel::JobPanel() {
    cancelButton.setButtonText(U("取消"));
    logButton.setButtonText(U("看日志"));
    logButton.setTooltip(U("管线的完整输出（出错时把它发出来最有用）"));
    for (auto* b : {&cancelButton, &logButton}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, panelAlt);
        b->setWantsKeyboardFocus(false);
    }
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    logButton.onClick = [this] { if (onShowLog) onShowLog(); };
}

void JobPanel::begin(const juce::String& t) {
    title = t;
    step = U("正在启动管线…");
    message = {};
    fraction = -1;
    running = true;
    cancelling = false;
    cancelButton.setEnabled(true);
    startedMs = juce::Time::currentTimeMillis();
    startTimerHz(20);
    repaint();
}

void JobPanel::setStep(const juce::String& s) {
    step = s;
    message = {};
    fraction = -1;
    repaint();
}

void JobPanel::setProgress(double f, const juce::String& m) {
    if (m.isNotEmpty()) message = m;
    fraction = f;
    repaint();
}

void JobPanel::setCancelling() {
    cancelling = true;
    cancelButton.setEnabled(false);
    step = U("正在取消（等模型进程停下）…");
    repaint();
}

void JobPanel::end() {
    running = false;
    stopTimer();
    repaint();
}

void JobPanel::timerCallback() {
    phase = std::fmod(phase + 0.02f, 1.f);
    repaint();
}

void JobPanel::resized() {
    auto r = getLocalBounds().reduced(12, 0).withTrimmedTop(3);
    auto buttons = r.removeFromRight(170).withSizeKeepingCentre(170, 28);
    cancelButton.setBounds(buttons.removeFromRight(70));
    buttons.removeFromRight(8);
    logButton.setBounds(buttons.removeFromRight(80));
}

void JobPanel::paint(juce::Graphics& g) {
    g.fillAll(panel);
    // 顶上一条进度：知道百分比就画实条，不知道就画一段来回走的
    auto bar = getLocalBounds().removeFromTop(3).toFloat();
    g.setColour(line);
    g.fillRect(bar);
    g.setColour(cancelling ? muted : accent);
    if (fraction >= 0) g.fillRect(bar.withWidth(bar.getWidth() * (float)fraction));
    else if (running) {
        const float w = bar.getWidth() * .25f;
        const float x = (bar.getWidth() + w) * phase - w;
        g.fillRect(bar.withX(x).withWidth(w).getIntersection(bar));
    }

    auto r = getLocalBounds().reduced(14, 0).withTrimmedTop(3).withTrimmedRight(180);
    const auto secs = (int)((juce::Time::currentTimeMillis() - startedMs) / 1000);
    const auto elapsed = juce::String(secs / 60) + ":" + juce::String(secs % 60).paddedLeft('0', 2);
    auto top = r.removeFromTop(30);
    g.setColour(muted);
    g.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 13.f, juce::Font::plain));
    g.drawText(elapsed, top.removeFromRight(60), juce::Justification::centredRight);
    g.setFont(font(14, true));
    g.setColour(ink);
    const auto tw = juce::jmin(top.getWidth() / 2, g.getCurrentFont().getStringWidth(title) + 16);
    g.drawText(title, top.removeFromLeft(tw), juce::Justification::centredLeft, true);
    g.setFont(font(13));
    g.setColour(accent);
    g.drawText(step, top, juce::Justification::centredLeft, true);
    g.setColour(muted);
    g.setFont(font(12));
    auto line2 = message;
    if (fraction >= 0) line2 = juce::String(juce::roundToInt(fraction * 100)) + "%   " + line2;
    g.drawText(line2, r.removeFromTop(20), juce::Justification::centredLeft, true);
}

} // namespace reader
