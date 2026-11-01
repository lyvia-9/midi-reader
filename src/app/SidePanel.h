// 左栏：声部列表（整理模式：改乐器、合并、拆出、挪来源、改名、排序）和规则结论队列（确认/否决）。
#pragma once
#include "app/Document.h"
#include "app/LaneMix.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace reader {

class SidePanel : public juce::Component {
public:
    explicit SidePanel(Document&);
    ~SidePanel() override;

    void refresh();                         // 投影换了
    void setHighlightedLane(int lane);      // 画布点了一条
    void setLaneMix(std::vector<std::uint8_t> laneFlags);   // LaneMixFlag，每条声部一个

    std::function<void(int lane)> onLaneSelected;
    std::function<void(int lane, double tick)> onReveal;
    std::function<void(int lane)> onExpand;
    std::function<void(int lane)> onMuteToggled;
    std::function<void(int lane, bool exclusive)> onSoloToggled;   // exclusive = 只听这一条
    std::function<void()> onMixCleared;
    std::function<void(const juce::String& message)> onMessage;

    void showLaneMenu(int lane);            // 画布右键一条声部也用这个菜单

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class LaneList;
    class DecisionList;
    void runAndReport(const juce::String& error);
    void askRename(int lane);
    void askMergeName(const juce::StringArray& laneIds, const juce::String& suggestion);
    juce::String streamLabel(const std::string& streamId) const;
    int laneOfStream(const std::string& streamId) const;
    std::pair<int, double> firstAffected(const core::Decision&) const;
    juce::String decisionTitle(const core::Decision&) const;

    Document& doc;
    std::unique_ptr<LaneList> lanes;
    std::unique_ptr<DecisionList> decisions;
    juce::Label lanesTitle, decisionsTitle, info;
    juce::ToggleButton pendingOnly{juce::String::fromUTF8("只看待确认")};
    std::vector<std::uint8_t> mix;
    std::uint8_t mixOf(int lane) const { return lane >= 0 && (size_t)lane < mix.size() ? mix[(size_t)lane] : 0; }
    friend class LaneList;
    friend class DecisionList;
};

} // namespace reader
