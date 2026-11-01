// MultiLaneCanvas.h — MIDI 阅读器的多轨只读画布
// GPL-3.0（链接 PianoRollCore）
// 对应接口设计文档“头文件草稿”；比草稿多了竖向滚动（几十条声部放不下一屏）和悬停提示。
#pragma once
#include "app/LaneMix.h"
#include "app/LaneModel.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <PianoRollCore/Core.h>
#include <PianoRollCore/Meter.h>
#include <PianoRollCore/Navigation.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace reader {

enum class LanePitchFit { usedPitches, instrumentLimits };

struct LaneLayout {
    float laneHeight = 30, groupHeaderHeight = 20, headerWidth = 150, rulerHeight = 22;
    LanePitchFit pitchFit = LanePitchFit::usedPitches;
    bool showKeySwitches = false;
};

class MultiLaneCanvas : public juce::Component, public juce::TooltipClient,
                        private juce::ScrollBar::Listener, private juce::Timer {
public:
    explicit MultiLaneCanvas(LaneModel&);
    ~MultiLaneCanvas() override;

    void modelChanged();                 // 投影重算后在消息线程调用
    void setLayout(LaneLayout);
    const LaneLayout& getLayout() const { return layout; }
    void setGroupCollapsed(const juce::String& group, bool collapsed);
    bool isGroupCollapsed(const juce::String& group) const;

    // 只用时间部分；音高范围每条自适应
    pianoroll::ViewState viewState() const;
    void setViewState(pianoroll::ViewState);
    void fitAll();

    void setPlayhead(double time, bool playing); // ~60Hz
    void setFollowEnabled(bool);
    bool followEnabled() const { return follow.enabled(); }
    void setHighlightedLane(int lane);           // -1 = 无
    int highlightedLane() const { return highlighted; }
    void revealNote(int lane, double time);
    void setLaneMix(std::vector<std::uint8_t> laneFlags);   // LaneMixFlag，每条声部一个

    std::function<void(double time)> onSeek;
    std::function<void(int lane)> onLaneClicked;
    std::function<void(int lane)> onLaneExpandRequested;
    std::function<void(int lane, const LaneNote&)> onNoteHovered;
    std::function<void(pianoroll::ViewState)> onViewChanged;
    std::function<void(int lane, const LaneNote&, juce::Point<int> screenPos)> onNoteRightClicked;
    std::function<void(int lane, juce::Point<int> screenPos)> onLaneRightClicked;
    std::function<void(int lane)> onMuteClicked;                    // 行头的 M
    std::function<void(int lane, bool exclusive)> onSoloClicked;    // 行头的 S；Ctrl+点击 = 只听这一条

    juce::String getTooltip() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Row {                 // 一行 = 一条声部，或一个组的标题 / 折叠后的组概览
        enum Kind { lane, groupHeader, groupOverview } kind = lane;
        int laneIndex = -1;      // kind == lane 时有效
        int firstLane = -1, lastLane = -1;   // 组的范围
        juce::String group;
        float y = 0, height = 0; // 内容坐标（未减滚动）
        double pitchLow = 0, pitchHigh = 127;
    };
    struct LaneStats { int review = 0, hard = 0, soft = 0, keySwitch = 0, notes = 0; double usedLow = 60, usedHigh = 60; bool any = false; };
    class PlayheadOverlay;

    pianoroll::Viewport viewportFor(const Row&) const;
    juce::Rectangle<float> rowBounds(const Row&) const;  // 屏幕坐标（已减滚动）
    juce::Rectangle<float> contentArea() const;           // 声部区（不含行头、标尺、滚动条）
    void rebuildRows();          // 布局、折叠、revision 变化时
    void computeStats();
    void paintRuler(juce::Graphics&, const std::vector<pianoroll::MeterGridMark>&);
    void paintRow(juce::Graphics&, const Row&, const std::vector<pianoroll::MeterGridMark>&);
    void paintNotes(juce::Graphics&, const Row&, int lane, const pianoroll::Viewport&, juce::Rectangle<float> clip, bool overview);
    void paintDensityBand(juce::Graphics&, const Row&, int lane, const pianoroll::Viewport&, juce::Rectangle<float> clip); // 缩小到 <1px/音符时
    void paintHeader(juce::Graphics&, const Row&, juce::Rectangle<float>);
    void paintHover(juce::Graphics&);
    int rowAt(juce::Point<float>) const;
    juce::Rectangle<float> mixButton(juce::Rectangle<float> header, bool solo) const;
    int mixButtonAt(juce::Point<float>, int& lane) const;   // 0 = 不在按钮上，1 = M，2 = S
    std::uint8_t mixOf(int lane) const { return lane >= 0 && (size_t)lane < mix.size() ? mix[(size_t)lane] : 0; }
    const LaneNote* noteAt(int lane, juce::Point<float>) const;
    juce::Colour noteColour(const LaneNote&, juce::Colour laneColour) const;
    pianoroll::NavigationLimits navLimits() const;
    void applyView(pianoroll::ViewState, bool userAction);
    void setScrollY(float);
    float contentHeight() const;
    void updateScrollBars();
    void positionPlayhead();
    bool densityMode() const;
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void timerCallback() override;

    LaneModel& model;
    LaneLayout layout;
    std::vector<Row> rows;
    std::vector<LaneStats> stats;
    std::map<juce::String, bool> collapsed;
    pianoroll::ViewState view;
    pianoroll::NavigationDrag drag;
    pianoroll::PlayheadFollow follow;
    pianoroll::MeterMap meter;
    std::uint64_t seenRevision = ~std::uint64_t(0);
    int highlighted = -1;
    std::vector<std::uint8_t> mix;
    std::unique_ptr<PlayheadOverlay> playhead;
    juce::ScrollBar vbar{true}, hbar{false};
    float scrollY = 0;
    double playTime = 0;
    bool playing = false;

    // 拖动：中键或左键拖 = 平移；左键不拖 = 单击
    bool dragging = false, leftPending = false;
    float dragStartScroll = 0;
    juce::Point<float> pressPos;

    // 悬停
    int hoverLane = -1;
    LaneNote hoverNote;
    bool hasHover = false;
    juce::Point<float> hoverPos;

    // revealNote 的闪烁
    int flashLane = -1;
    double flashTime = 0;
    int flashTicks = 0;
};

} // namespace reader
