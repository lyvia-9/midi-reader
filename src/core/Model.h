// 核心数据类型（不依赖 JUCE）。与 proto/reader_proto/model.py 一一对应；
// 画布侧的 reader::LaneNote / LaneInfo（接口设计文档的头文件草稿）由 app 层从这里转换。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace reader::core {

enum NoteFlag : std::uint8_t {
    noteFlagNone = 0,
    noteFlagSoftRange = 1 << 0,
    noteFlagHardRange = 1 << 1,
    noteFlagReview = 1 << 2,
    noteFlagKeySwitch = 1 << 3,
};

struct LaneNote {
    std::int64_t start = 0, length = 0;   // 原始 tick
    int pitch = 60, velocity = 100;
    std::uint32_t sourceNote = 0;
    std::uint8_t flags = noteFlagNone;
    std::int64_t end() const { return start + length; }
};

inline bool noteLess(const LaneNote& a, const LaneNote& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.pitch != b.pitch) return a.pitch < b.pitch;
    return a.sourceNote < b.sourceNote;
}

struct LaneInfo {
    std::string name, group;
    std::string colour = "#808080";   // "#rrggbb"
    std::string role = "unknown";
    int sourceTrackCount = 1;
    int comfortLow = 0, comfortHigh = 127, limitLow = 0, limitHigh = 127;
};

struct MeterChange { std::int64_t tick = 0; int numerator = 4, denominator = 4; };
struct TempoChange { std::int64_t tick = 0; int microsPerQuarter = 500000; };

} // namespace reader::core
