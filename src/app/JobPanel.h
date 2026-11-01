// 主窗口底部的任务条：扒谱 / 翻唱在后台跑时显示当前步骤、最新一行输出、进度和用时，可以取消。
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace reader {

class JobPanel : public juce::Component, private juce::Timer {
public:
    JobPanel();
    static constexpr int preferredHeight = 62;

    void begin(const juce::String& title);
    void setStep(const juce::String& step);
    void setProgress(double fraction, const juce::String& message);   // fraction < 0 = 不知道进度
    void setCancelling();
    void end();
    bool active() const { return running; }

    std::function<void()> onCancel, onShowLog;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    juce::String title, step, message;
    double fraction = -1;
    juce::int64 startedMs = 0;
    bool running = false, cancelling = false;
    float phase = 0;
    juce::TextButton cancelButton, logButton;
};

// 从一行进度输出里认出百分比（tqdm 的 " 45%|████" 这种），认不出返回 -1
double progressFromText(const juce::String&);

} // namespace reader
