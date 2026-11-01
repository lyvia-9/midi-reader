// 阅读器的全部颜色。换皮肤只改这里：JUCE 控件的底色由 Main.cpp 用这些值拼成 ColourScheme，
// 细看窗（pianoroll-core 的 PianoRollView）的调色板由 LaneDetailView.cpp 的 detailPalette() 从这里取。
// 例外：各声部的颜色来自核心乐器表（和 Python 原型逐字节比对），做浅色主题时要在界面这层另做映射。
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace reader::theme {
// 底色、线、文字：与 pianoroll-core 的 ViewPalette 默认值同一套深色基调
inline const juce::Colour background{0xff1b1d1f};
inline const juce::Colour panel{0xff222527};
inline const juce::Colour panelAlt{0xff26292b};
inline const juce::Colour line{0xff393d3f};
inline const juce::Colour barLine{0xff3a3f42};
inline const juce::Colour beatLine{0xff2a2e30};
inline const juce::Colour laneBg{0xff1f2224};
inline const juce::Colour laneBgAlt{0xff1c1f21};
inline const juce::Colour laneHighlight{0xff2f3538};
inline const juce::Colour ink{0xffe6e5e0};
inline const juce::Colour muted{0xff979d9a};
inline const juce::Colour accent{0xffdc9a68};
inline const juce::Colour hard{0xffe5534b};      // 超出演奏极限
inline const juce::Colour warn{0xffe8c547};      // 超出舒适音域 / 待确认
inline const juce::Colour keySwitch{0xff6b7075};

// 浮层、滚动条
inline const juce::Colour tooltipBg{0xf0151718};  // 工具提示、悬停音符信息
inline const juce::Colour scrollThumb{0xff4a4f52};

// 规则结论的状态标签（待确认用 warn）
inline const juce::Colour chipAuto{0xff7fb069};       // 已自动
inline const juce::Colour chipConfirmed{0xff6fa8dc};  // 你确认了

// 静音 / 独奏
inline const juce::Colour muteOn{0xff7f97bd};
inline const juce::Colour soloOn{0xff74b87a};
inline constexpr float silentAlpha = .28f;           // 不出声的声部，音符画淡

// 细看窗（完整钢琴窗）独有的颜色
inline const juce::Colour pianoScaleRow{0xff263a39};
inline const juce::Colour pianoBlackRow{0xff202325};
inline const juce::Colour pianoWhiteRow{0xff272a2c};
inline const juce::Colour pianoBeatLine{0xff303436};
inline const juce::Colour pianoSubdivisionLine{0xff292d2f};
inline const juce::Colour pianoBlackKey{0xff191c1e};
inline const juce::Colour pianoWhiteKey{0xff424649};
inline const juce::Colour velocityBar{0xff80a7b1};

inline juce::Font font(float size, bool bold = false) {
    return juce::Font(size, bold ? juce::Font::bold : juce::Font::plain);
}
} // namespace reader::theme
