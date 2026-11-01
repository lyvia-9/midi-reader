#include "app/MultiLaneCanvas.h"
#include "app/Theme.h"

#include <algorithm>
#include <cmath>

namespace reader {

using namespace theme;

// 叠在画布上的窄光标：只移动它，不触发整屏重绘
class MultiLaneCanvas::PlayheadOverlay : public juce::Component {
public:
    PlayheadOverlay() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override {
        g.setColour(accent);
        g.fillRect(getLocalBounds().withSizeKeepingCentre(1, getHeight()).toFloat().withWidth(1.5f));
        juce::Path tri;
        const float cx = getWidth() * .5f;
        tri.addTriangle(cx - 5, 0, cx + 5, 0, cx, 7);
        g.fillPath(tri);
    }
};

MultiLaneCanvas::MultiLaneCanvas(LaneModel& m) : model(m), playhead(std::make_unique<PlayheadOverlay>()) {
    addAndMakeVisible(*playhead);
    addAndMakeVisible(vbar);
    addAndMakeVisible(hbar);
    vbar.addListener(this);
    hbar.addListener(this);
    vbar.setAutoHide(false);
    hbar.setAutoHide(false);
    setOpaque(true);
    setWantsKeyboardFocus(false);
    view.timeStart = 0;
    view.timeSpan = 480 * 16;
    modelChanged();
}

MultiLaneCanvas::~MultiLaneCanvas() = default;

// ---------------------------------------------------------------------------
// 数据与布局

void MultiLaneCanvas::modelChanged() {
    const bool first = seenRevision == ~std::uint64_t(0) || model.laneCount() == 0;
    seenRevision = model.revision();
    std::vector<pianoroll::MeterChange> changes = model.meterChanges();
    meter.assign(changes, model.unitsPerQuarter());
    computeStats();
    rebuildRows();
    if (first) fitAll();
    else applyView(view, false);
    if (highlighted >= model.laneCount()) highlighted = -1;
    hasHover = false;
    repaint();
}

void MultiLaneCanvas::computeStats() {
    stats.assign((size_t)model.laneCount(), {});
    for (int i = 0; i < model.laneCount(); ++i) {
        auto& s = stats[(size_t)i];
        for (auto& n : model.laneNotes(i)) {
            ++s.notes;
            if (n.flags & noteFlagKeySwitch) { ++s.keySwitch; if (!layout.showKeySwitches) continue; }
            if (n.flags & noteFlagReview) ++s.review;
            if (n.flags & noteFlagHardRange) ++s.hard;
            else if (n.flags & noteFlagSoftRange) ++s.soft;
            if (!s.any) { s.usedLow = s.usedHigh = n.pitch; s.any = true; }
            s.usedLow = std::min<double>(s.usedLow, n.pitch);
            s.usedHigh = std::max<double>(s.usedHigh, n.pitch);
        }
    }
}

void MultiLaneCanvas::setLayout(LaneLayout l) {
    layout = l;
    computeStats();
    rebuildRows();
    resized();
    repaint();
}

void MultiLaneCanvas::setGroupCollapsed(const juce::String& group, bool c) {
    collapsed[group] = c;
    rebuildRows();
    updateScrollBars();
    repaint();
}

bool MultiLaneCanvas::isGroupCollapsed(const juce::String& group) const {
    auto it = collapsed.find(group);
    return it != collapsed.end() && it->second;
}

void MultiLaneCanvas::rebuildRows() {
    rows.clear();
    float y = 0;
    auto pitchRange = [&](int first, int last, double& lo, double& hi) {
        bool any = false;
        for (int i = first; i <= last; ++i) {
            const auto& s = stats[(size_t)i];
            double a = s.usedLow, b = s.usedHigh;
            bool has = s.any;
            if (layout.pitchFit == LanePitchFit::instrumentLimits) {
                const auto info = model.laneInfo(i);
                if (info.limitLow > 0 || info.limitHigh < 127) {
                    a = has ? std::min<double>(a, info.limitLow) : info.limitLow;
                    b = has ? std::max<double>(b, info.limitHigh) : info.limitHigh;
                    has = true;
                }
            }
            if (!has) continue;
            lo = any ? std::min(lo, a) : a;
            hi = any ? std::max(hi, b) : b;
            any = true;
        }
        if (!any) { lo = 54; hi = 66; }
        // 上下各留一点边；太窄的拉到至少 8 个半音，免得单音轨一条音符占满整行
        lo -= 1; hi += 1;
        if (hi - lo < 8) { const double c = (lo + hi) * .5; lo = c - 4; hi = c + 4; }
    };
    const int n = model.laneCount();
    for (int i = 0; i < n;) {
        const auto group = model.laneInfo(i).group;
        int last = i;
        if (group.isNotEmpty())
            while (last + 1 < n && model.laneInfo(last + 1).group == group) ++last;
        if (group.isNotEmpty()) {
            Row h;
            h.group = group;
            h.firstLane = i;
            h.lastLane = last;
            if (isGroupCollapsed(group)) {
                h.kind = Row::groupOverview;
                h.y = y;
                h.height = layout.laneHeight;
                pitchRange(i, last, h.pitchLow, h.pitchHigh);
                rows.push_back(h);
                y += h.height;
                i = last + 1;
                continue;
            }
            h.kind = Row::groupHeader;
            h.y = y;
            h.height = layout.groupHeaderHeight;
            rows.push_back(h);
            y += h.height;
        }
        for (int l = i; l <= last; ++l) {
            Row r;
            r.kind = Row::lane;
            r.laneIndex = l;
            r.group = group;
            r.firstLane = r.lastLane = l;
            r.y = y;
            r.height = layout.laneHeight;
            pitchRange(l, l, r.pitchLow, r.pitchHigh);
            rows.push_back(r);
            y += r.height;
        }
        i = last + 1;
    }
}

float MultiLaneCanvas::contentHeight() const { return rows.empty() ? 0 : rows.back().y + rows.back().height; }

juce::Rectangle<float> MultiLaneCanvas::contentArea() const {
    const float sb = 12;
    return {layout.headerWidth, layout.rulerHeight, std::max(1.f, getWidth() - layout.headerWidth - sb),
            std::max(1.f, getHeight() - layout.rulerHeight - sb)};
}

juce::Rectangle<float> MultiLaneCanvas::rowBounds(const Row& r) const {
    return {0, layout.rulerHeight + r.y - scrollY, (float)getWidth() - 12, r.height};
}

pianoroll::Viewport MultiLaneCanvas::viewportFor(const Row& r) const {
    const auto area = contentArea();
    const auto b = rowBounds(r);
    pianoroll::Viewport v;
    v.area = {area.getX(), b.getY() + 1, area.getWidth(), b.getHeight() - 2};
    v.timeStart = view.timeStart;
    v.timeSpan = view.timeSpan;
    const double span = r.pitchHigh - r.pitchLow + 1;
    v.rowHeight = (b.getHeight() - 2) / span;
    v.topPitch = r.pitchHigh;
    return v;
}

void MultiLaneCanvas::resized() {
    const int sb = 12;
    vbar.setBounds(getWidth() - sb, (int)layout.rulerHeight, sb, getHeight() - (int)layout.rulerHeight - sb);
    hbar.setBounds((int)layout.headerWidth, getHeight() - sb, getWidth() - (int)layout.headerWidth - sb, sb);
    setScrollY(scrollY);
    applyView(view, false);
}

// ---------------------------------------------------------------------------
// 视图

pianoroll::NavigationLimits MultiLaneCanvas::navLimits() const {
    const double ppq = std::max(1, model.unitsPerQuarter());
    const double len = std::max(model.documentLength(), ppq * 16);
    pianoroll::NavigationLimits l;
    l.minimumTimeSpan = ppq;               // 最近看一拍
    l.maximumTimeSpan = len * 1.1 + ppq * 4;
    l.timeEnd = len + ppq * 4;
    l.minimumPitchSpan = 1;
    return l;
}

pianoroll::ViewState MultiLaneCanvas::viewState() const { return view; }

void MultiLaneCanvas::setViewState(pianoroll::ViewState v) { applyView(v, false); }

void MultiLaneCanvas::applyView(pianoroll::ViewState v, bool userAction) {
    v.pitchLow = 0;
    v.pitchHigh = 127;   // 音高部分忽略（每条自适应）
    v = pianoroll::clampNavigation(v, navLimits());
    const bool changed = v.timeStart != view.timeStart || v.timeSpan != view.timeSpan;
    view = v;
    if (userAction) follow.userNavigated();
    updateScrollBars();
    positionPlayhead();
    if (changed) {
        repaint();
        if (onViewChanged) onViewChanged(view);
    }
}

void MultiLaneCanvas::fitAll() {
    const double ppq = std::max(1, model.unitsPerQuarter());
    pianoroll::ViewState v = view;
    v.timeStart = 0;
    v.timeSpan = std::max(model.documentLength(), ppq * 4) * 1.02;
    applyView(v, false);
}

void MultiLaneCanvas::setScrollY(float y) {
    const float maxY = std::max(0.f, contentHeight() - contentArea().getHeight());
    const float ny = juce::jlimit(0.f, maxY, y);
    if (ny != scrollY) {
        scrollY = ny;
        repaint();
    }
    updateScrollBars();
}

void MultiLaneCanvas::updateScrollBars() {
    const auto area = contentArea();
    vbar.setRangeLimits(0, std::max<double>(contentHeight(), area.getHeight()), juce::dontSendNotification);
    vbar.setCurrentRange(scrollY, area.getHeight(), juce::dontSendNotification);
    const auto lim = navLimits();
    hbar.setRangeLimits(0, std::max(lim.timeEnd, view.timeStart + view.timeSpan), juce::dontSendNotification);
    hbar.setCurrentRange(view.timeStart, view.timeSpan, juce::dontSendNotification);
}

void MultiLaneCanvas::scrollBarMoved(juce::ScrollBar* bar, double start) {
    if (bar == &vbar) {
        setScrollY((float)start);
    } else {
        auto v = view;
        v.timeStart = start;
        applyView(v, true);
    }
}

bool MultiLaneCanvas::densityMode() const {
    // 一个十六分音符不到 1 像素宽时改画音域色带
    const double tpp = view.timeSpan / std::max(1.f, contentArea().getWidth());
    return tpp > model.unitsPerQuarter() / 4.0;
}

// ---------------------------------------------------------------------------
// 播放光标

void MultiLaneCanvas::setPlayhead(double time, bool isPlaying) {
    playTime = time;
    playing = isPlaying;
    const auto next = follow.update(view, time, isPlaying, dragging, navLimits());
    if (next.timeStart != view.timeStart) applyView(next, false);   // 自动翻页才整体重绘
    positionPlayhead();
}

void MultiLaneCanvas::setFollowEnabled(bool e) { follow.setEnabled(e); }

void MultiLaneCanvas::positionPlayhead() {
    const auto area = contentArea();
    pianoroll::Viewport v;
    v.area = {area.getX(), 0, area.getWidth(), 1};
    v.timeStart = view.timeStart;
    v.timeSpan = view.timeSpan;
    const double x = v.timeToX(playTime);
    const bool visible = x >= area.getX() - 1 && x <= area.getRight() + 1 && model.laneCount() > 0;
    playhead->setVisible(visible);
    if (visible) playhead->setBounds((int)std::round(x) - 5, (int)layout.rulerHeight - 7, 11,
                                     (int)(area.getBottom() - layout.rulerHeight + 7));
}

// ---------------------------------------------------------------------------
// 绘制

void MultiLaneCanvas::paint(juce::Graphics& g) {
    static const bool perfLog = juce::SystemStats::getEnvironmentVariable("MIDI_READER_PERF", {}).isNotEmpty();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    struct Log { bool on; double t0; ~Log() { if (on) std::fprintf(stderr, "paint %.2f ms\n", juce::Time::getMillisecondCounterHiRes() - t0); } } log{perfLog, t0};
    g.fillAll(background);
    if (model.revision() != seenRevision) modelChanged();
    const auto area = contentArea();
    if (model.laneCount() == 0) {
        g.setColour(muted);
        g.setFont(font(15));
        g.drawFittedText(juce::String::fromUTF8("把 MIDI 文件拖到这里，或点左上角“打开”\n音频文件（wav / mp3 / flac…）也可以：会先按编制预设扒成 MIDI"), getLocalBounds(),
                         juce::Justification::centred, 2);
        return;
    }
    const double tpp = view.timeSpan / std::max(1.f, area.getWidth());
    const auto grid = meter.grid(view.timeStart, view.timeStart + view.timeSpan, tpp * 6);   // 一次 paint 只算一次网格

    {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(juce::Rectangle<int>(0, (int)layout.rulerHeight, getWidth() - 12,
                                                getHeight() - (int)layout.rulerHeight - 12));
        const auto clip = g.getClipBounds().toFloat();
        for (auto& r : rows) {
            const auto b = rowBounds(r);
            if (b.getBottom() < clip.getY() || b.getY() > clip.getBottom()) continue;
            paintRow(g, r, grid);
        }
        const float end = layout.rulerHeight + contentHeight() - scrollY;
        if (end < getHeight()) {
            g.setColour(panel);
            g.fillRect(0.f, end, layout.headerWidth, (float)getHeight() - end);
        }
    }
    paintRuler(g, grid);
    g.setColour(panel);
    g.fillRect((float)getWidth() - 12, (float)getHeight() - 12, 12.f, 12.f);
    g.fillRect(0.f, (float)getHeight() - 12, layout.headerWidth, 12.f);
    paintHover(g);
}

void MultiLaneCanvas::paintRuler(juce::Graphics& g, const std::vector<pianoroll::MeterGridMark>& grid) {
    const auto area = contentArea();
    const juce::Rectangle<float> ruler(0, 0, (float)getWidth(), layout.rulerHeight);
    g.setColour(panel);
    g.fillRect(ruler);
    g.setColour(line);
    g.drawHorizontalLine((int)layout.rulerHeight - 1, 0, (float)getWidth());
    pianoroll::Viewport v;
    v.area = {area.getX(), 0, area.getWidth(), layout.rulerHeight};
    v.timeStart = view.timeStart;
    v.timeSpan = view.timeSpan;
    g.setFont(font(11));
    float lastLabelRight = -1e9f;
    juce::Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(juce::Rectangle<float>(area.getX(), 0, area.getWidth(), layout.rulerHeight).toNearestInt());
    for (auto& m : grid) {
        const float x = (float)v.timeToX(m.time);
        if (m.barStart) {
            g.setColour(muted);
            g.drawVerticalLine((int)x, layout.rulerHeight * .45f, layout.rulerHeight);
            juce::String label(m.bar);
            if (m.signatureChange) {
                const auto pos = meter.positionAt(m.time);
                label << "  " << pos.numerator << "/" << pos.denominator;
            }
            const float w = (float)g.getCurrentFont().getStringWidth(label) + 4;
            if (x + 3 > lastLabelRight + 6) {
                g.setColour(m.signatureChange ? accent : ink.withAlpha(.8f));
                g.drawText(label, juce::Rectangle<float>(x + 3, 1, w, layout.rulerHeight * .6f), juce::Justification::centredLeft);
                lastLabelRight = x + 3 + w;
            }
        } else {
            g.setColour(line);
            g.drawVerticalLine((int)x, layout.rulerHeight * .75f, layout.rulerHeight);
        }
    }
}

juce::Colour MultiLaneCanvas::noteColour(const LaneNote& n, juce::Colour laneColour) const {
    if (n.flags & noteFlagKeySwitch) return keySwitch.withAlpha(.6f);
    if (n.flags & noteFlagHardRange) return hard;
    if (n.flags & (noteFlagReview | noteFlagSoftRange)) return warn;
    return laneColour;
}

void MultiLaneCanvas::paintRow(juce::Graphics& g, const Row& r, const std::vector<pianoroll::MeterGridMark>& grid) {
    const auto b = rowBounds(r);
    const auto area = contentArea();
    const auto content = juce::Rectangle<float>(area.getX(), b.getY(), area.getWidth(), b.getHeight());
    const bool isHighlighted = r.kind == Row::lane && r.laneIndex == highlighted;

    if (r.kind == Row::groupHeader) {
        g.setColour(panelAlt);
        g.fillRect(b);
        g.setColour(line);
        g.drawHorizontalLine((int)b.getBottom() - 1, 0, b.getRight());
        paintHeader(g, r, b);
        return;
    }
    // 底色：交替两档，高亮提亮一档
    const int idx = r.kind == Row::lane ? r.laneIndex : r.firstLane;
    g.setColour(isHighlighted ? laneHighlight : (idx % 2 ? laneBgAlt : laneBg));
    g.fillRect(content);
    // 网格竖线
    pianoroll::Viewport gv;
    gv.area = {area.getX(), b.getY(), area.getWidth(), b.getHeight()};
    gv.timeStart = view.timeStart;
    gv.timeSpan = view.timeSpan;
    for (auto& m : grid) {
        g.setColour(m.barStart ? barLine : beatLine);
        g.drawVerticalLine((int)gv.timeToX(m.time), b.getY(), b.getBottom());
    }
    g.setColour(line.withAlpha(.6f));
    g.drawHorizontalLine((int)b.getBottom() - 1, area.getX(), area.getRight());

    // 音符（裁到内容区）
    {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(content.toNearestInt());
        const auto vp = viewportFor(r);
        if (r.kind == Row::lane) {
            if (densityMode()) paintDensityBand(g, r, r.laneIndex, vp, content);
            else paintNotes(g, r, r.laneIndex, vp, content, false);
        } else {
            for (int l = r.firstLane; l <= r.lastLane; ++l) {
                if (densityMode()) paintDensityBand(g, r, l, vp, content);
                else paintNotes(g, r, l, vp, content, true);
            }
        }
        if (flashTicks > 0 && r.kind == Row::lane && r.laneIndex == flashLane) {
            const float x = (float)vp.timeToX(flashTime);
            g.setColour(accent.withAlpha(std::min(1.f, flashTicks / 20.f) * .9f));
            g.drawRect(juce::Rectangle<float>(x - 10, b.getY() + 1, 20, b.getHeight() - 2), 2.f);
        }
    }
    paintHeader(g, r, b);
}

void MultiLaneCanvas::paintNotes(juce::Graphics& g, const Row& r, int lane, const pianoroll::Viewport& vp,
                                 juce::Rectangle<float> clip, bool overview) {
    const auto& notes = model.laneNotes(lane);
    const auto colour = model.laneInfo(lane).colour;
    const bool silent = mixOf(lane) & mixSilent;
    // 只取可见音符：按 start 二分，起点往前推“最长音长”，不漏掉横跨进来的长音
    const double from = view.timeStart - model.longestNote(lane);
    const double to = view.timeStart + view.timeSpan;
    auto it = std::lower_bound(notes.begin(), notes.end(), from, [](const LaneNote& n, double t) { return n.start < t; });
    const double h = std::max(2.5, std::min(vp.rowHeight, 6.0));
    for (; it != notes.end() && it->start <= to; ++it) {
        const auto& n = *it;
        if (n.start + n.length < view.timeStart) continue;
        const bool isKs = n.flags & noteFlagKeySwitch;
        if (isKs && !layout.showKeySwitches) continue;
        const double pitch = juce::jlimit(r.pitchLow, r.pitchHigh, (double)n.pitch);   // 超出范围的压在边上
        const double y = vp.pitchToY(pitch) + (vp.rowHeight - h) * .5;
        const double x0 = vp.timeToX(n.start), x1 = vp.timeToX(n.start + n.length);
        auto rect = juce::Rectangle<float>((float)x0, (float)y, (float)std::max(1.5, x1 - x0 - .5), (float)h);
        if (!rect.intersects(clip)) continue;
        auto c = noteColour(n, colour);
        if (overview) c = c.withAlpha(.85f);
        if (silent) c = c.withMultipliedAlpha(silentAlpha);
        g.setColour(c);
        g.fillRect(rect);
    }
}

void MultiLaneCanvas::paintDensityBand(juce::Graphics& g, const Row& r, int lane, const pianoroll::Viewport& vp,
                                       juce::Rectangle<float> clip) {
    // 按像素列汇总“这一列有没有声音、音高从哪到哪”，画成音域色带
    const int x0 = (int)std::floor(clip.getX()), cols = (int)std::ceil(clip.getWidth()) + 1;
    std::vector<float> lo((size_t)cols, 1e9f), hi((size_t)cols, -1e9f);
    std::vector<std::uint8_t> worst((size_t)cols, 0);
    const auto& notes = model.laneNotes(lane);
    const double from = view.timeStart - model.longestNote(lane), to = view.timeStart + view.timeSpan;
    auto it = std::lower_bound(notes.begin(), notes.end(), from, [](const LaneNote& n, double t) { return n.start < t; });
    for (; it != notes.end() && it->start <= to; ++it) {
        const auto& n = *it;
        if ((n.flags & noteFlagKeySwitch) && !layout.showKeySwitches) continue;
        const int a = std::max(0, (int)std::floor(vp.timeToX(n.start)) - x0);
        const int bcol = std::min(cols - 1, std::max(a, (int)std::floor(vp.timeToX(n.start + n.length)) - x0));
        if (a >= cols || bcol < 0) continue;
        const float p = (float)juce::jlimit(r.pitchLow, r.pitchHigh, (double)n.pitch);
        const std::uint8_t sev = (n.flags & noteFlagHardRange) ? 2 : (n.flags & (noteFlagReview | noteFlagSoftRange)) ? 1 : 0;
        for (int c = a; c <= bcol; ++c) {
            lo[(size_t)c] = std::min(lo[(size_t)c], p);
            hi[(size_t)c] = std::max(hi[(size_t)c], p);
            worst[(size_t)c] = std::max(worst[(size_t)c], sev);
        }
    }
    const auto colour = model.laneInfo(lane).colour;
    for (int c = 0; c < cols; ++c) {
        if (hi[(size_t)c] < lo[(size_t)c]) continue;
        const float yTop = (float)vp.pitchToY(hi[(size_t)c]);
        const float yBot = (float)(vp.pitchToY(lo[(size_t)c]) + std::max(1.0, vp.rowHeight));
        const auto cc = worst[(size_t)c] == 2 ? hard : worst[(size_t)c] == 1 ? warn : colour;
        g.setColour((mixOf(lane) & mixSilent) ? cc.withMultipliedAlpha(silentAlpha) : cc);
        g.fillRect((float)(x0 + c), yTop, 1.f, std::max(1.f, yBot - yTop));
    }
}

void MultiLaneCanvas::paintHeader(juce::Graphics& g, const Row& r, juce::Rectangle<float> b) {
    auto h = b.withWidth(layout.headerWidth);
    g.setColour(r.kind == Row::groupHeader ? panelAlt : (r.kind == Row::lane && r.laneIndex == highlighted ? laneHighlight : panel));
    g.fillRect(h);
    g.setColour(line);
    g.drawVerticalLine((int)h.getRight() - 1, h.getY(), h.getBottom());
    g.drawHorizontalLine((int)h.getBottom() - 1, h.getX(), h.getRight());

    if (r.kind != Row::lane) {
        // 组标题：▶/▼ 组名（声部数）
        const bool c = r.kind == Row::groupOverview;
        auto t = h.reduced(6, 0);
        juce::Path tri;
        const float cy = t.getCentreY();
        if (c) tri.addTriangle(t.getX(), cy - 4, t.getX(), cy + 4, t.getX() + 7, cy);
        else tri.addTriangle(t.getX(), cy - 3, t.getX() + 8, cy - 3, t.getX() + 4, cy + 4);
        g.setColour(muted);
        g.fillPath(tri);
        g.setColour(ink);
        g.setFont(font(12, true));
        g.drawFittedText(r.group + "  (" + juce::String(r.lastLane - r.firstLane + 1) + ")",
                         t.withTrimmedLeft(12).toNearestInt(), juce::Justification::centredLeft, 1);
        return;
    }
    const auto info = model.laneInfo(r.laneIndex);
    const auto& s = stats[(size_t)r.laneIndex];
    const auto f = mixOf(r.laneIndex);
    g.setColour(info.colour);
    g.fillRect(h.withWidth(4).reduced(0, 2));
    // M / S 按钮在最右
    const auto mb = mixButton(h, false), sb = mixButton(h, true);
    auto button = [&](juce::Rectangle<float> rb, const char* text, bool on, juce::Colour onColour) {
        if (on) {
            g.setColour(onColour);
            g.fillRoundedRectangle(rb, 3);
            g.setColour(background);
        } else {
            g.setColour(line);
            g.drawRoundedRectangle(rb.reduced(.5f), 3, 1);
            g.setColour(muted);
        }
        g.setFont(font(std::max(9.f, rb.getHeight() * .62f), true));
        g.drawText(text, rb, juce::Justification::centred);
    };
    button(mb, "M", f & mixMuted, muteOn);
    button(sb, "S", f & mixSoloed, soloOn);
    auto t = h.reduced(8, 1).withTrimmedLeft(2).withRight(mb.getX() - 2);
    // 右侧小标记：待确认 / 超极限
    auto marks = t.removeFromRight(20);
    float mx = marks.getRight();
    auto dot = [&](juce::Colour c) {
        mx -= 9;
        g.setColour(c);
        g.fillEllipse(mx, marks.getCentreY() - 3.5f, 7, 7);
    };
    if (s.hard) dot(hard);
    if (s.review) dot(warn);
    g.setColour((f & mixSilent) ? muted : ink);
    g.setFont(font(layout.laneHeight >= 26 ? 13.f : 11.f));
    juce::String name = info.name;
    if (info.sourceTrackCount > 1) name << "  x" << info.sourceTrackCount;
    g.drawFittedText(name, t.toNearestInt(), juce::Justification::centredLeft, 1, .8f);
}

void MultiLaneCanvas::paintHover(juce::Graphics& g) {
    if (!hasHover || hoverLane < 0 || hoverLane >= model.laneCount()) return;
    const auto& n = hoverNote;
    const auto info = model.laneInfo(hoverLane);
    const auto pos = meter.positionAt(n.start);
    const double ppq = std::max(1, model.unitsPerQuarter());
    juce::StringArray lines;
    lines.add(info.name);
    lines.add(juce::String(pianoroll::pitchName(n.pitch, 5)) + "  (MIDI " + juce::String(n.pitch) + ")   "
              + juce::String::fromUTF8("力度 ") + juce::String(n.velocity));
    lines.add(juce::String::fromUTF8("第 ") + juce::String(pos.bar) + juce::String::fromUTF8(" 小节 第 ")
              + juce::String(pos.beat, 2) + juce::String::fromUTF8(" 拍   长 ") + juce::String(n.length / ppq, 2)
              + juce::String::fromUTF8(" 拍"));
    if (n.flags & noteFlagKeySwitch) lines.add(juce::String::fromUTF8("判为 keyswitch"));
    if (n.flags & noteFlagHardRange)
        lines.add(juce::String::fromUTF8("超出演奏极限（") + juce::String(pianoroll::pitchName(info.limitLow, 5)) + "–"
                  + juce::String(pianoroll::pitchName(info.limitHigh, 5)) + juce::String::fromUTF8("）"));
    else if (n.flags & noteFlagSoftRange)
        lines.add(juce::String::fromUTF8("超出舒适音域（") + juce::String(pianoroll::pitchName(info.comfortLow, 5)) + "–"
                  + juce::String(pianoroll::pitchName(info.comfortHigh, 5)) + juce::String::fromUTF8("）"));
    if (n.flags & noteFlagReview) lines.add(juce::String::fromUTF8("待确认：见左栏“规则结论”"));
    lines.add(juce::String::fromUTF8("原始音符 #") + juce::String((int)n.sourceNote));

    g.setFont(font(12));
    float w = 0;
    for (auto& l : lines) w = std::max(w, (float)g.getCurrentFont().getStringWidth(l));
    const float lh = 16, pad = 6;
    juce::Rectangle<float> box(hoverPos.x + 14, hoverPos.y + 14, w + pad * 2, lines.size() * lh + pad * 2);
    if (box.getRight() > getWidth() - 14) box.setX(hoverPos.x - box.getWidth() - 10);
    if (box.getBottom() > getHeight() - 14) box.setY(hoverPos.y - box.getHeight() - 10);
    g.setColour(tooltipBg);
    g.fillRoundedRectangle(box, 4);
    g.setColour(line);
    g.drawRoundedRectangle(box, 4, 1);
    for (int i = 0; i < lines.size(); ++i) {
        g.setColour(i == 0 ? info.colour.brighter(.3f) : ink);
        g.drawText(lines[i], juce::Rectangle<float>(box.getX() + pad, box.getY() + pad + i * lh, w + 2, lh),
                   juce::Justification::centredLeft);
    }
}

// ---------------------------------------------------------------------------
// 命中

int MultiLaneCanvas::rowAt(juce::Point<float> p) const {
    if (p.y < layout.rulerHeight || p.x > getWidth() - 12 || p.y > getHeight() - 12) return -1;
    const float cy = p.y - layout.rulerHeight + scrollY;
    for (size_t i = 0; i < rows.size(); ++i)
        if (cy >= rows[i].y && cy < rows[i].y + rows[i].height) return (int)i;
    return -1;
}

juce::Rectangle<float> MultiLaneCanvas::mixButton(juce::Rectangle<float> header, bool solo) const {
    const float size = juce::jlimit(12.f, 18.f, header.getHeight() - 8.f);
    const float right = header.getX() + layout.headerWidth - 6;
    const float x = solo ? right - size : right - 2 * size - 3;
    return {x, header.getCentreY() - size / 2, size, size};
}

int MultiLaneCanvas::mixButtonAt(juce::Point<float> p, int& lane) const {
    if (p.x >= layout.headerWidth) return 0;
    const int ri = rowAt(p);
    if (ri < 0 || rows[(size_t)ri].kind != Row::lane) return 0;
    const auto h = rowBounds(rows[(size_t)ri]).withWidth(layout.headerWidth);
    lane = rows[(size_t)ri].laneIndex;
    if (mixButton(h, false).expanded(1).contains(p)) return 1;
    if (mixButton(h, true).expanded(1).contains(p)) return 2;
    return 0;
}

juce::String MultiLaneCanvas::getTooltip() {
    int lane = -1;
    switch (mixButtonAt(getMouseXYRelative().toFloat(), lane)) {
        case 1: return juce::String::fromUTF8("静音（M 键）");
        case 2: return juce::String::fromUTF8("独奏（S 键）。可以同时独奏几条；Ctrl+点击：只听这一条");
        default: return {};
    }
}

void MultiLaneCanvas::setLaneMix(std::vector<std::uint8_t> laneFlags) {
    mix = std::move(laneFlags);
    repaint();
}

const LaneNote* MultiLaneCanvas::noteAt(int lane, juce::Point<float> p) const {
    const Row* row = nullptr;
    for (auto& r : rows)
        if (r.kind == Row::lane && r.laneIndex == lane) row = &r;
    if (!row) return nullptr;
    const auto vp = viewportFor(*row);
    const auto& notes = model.laneNotes(lane);
    const double t = vp.xToTime(p.x);
    const double tol = view.timeSpan / std::max(1.f, contentArea().getWidth()) * 3;
    const double h = std::max(2.5, std::min(vp.rowHeight, 6.0));
    auto it = std::lower_bound(notes.begin(), notes.end(), t - model.longestNote(lane) - tol,
                               [](const LaneNote& n, double x) { return n.start < x; });
    const LaneNote* best = nullptr;
    double bestDist = 1e9;
    for (; it != notes.end() && it->start <= t + tol; ++it) {
        if ((it->flags & noteFlagKeySwitch) && !layout.showKeySwitches) continue;
        if (t < it->start - tol || t > it->start + it->length + tol) continue;
        const double pitch = juce::jlimit(row->pitchLow, row->pitchHigh, (double)it->pitch);
        const double y = vp.pitchToY(pitch) + vp.rowHeight * .5;
        const double d = std::abs(y - p.y);
        if (d <= std::max(4.0, h) && d < bestDist) { best = &*it; bestDist = d; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// 鼠标

void MultiLaneCanvas::mouseDown(const juce::MouseEvent& e) {
    const auto p = e.position;
    pressPos = p;
    dragging = false;
    leftPending = false;
    if (model.laneCount() == 0) return;
    const auto area = contentArea();
    if (e.mods.isMiddleButtonDown()) {
        drag = pianoroll::beginMiddleDrag(pianoroll::PointerRegion::notes, {p.x, p.y}, view,
                                          {area.getX(), area.getY(), area.getWidth(), area.getHeight()});
        dragStartScroll = scrollY;
        dragging = true;
        return;
    }
    if (p.y < layout.rulerHeight && p.x >= area.getX()) {
        pianoroll::Viewport v;
        v.area = {area.getX(), 0, area.getWidth(), 1};
        v.timeStart = view.timeStart;
        v.timeSpan = view.timeSpan;
        follow.transportLocated();
        if (onSeek) onSeek(std::max(0.0, v.xToTime(p.x)));
        return;
    }
    const int ri = rowAt(p);
    if (ri < 0) return;
    const auto& r = rows[(size_t)ri];
    if (e.mods.isPopupMenu()) {
        if (r.kind == Row::lane) {
            setHighlightedLane(r.laneIndex);
            if (onLaneClicked) onLaneClicked(r.laneIndex);
            if (auto* n = noteAt(r.laneIndex, p); n && onNoteRightClicked)
                onNoteRightClicked(r.laneIndex, *n, e.getScreenPosition());
            else if (onLaneRightClicked)
                onLaneRightClicked(r.laneIndex, e.getScreenPosition());
        }
        return;
    }
    if (r.kind != Row::lane && p.x < layout.headerWidth) {
        setGroupCollapsed(r.group, r.kind == Row::groupHeader);
        return;
    }
    int mixLane = -1;
    if (const int which = mixButtonAt(p, mixLane)) {
        if (which == 1 && onMuteClicked) onMuteClicked(mixLane);
        if (which == 2 && onSoloClicked) onSoloClicked(mixLane, e.mods.isCommandDown());
        return;
    }
    // 左键：先当成单击，拖动超过几像素再变成平移
    drag = pianoroll::beginMiddleDrag(pianoroll::PointerRegion::notes, {p.x, p.y}, view,
                                      {area.getX(), area.getY(), area.getWidth(), area.getHeight()});
    dragStartScroll = scrollY;
    leftPending = true;
}

void MultiLaneCanvas::mouseDrag(const juce::MouseEvent& e) {
    if (leftPending && e.position.getDistanceFrom(pressPos) > 4) {
        leftPending = false;
        dragging = true;
    }
    if (!dragging) return;
    const auto res = pianoroll::dragNavigation(drag, {e.position.x, e.position.y}, navLimits());
    if (res.kind == pianoroll::NavigationKind::pan || res.kind == pianoroll::NavigationKind::none) {
        auto v = view;
        v.timeStart = res.view.timeStart;
        applyView(v, true);
    }
    setScrollY(dragStartScroll - (e.position.y - pressPos.y));
}

void MultiLaneCanvas::mouseUp(const juce::MouseEvent& e) {
    if (leftPending) {
        leftPending = false;
        const int ri = rowAt(e.position);
        if (ri >= 0 && rows[(size_t)ri].kind == Row::lane) {
            const int lane = rows[(size_t)ri].laneIndex;
            setHighlightedLane(lane);
            if (onLaneClicked) onLaneClicked(lane);
        } else if (ri >= 0) {
            const auto& r = rows[(size_t)ri];
            setGroupCollapsed(r.group, r.kind == Row::groupHeader);
        }
    }
    dragging = false;
}

void MultiLaneCanvas::mouseDoubleClick(const juce::MouseEvent& e) {
    const int ri = rowAt(e.position);
    if (ri < 0 || e.position.y < layout.rulerHeight) return;
    int mixLane = -1;
    if (mixButtonAt(e.position, mixLane)) return;   // 连点 M/S 不打开细看
    const auto& r = rows[(size_t)ri];
    if (r.kind == Row::lane && onLaneExpandRequested) onLaneExpandRequested(r.laneIndex);
}

void MultiLaneCanvas::mouseMove(const juce::MouseEvent& e) {
    hoverPos = e.position;
    const int ri = rowAt(e.position);
    const LaneNote* n = nullptr;
    int lane = -1;
    if (ri >= 0 && rows[(size_t)ri].kind == Row::lane && e.position.x >= layout.headerWidth) {
        lane = rows[(size_t)ri].laneIndex;
        n = noteAt(lane, e.position);
    }
    const bool had = hasHover;
    if (n) {
        const bool same = had && hoverLane == lane && hoverNote.sourceNote == n->sourceNote;
        hasHover = true;
        hoverLane = lane;
        hoverNote = *n;
        if (!same && onNoteHovered) onNoteHovered(lane, *n);
        repaint();
    } else if (had) {
        hasHover = false;
        repaint();
    }
}

void MultiLaneCanvas::mouseExit(const juce::MouseEvent&) {
    if (hasHover) {
        hasHover = false;
        repaint();
    }
}

void MultiLaneCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (model.laneCount() == 0) return;
    const auto area = contentArea();
    pianoroll::InputModifiers mods;
    mods.command = e.mods.isCommandDown();
    mods.shift = e.mods.isShiftDown();
    const bool horizontal = std::abs(w.deltaX) > .01f;
    if (mods.command || mods.shift || horizontal) {
        // Ctrl+滚轮 缩放时间、Shift+滚轮 / 横向滚动 平移时间——与 pianoroll-core 的钢琴窗一致
        const auto res = pianoroll::wheelNavigation(view, {area.getX(), area.getY(), area.getWidth(), area.getHeight()},
                                                    {e.position.x, e.position.y}, w.deltaX, w.deltaY, mods, navLimits());
        auto v = view;
        v.timeStart = res.view.timeStart;
        v.timeSpan = res.view.timeSpan;
        applyView(v, true);
        if (!horizontal || mods.command) return;
    }
    if (!mods.command && !mods.shift) setScrollY(scrollY - w.deltaY * layout.laneHeight * 4);   // 普通滚轮：上下翻声部
}

// ---------------------------------------------------------------------------
// 外部驱动

void MultiLaneCanvas::setHighlightedLane(int lane) {
    if (lane == highlighted) return;
    highlighted = lane;
    repaint();
}

void MultiLaneCanvas::revealNote(int lane, double time) {
    if (lane < 0 || lane >= model.laneCount()) return;
    const auto group = model.laneInfo(lane).group;
    if (group.isNotEmpty() && isGroupCollapsed(group)) setGroupCollapsed(group, false);
    for (auto& r : rows) {
        if (r.kind != Row::lane || r.laneIndex != lane) continue;
        const float h = contentArea().getHeight();
        if (r.y < scrollY || r.y + r.height > scrollY + h) setScrollY(r.y - h * .4f);
    }
    if (time < view.timeStart || time > view.timeStart + view.timeSpan * .9) {
        auto v = view;
        v.timeStart = std::max(0.0, time - view.timeSpan * .3);
        applyView(v, false);
    }
    setHighlightedLane(lane);
    flashLane = lane;
    flashTime = time;
    flashTicks = 40;
    startTimerHz(30);
    repaint();
}

void MultiLaneCanvas::timerCallback() {
    if (--flashTicks <= 0) {
        flashTicks = 0;
        stopTimer();
    }
    repaint();
}

} // namespace reader
