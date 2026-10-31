// 画布的只读数据接口——《多轨只读画布 接口设计》“头文件草稿”的实现版。
// 与草稿相比只多了 laneId()（sidecar 里的声部 id，左栏和画布对齐用）。
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <PianoRollCore/Meter.h>

#include <cstdint>
#include <vector>

namespace reader {

enum NoteFlag : std::uint8_t {
    noteFlagNone      = 0,
    noteFlagSoftRange = 1 << 0, // 超出舒适音域
    noteFlagHardRange = 1 << 1, // 超出演奏极限
    noteFlagReview    = 1 << 2, // 清洗规则存疑，待确认
    noteFlagKeySwitch = 1 << 3, // 判为 keyswitch，默认不画
};

struct LaneNote {
    double start = 0, length = 0;  // 原始 tick
    int pitch = 60, velocity = 100;
    std::uint32_t sourceNote = 0;  // 原 MIDI 音符编号，对回 sidecar
    std::uint8_t flags = noteFlagNone;
};

enum class LaneRole { unknown, melody, harmony, bass, rhythm };

struct LaneInfo {
    juce::String name;        // 逻辑声部名，如 “小提琴 I”
    juce::String group;       // 乐器组，如 “弦乐”；空 = 不分组
    juce::Colour colour;
    LaneRole role = LaneRole::unknown;
    int sourceTrackCount = 1; // 合并了几条原始轨
    int comfortLow = 0, comfortHigh = 127; // 舒适音域；未指定乐器时 0..127
    int limitLow = 0, limitHigh = 127;     // 演奏极限
};

// 只读数据源，由逻辑声部投影层实现。只在消息线程读。
class LaneModel {
public:
    virtual ~LaneModel() = default;
    virtual int unitsPerQuarter() const = 0;   // 原始 PPQ
    virtual double documentLength() const = 0; // tick
    virtual std::vector<pianoroll::MeterChange> meterChanges() const = 0;
    virtual int laneCount() const = 0;
    virtual LaneInfo laneInfo(int lane) const = 0;
    virtual juce::String laneId(int lane) const = 0;
    // 按 start 升序。引用在下一次 revision 变化前有效。
    virtual const std::vector<LaneNote>& laneNotes(int lane) const = 0;
    virtual double longestNote(int lane) const = 0; // 供可见范围查找往前推
    virtual std::uint64_t revision() const = 0;   // 每次投影重算后自增
};

} // namespace reader
