#include "core/Instruments.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <regex>

namespace reader::core {

namespace {
struct GroupDef { const char* key; const char* name; int order; const char* colour; };
struct InstrumentDef {
    const char* id; const char* nameZh; const char* group; int order;
    int comfortLow, comfortHigh, limitLow, limitHigh;
    std::initializer_list<const char*> keywords;
    std::initializer_list<int> programs;
    bool hasRange;
};
#include "core/InstrumentTable.inc"

struct Compiled {
    Instrument inst;
    std::vector<std::regex> patterns;
};

const std::vector<Compiled>& compiled() {
    static const std::vector<Compiled> table = [] {
        std::vector<Compiled> out;
        for (auto& d : kInstruments) {
            Compiled c;
            c.inst.id = d.id;
            c.inst.nameZh = d.nameZh;
            c.inst.group = d.group;
            c.inst.order = d.order;
            c.inst.comfortLow = d.comfortLow;
            c.inst.comfortHigh = d.comfortHigh;
            c.inst.limitLow = d.limitLow;
            c.inst.limitHigh = d.limitHigh;
            c.inst.programs.assign(d.programs.begin(), d.programs.end());
            c.inst.hasRange = d.hasRange;
            for (auto* k : d.keywords) c.patterns.emplace_back(k, std::regex::ECMAScript | std::regex::optimize);
            out.push_back(std::move(c));
        }
        return out;
    }();
    return table;
}

const GroupDef* findGroup(const std::string& key) {
    for (auto& g : kGroups)
        if (key == g.key) return &g;
    return nullptr;
}
} // namespace

const std::vector<Instrument>& allInstruments() {
    static const std::vector<Instrument> list = [] {
        std::vector<Instrument> out;
        for (auto& c : compiled()) out.push_back(c.inst);
        return out;
    }();
    return list;
}

const Instrument* instrumentById(const std::string& id) {
    for (auto& i : allInstruments())
        if (i.id == id) return &i;
    return nullptr;
}

std::string groupName(const std::string& key) {
    auto* g = findGroup(key);
    return g ? g->name : key;
}
std::string groupColour(const std::string& key) {
    auto* g = findGroup(key);
    return g ? g->colour : "#808a99";
}
int groupOrder(const std::string& key) {
    auto* g = findGroup(key);
    return g ? g->order : 99;
}
const char* unassignedGroup() { return kUnassignedGroup; }

std::vector<std::pair<std::string, std::string>> groupList() {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& g : kGroups) out.emplace_back(g.key, g.name);
    return out;
}

std::string normalizeName(const std::string& name) {
    // Python: lower() → [_\-–—/|·•:()\[\]{}]+ 换成空格 → 合并空白 → strip
    static const char* const multi[] = {"\xE2\x80\x93", "\xE2\x80\x94", "\xC2\xB7", "\xE2\x80\xA2"};  // – — · •
    std::string s;
    for (std::size_t i = 0; i < name.size();) {
        bool replaced = false;
        for (auto* m : multi) {
            const std::string ms(m);
            if (name.compare(i, ms.size(), ms) == 0) {
                s += ' ';
                i += ms.size();
                replaced = true;
                break;
            }
        }
        if (replaced) continue;
        char c = name[i++];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        switch (c) {
            case '_': case '-': case '/': case '|': case ':': case '(': case ')':
            case '[': case ']': case '{': case '}': s += ' '; break;
            default: s += c;
        }
    }
    std::string out;
    bool space = false;
    for (char c : s) {
        const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        if (ws) { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

const Instrument* matchByName(const std::string& trackName) {
    const auto s = normalizeName(trackName);
    if (s.empty()) return nullptr;
    for (auto& c : compiled())
        for (auto& p : c.patterns)
            if (std::regex_search(s, p)) return instrumentById(c.inst.id);
    return nullptr;
}

const Instrument* matchByProgram(int program) {
    if (program <= 0) return nullptr;   // -1 = 没设；0 是默认值，不当证据
    for (auto& i : allInstruments())
        for (int p : i.programs)
            if (p == program) return &i;
    return nullptr;
}

std::pair<const Instrument*, std::string> guessInstrument(const std::string& trackName, int program, bool isDrum) {
    if (isDrum) return {instrumentById("drums"), "drumChannel"};
    if (auto* i = matchByName(trackName)) return {i, "name"};
    if (auto* i = matchByProgram(program)) return {i, "program"};
    return {nullptr, ""};
}

bool trustedInstrumentSource(const std::string& s) {
    return s == "user" || s == "name" || s == "drumChannel";
}

std::pair<int, int> scoreOrderKey(const Instrument* inst) {
    if (!inst) return {groupOrder("other"), 0};
    return {groupOrder(inst->group), inst->order};
}

} // namespace reader::core
