#include "app/SidePanel.h"
#include "app/Theme.h"
#include "core/Instruments.h"

#include <PianoRollCore/Navigation.h>
#include <set>

namespace reader {

using namespace theme;
static juce::String U(const char* s) { return juce::String::fromUTF8(s); }

// ---------------------------------------------------------------------------
// 声部列表

class SidePanel::LaneList : public juce::ListBox, private juce::ListBoxModel {
public:
    explicit LaneList(SidePanel& p) : owner(p) {
        setModel(this);
        setRowHeight(36);
        setMultipleSelectionEnabled(true);
        setColour(juce::ListBox::backgroundColourId, panel);
        setOutlineThickness(0);
    }
    int getNumRows() override { return owner.doc.laneModel().laneCount(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
        auto& m = owner.doc.laneModel();
        if (row >= m.laneCount()) return;
        const auto& pl = owner.doc.projection().lanes[(size_t)row];
        const auto info = m.laneInfo(row);
        g.setColour(selected ? laneHighlight : (row % 2 ? panel : panelAlt));
        g.fillRect(0, 0, w, h);
        g.setColour(info.colour);
        g.fillRect(0, 3, 4, h - 6);
        int review = 0, hardN = 0, ks = 0;
        for (auto& n : pl.notes) {
            if (n.flags & core::noteFlagKeySwitch) { ++ks; continue; }
            review += (n.flags & core::noteFlagReview) != 0;
            hardN += (n.flags & core::noteFlagHardRange) != 0;
        }
        auto r = juce::Rectangle<int>(10, 2, w - 14, h - 4);
        auto right = r.removeFromRight(70);
        g.setFont(font(13));
        g.setColour(ink);
        juce::String name = info.name;
        if (owner.soloLane == row) name = U("[独奏] ") + name;
        g.drawFittedText(name, r.removeFromTop(r.getHeight() / 2 + 1), juce::Justification::centredLeft, 1, .85f);
        g.setFont(font(11));
        g.setColour(muted);
        juce::String sub;
        const auto* spec = pl.unassigned() ? nullptr : &owner.doc.sidecar().lanes[(size_t)row];
        if (pl.unassigned()) sub = U("未归类");
        else if (spec && !spec->instrument.empty()) {
            if (auto* inst = core::instrumentById(spec->instrument)) sub = toJuce(inst->nameZh);
            if (spec->instrumentSource == "user") sub << U("（你指定）");
            else if (spec->instrumentSource == "program") sub << U("（按音色号猜）");
        } else sub = U("乐器未知");
        sub << "  " << juce::String((int)pl.notes.size() - ks) << U(" 音");
        if (pl.sources.size() > 1) sub << "  " << juce::String((int)pl.sources.size()) << U(" 个来源");
        g.drawFittedText(sub, r, juce::Justification::centredLeft, 1, .85f);
        g.setFont(font(11));
        int y = right.getY() + 2;
        auto tag = [&](juce::Colour c, const juce::String& t) {
            g.setColour(c);
            g.drawText(t, right.getX(), y, right.getWidth(), 14, juce::Justification::centredRight);
            y += 14;
        };
        if (hardN) tag(hard, juce::String(hardN) + U(" 超极限"));
        if (review) tag(warn, juce::String(review) + U(" 待确认"));
        if (ks && y < right.getBottom() - 10) tag(muted, juce::String(ks) + " KS");
    }
    void selectedRowsChanged(int last) override {
        if (!suppress && last >= 0 && owner.onLaneSelected) owner.onLaneSelected(last);
    }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        if (owner.onExpand) owner.onExpand(row);
    }
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) {
            if (!isRowSelected(row)) selectRow(row);
            owner.showLaneMenu(row);
        }
    }
    juce::String getTooltipForRow(int row) override {
        if (row >= (int)owner.doc.projection().lanes.size()) return {};
        const auto& pl = owner.doc.projection().lanes[(size_t)row];
        juce::StringArray s;
        for (auto& id : pl.sources) s.add(owner.streamLabel(id));
        return U("来源：\n") + s.joinIntoString("\n") + U("\n\n右键：改乐器、合并、拆出、改名…");
    }
    bool suppress = false;
private:
    SidePanel& owner;
};

// ---------------------------------------------------------------------------
// 规则结论

class SidePanel::DecisionList : public juce::ListBox, private juce::ListBoxModel {
public:
    explicit DecisionList(SidePanel& p) : owner(p) {
        setModel(this);
        setRowHeight(58);
        setColour(juce::ListBox::backgroundColourId, panel);
        setOutlineThickness(0);
    }
    std::vector<int> items;   // sidecar.decisions 下标
    void rebuild(bool pendingOnly) {
        items.clear();
        const auto& ds = owner.doc.sidecar().decisions;
        // 待确认排前面
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < ds.size(); ++i) {
                const bool pend = ds[i].pending();
                if ((pass == 0) != pend) continue;
                if (pendingOnly && !pend) continue;
                items.push_back((int)i);
            }
        updateContent();
        repaint();
    }
    int getNumRows() override { return (int)items.size(); }
    const core::Decision* at(int row) const {
        if (row < 0 || row >= (int)items.size()) return nullptr;
        return &owner.doc.sidecar().decisions[(size_t)items[(size_t)row]];
    }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow(int row, bool selected, juce::Component* existing) override {
        auto* c = dynamic_cast<RowComp*>(existing);
        if (!c) {
            delete existing;
            c = new RowComp(*this);
        }
        c->set(row, selected);
        return c;
    }
    void listBoxItemClicked(int row, const juce::MouseEvent&) override { reveal(row); }
    void reveal(int row) {
        if (auto* d = at(row)) {
            const auto [lane, tick] = owner.firstAffected(*d);
            if (lane >= 0 && owner.onReveal) owner.onReveal(lane, tick);
        }
    }
    void act(int row, const char* verdict) {
        if (auto* d = at(row)) {
            const auto id = toJuce(d->id);
            owner.runAndReport(owner.doc.decide(id, verdict));
        }
    }

    class RowComp : public juce::Component, public juce::SettableTooltipClient {
    public:
        explicit RowComp(DecisionList& l) : list(l) {
            for (auto* b : {&yes, &no, &undo}) {
                addAndMakeVisible(*b);
                b->setColour(juce::TextButton::buttonColourId, line);
            }
            yes.onClick = [this] { list.act(row, core::statusConfirmed); };
            no.onClick = [this] { list.act(row, core::statusRejected); };
            undo.onClick = [this] { list.act(row, "reset"); };
            yes.setTooltip(U("确认：按这条结论处理"));
            no.setTooltip(U("否决：不按这条处理，也不再标黄"));
            undo.setTooltip(U("撤回你的确认/否决，交还给自动判断"));
        }
        void set(int r, bool s) {
            row = r;
            selected = s;
            const auto* d = list.at(r);
            const bool user = d && d->userSet();
            undo.setVisible(user);
            yes.setVisible(d && !user);
            no.setVisible(d && !user);
            if (d && !user && d->status == core::statusAuto) yes.setVisible(false);   // 已自动应用，只需要能否决
            setTooltip(d ? toJuce(d->id) : juce::String());
            resized();
            repaint();
        }
        void resized() override {
            auto r = getLocalBounds().reduced(6, 4).removeFromTop(20);
            if (undo.isVisible()) undo.setBounds(r.removeFromRight(44));
            if (no.isVisible()) no.setBounds(r.removeFromRight(44));
            r.removeFromRight(4);
            if (yes.isVisible()) yes.setBounds(r.removeFromRight(44));
        }
        void paint(juce::Graphics& g) override {
            const auto* d = list.at(row);
            if (!d) return;
            g.setColour(selected ? laneHighlight : (row % 2 ? panel : panelAlt));
            g.fillAll();
            auto r = getLocalBounds().reduced(8, 4);
            auto top = r.removeFromTop(20);
            juce::String chip;
            juce::Colour cc;
            if (d->status == core::statusReview) { chip = U("待确认"); cc = warn; }
            else if (d->status == core::statusAuto) { chip = U("已自动"); cc = juce::Colour(0xff7fb069); }
            else if (d->status == core::statusConfirmed) { chip = U("你确认了"); cc = juce::Colour(0xff6fa8dc); }
            else { chip = U("你否决了"); cc = muted; }
            g.setFont(font(11, true));
            const int cw = g.getCurrentFont().getStringWidth(chip) + 10;
            auto chipR = top.removeFromLeft(cw).withSizeKeepingCentre(cw, 16);
            g.setColour(cc.withAlpha(.2f));
            g.fillRoundedRectangle(chipR.toFloat(), 3);
            g.setColour(cc);
            g.drawText(chip, chipR, juce::Justification::centred);
            top.removeFromLeft(6);
            int buttons = (yes.isVisible() ? 48 : 0) + (no.isVisible() ? 48 : 0) + (undo.isVisible() ? 48 : 0);
            top.removeFromRight(buttons);
            g.setFont(font(12.5f));
            g.setColour(ink);
            g.drawFittedText(list.owner.decisionTitle(*d), top, juce::Justification::centredLeft, 1, .8f);
            g.setFont(font(11));
            g.setColour(muted);
            g.drawFittedText(toJuce(d->reason), r, juce::Justification::topLeft, 2, .85f);
        }
        void mouseUp(const juce::MouseEvent& e) override {
            list.selectRow(row);
            if (!e.mods.isPopupMenu()) list.reveal(row);
        }
        DecisionList& list;
        int row = -1;
        bool selected = false;
        juce::TextButton yes{U("确认")}, no{U("否决")}, undo{U("撤回")};
    };
    SidePanel& owner;
};

// ---------------------------------------------------------------------------

SidePanel::SidePanel(Document& d) : doc(d) {
    lanes = std::make_unique<LaneList>(*this);
    decisions = std::make_unique<DecisionList>(*this);
    addAndMakeVisible(*lanes);
    addAndMakeVisible(*decisions);
    for (auto* l : {&lanesTitle, &decisionsTitle, &info}) {
        addAndMakeVisible(*l);
        l->setColour(juce::Label::textColourId, muted);
        l->setFont(font(12, true));
    }
    info.setFont(font(11));
    info.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(pendingOnly);
    pendingOnly.setColour(juce::ToggleButton::textColourId, muted);
    pendingOnly.onClick = [this] { decisions->rebuild(pendingOnly.getToggleState()); };
    refresh();
}

SidePanel::~SidePanel() = default;

void SidePanel::paint(juce::Graphics& g) {
    g.fillAll(panel);
    g.setColour(line);
    g.drawVerticalLine(getWidth() - 1, 0, (float)getHeight());
}

void SidePanel::resized() {
    auto r = getLocalBounds().withTrimmedRight(1);
    info.setBounds(r.removeFromBottom(54).reduced(8, 4));
    const int half = (int)(r.getHeight() * .52f);
    auto top = r.removeFromTop(half);
    lanesTitle.setBounds(top.removeFromTop(24).reduced(8, 0));
    lanes->setBounds(top);
    auto head = r.removeFromTop(26).reduced(8, 0);
    pendingOnly.setBounds(head.removeFromRight(110));
    decisionsTitle.setBounds(head);
    decisions->setBounds(r);
}

void SidePanel::refresh() {
    lanes->suppress = true;
    lanes->updateContent();
    lanes->repaint();
    lanes->suppress = false;
    decisions->rebuild(pendingOnly.getToggleState());
    if (!doc.hasFile()) {
        lanesTitle.setText(U("声部"), juce::dontSendNotification);
        decisionsTitle.setText(U("规则结论"), juce::dontSendNotification);
        info.setText({}, juce::dontSendNotification);
        return;
    }
    int pending = 0;
    for (auto& d : doc.sidecar().decisions) pending += d.pending();
    lanesTitle.setText(U("声部  ") + juce::String(doc.laneModel().laneCount()), juce::dontSendNotification);
    decisionsTitle.setText(U("规则结论  ") + juce::String((int)doc.sidecar().decisions.size())
                               + (pending ? U("（") + juce::String(pending) + U(" 条待确认）") : juce::String()),
                           juce::dontSendNotification);
    const auto& f = doc.smfFile();
    juce::StringArray lines;
    lines.add(juce::String("type ") + juce::String(f.format) + "  PPQ " + juce::String(f.ppq) + "  "
              + juce::String((int)f.chunks.size()) + U(" 轨  ") + juce::String((int)f.noteCount()) + U(" 个音符"));
    if (f.orphanNoteOffs || f.unterminatedNotes || f.retriggeredNotes)
        lines.add(U("解析：孤立 note-off ") + juce::String(f.orphanNoteOffs) + U("，未结束音 ")
                  + juce::String(f.unterminatedNotes) + U("，同音重触发 ") + juce::String(f.retriggeredNotes));
    for (auto& w : doc.projection().warnings) lines.add(U("注意：") + toJuce(w));
    if (doc.sidecarStatus().isNotEmpty()) lines.add(doc.sidecarStatus());
    info.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
}

void SidePanel::setHighlightedLane(int lane) {
    lanes->suppress = true;
    if (lane < 0) lanes->deselectAllRows();
    else if (!lanes->isRowSelected(lane)) {
        lanes->selectRow(lane);
        lanes->scrollToEnsureRowIsOnscreen(lane);
    }
    lanes->suppress = false;
}

void SidePanel::setSoloLane(int lane) {
    soloLane = lane;
    lanes->repaint();
}

void SidePanel::runAndReport(const juce::String& error) {
    if (error.isNotEmpty() && onMessage) onMessage(error);
}

juce::String SidePanel::streamLabel(const std::string& id) const {
    const auto* st = doc.smfFile().stream(id);
    if (!st) return toJuce(id) + U("（文件里没有）");
    juce::String s = U("轨 ") + juce::String(st->chunk);
    if (!st->trackName.empty()) s << U("「") << toJuce(st->trackName) << U("」");
    s << U(" 通道 ") << juce::String(st->channel + 1) << "  " << juce::String((int)st->notes.size()) << U(" 音");
    return s;
}

int SidePanel::laneOfStream(const std::string& id) const {
    const auto& ls = doc.projection().lanes;
    for (size_t i = 0; i < ls.size(); ++i)
        for (auto& s : ls[i].sources)
            if (s == id) return (int)i;
    return -1;
}

std::pair<int, double> SidePanel::firstAffected(const core::Decision& d) const {
    int lane = -1;
    if (d.rule == "channelSplit") {
        for (auto& s : d.params["streams"]) {
            lane = laneOfStream(s.get<std::string>());
            if (lane >= 0) break;
        }
    } else {
        lane = laneOfStream(d.target);
    }
    if (lane < 0) return {-1, 0};
    const auto* st = doc.smfFile().stream(d.rule == "channelSplit" ? d.params["streams"][0].get<std::string>() : d.target);
    std::set<int> pitches;
    if (d.rule == "keySwitch")
        for (auto& p : d.params["pitches"]) pitches.insert(p.get<int>());
    if (st)
        for (auto& n : st->notes) {
            if (d.rule == "keySwitch" && !pitches.count(n.pitch)) continue;
            if (d.rule == "octaveShift") {
                // 找第一个原位超极限的音
                const auto& spec = doc.projection().lanes[(size_t)lane].info;
                if (n.pitch >= spec.limitLow && n.pitch <= spec.limitHigh) continue;
            }
            return {lane, (double)n.start};
        }
    const auto& notes = doc.projection().lanes[(size_t)lane].notes;
    return {lane, notes.empty() ? 0.0 : (double)notes.front().start};
}

juce::String SidePanel::decisionTitle(const core::Decision& d) const {
    auto laneName = [&](const std::string& streamId) {
        const int l = laneOfStream(streamId);
        return l >= 0 ? toJuce(doc.projection().lanes[(size_t)l].info.name) : toJuce(streamId);
    };
    if (d.rule == "channelSplit") {
        const auto st = d.params["streams"][0].get<std::string>();
        const auto* s = doc.smfFile().stream(st);
        const auto name = s && !s->trackName.empty() ? toJuce(s->trackName) : U("轨 ") + toJuce(d.target.substr(1));
        return name + U("：按通道拆成 ") + juce::String((int)d.params["streams"].size()) + U(" 条");
    }
    if (d.rule == "keySwitch") {
        juce::StringArray ps;
        for (auto& p : d.params["pitches"]) ps.add(juce::String(pianoroll::pitchName(p.get<int>(), 5)));
        return laneName(d.target) + U("：keyswitch ") + ps.joinIntoString(" ");
    }
    if (d.rule == "octaveShift") {
        const int k = d.params["shift"].get<int>();
        return laneName(d.target) + (k > 0 ? U("：整体升 ") : U("：整体降 ")) + juce::String(std::abs(k) / 12) + U(" 个八度");
    }
    return toJuce(d.id);
}

// ---------------------------------------------------------------------------
// 右键菜单（整理模式）

void SidePanel::showLaneMenu(int lane) {
    if (!doc.hasFile() || lane < 0 || lane >= (int)doc.projection().lanes.size()) return;
    const auto& pl = doc.projection().lanes[(size_t)lane];
    const auto laneId = toJuce(pl.id);
    juce::PopupMenu m;
    juce::StringArray selectedIds;
    for (int i = 0; i < lanes->getNumSelectedRows(); ++i) {
        const int r = lanes->getSelectedRow(i);
        if (r >= 0 && r < (int)doc.projection().lanes.size() && !doc.projection().lanes[(size_t)r].unassigned())
            selectedIds.add(toJuce(doc.projection().lanes[(size_t)r].id));
    }
    if (pl.unassigned()) {
        juce::PopupMenu into;
        for (size_t i = 0; i < doc.projection().lanes.size(); ++i) {
            const auto& o = doc.projection().lanes[i];
            if (o.unassigned()) continue;
            into.addItem(toJuce(o.info.name), [this, sid = toJuce(pl.sources.front()), to = toJuce(o.id)] {
                runAndReport(doc.moveStream(sid, to));
            });
        }
        m.addSubMenu(U("收进声部"), into);
    } else {
        m.addItem(U("改名…"), [this, lane] { askRename(lane); });
        // 指定乐器：按组分子菜单
        juce::PopupMenu instMenu;
        const auto& spec = doc.sidecar().lanes[(size_t)lane];
        instMenu.addItem(U("不指定（当纯 MIDI 看）"), true, spec.instrument.empty(),
                         [this, laneId] { runAndReport(doc.setInstrument(laneId, {})); });
        for (auto& [key, gname] : core::groupList()) {
            juce::PopupMenu sub;
            for (auto& inst : core::allInstruments()) {
                if (inst.group != key) continue;
                sub.addItem(toJuce(inst.nameZh), true, spec.instrument == inst.id,
                            [this, laneId, id = toJuce(inst.id)] { runAndReport(doc.setInstrument(laneId, id)); });
            }
            if (sub.getNumItems() > 0) instMenu.addSubMenu(toJuce(gname), sub);
        }
        m.addSubMenu(U("指定乐器"), instMenu);
        m.addSeparator();
        if (selectedIds.size() >= 2) {
            m.addItem(U("合并选中的 ") + juce::String(selectedIds.size()) + U(" 条声部…"),
                      [this, selectedIds, name = toJuce(doc.projection().lanes[(size_t)lanes->getSelectedRow(0)].info.name)] {
                          askMergeName(selectedIds, name);
                      });
        } else {
            m.addItem(U("合并：先按住 Ctrl 多选几条声部"), false, false, [] {});
        }
        if (pl.sources.size() > 1) {
            juce::PopupMenu split, move;
            for (auto& s : pl.sources) {
                split.addItem(streamLabel(s), [this, sid = toJuce(s)] { runAndReport(doc.splitOut(sid)); });
                juce::PopupMenu to;
                for (size_t i = 0; i < doc.projection().lanes.size(); ++i) {
                    const auto& o = doc.projection().lanes[i];
                    if ((int)i == lane || o.unassigned()) continue;
                    to.addItem(toJuce(o.info.name), [this, sid = toJuce(s), tid = toJuce(o.id)] { runAndReport(doc.moveStream(sid, tid)); });
                }
                move.addSubMenu(streamLabel(s), to);
            }
            m.addSubMenu(U("拆出一个来源，单独成一条"), split);
            m.addSubMenu(U("把一个来源挪到别的声部"), move);
        } else {
            juce::PopupMenu to;
            for (size_t i = 0; i < doc.projection().lanes.size(); ++i) {
                const auto& o = doc.projection().lanes[i];
                if ((int)i == lane || o.unassigned()) continue;
                to.addItem(toJuce(o.info.name), [this, sid = toJuce(pl.sources.front()), tid = toJuce(o.id)] {
                    runAndReport(doc.moveStream(sid, tid));
                });
            }
            m.addSubMenu(U("并入另一条声部"), to);
        }
        m.addSeparator();
        m.addItem(U("上移"), lane > 0, false, [this, laneId, lane] { runAndReport(doc.moveLane(laneId, lane - 1)); });
        m.addItem(U("下移"), lane + 1 < (int)doc.sidecar().lanes.size(), false,
                  [this, laneId, lane] { runAndReport(doc.moveLane(laneId, lane + 1)); });
    }
    m.addSeparator();
    m.addItem(soloLane == lane ? U("取消独奏") : U("独奏这条"), [this, lane] {
        if (onSoloToggled) onSoloToggled(soloLane == lane ? -1 : lane);
    });
    m.addItem(U("细看（完整钢琴窗）"), [this, lane] { if (onExpand) onExpand(lane); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(nullptr));
}

void SidePanel::askRename(int lane) {
    const auto& pl = doc.projection().lanes[(size_t)lane];
    auto* w = new juce::AlertWindow(U("改名"), {}, juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", toJuce(pl.info.name));
    w->addButton(U("确定"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton(U("取消"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, id = toJuce(pl.id)](int r) {
                           if (r == 1) {
                               const auto name = w->getTextEditorContents("name").trim();
                               if (name.isNotEmpty()) runAndReport(doc.rename(id, name));
                           }
                       }),
                       true);
}

void SidePanel::askMergeName(const juce::StringArray& ids, const juce::String& suggestion) {
    auto* w = new juce::AlertWindow(U("合并声部"), U("合并后的声部叫什么？"), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", suggestion);
    w->addButton(U("合并"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton(U("取消"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, ids](int r) {
                           if (r == 1) runAndReport(doc.merge(ids, w->getTextEditorContents("name").trim()));
                       }),
                       true);
}

} // namespace reader
