// 最小乐器库。表本身由 tools/gen_instruments.py 从 Python 版生成（InstrumentTable.inc）。
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace reader::core {

struct Instrument {
    std::string id, nameZh, group;
    int order = 0;
    int comfortLow = 0, comfortHigh = 127, limitLow = 0, limitHigh = 127;
    std::vector<int> programs;
    bool hasRange = true;
};

const std::vector<Instrument>& allInstruments();
const Instrument* instrumentById(const std::string& id);
std::string groupName(const std::string& groupKey);          // 显示名，如 “弦乐”
std::string groupColour(const std::string& groupKey);
int groupOrder(const std::string& groupKey);
const char* unassignedGroup();                               // “未归类”
std::vector<std::pair<std::string, std::string>> groupList(); // (key, 显示名)，按总谱顺序

std::string normalizeName(const std::string&);
const Instrument* matchByName(const std::string& trackName);
const Instrument* matchByProgram(int program);
// 来源："drumChannel" | "name" | "program" | ""（没认出）
std::pair<const Instrument*, std::string> guessInstrument(const std::string& trackName, int program, bool isDrum);
bool trustedInstrumentSource(const std::string& source);     // user / name / drumChannel
std::pair<int, int> scoreOrderKey(const Instrument*);

} // namespace reader::core
