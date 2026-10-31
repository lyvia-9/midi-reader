// 双击一条声部：用 pianoroll-core 的完整钢琴窗（PianoRollView）只读地细看。
// 只读 PianoRollHost 适配器：note() 从 LaneModel 取，ViewNote.editable = false，编辑类虚函数一律空实现。
#pragma once
#include "app/Document.h"
#include "app/Transport.h"

#include <PianoRollCore/PianoRollView.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace reader {

class LaneDetailWindow : public juce::DocumentWindow, private juce::Timer {
public:
    LaneDetailWindow(Document&, Transport&, const juce::String& laneId, bool showKeySwitches);
    ~LaneDetailWindow() override;
    void closeButtonPressed() override;
    void modelChanged();   // 投影换了：重读这条声部；声部没了就关窗
    const juce::String& laneId() const { return id; }
    std::function<void(LaneDetailWindow*)> onClose;

private:
    class Host;
    void timerCallback() override;
    Document& doc;
    Transport& transport;
    juce::String id;
    std::unique_ptr<Host> host;
    std::unique_ptr<pianoroll::PianoRollView> view;
};

} // namespace reader
