#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace reader::theme {
// 与 pianoroll-core 的 ViewPalette 同一套深色基调
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

inline juce::Font font(float size, bool bold = false) {
    return juce::Font(size, bold ? juce::Font::bold : juce::Font::plain);
}
} // namespace reader::theme
