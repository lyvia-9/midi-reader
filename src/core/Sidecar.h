// sidecar v1：映射 + 规则结论 + 逐音修正。格式见 docs/sidecar-format.md。
// 写出的文本与 Python 原型逐字节相同（同样的键顺序和缩进）。
#pragma once
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace reader::core {

using Json = nlohmann::ordered_json;

struct SidecarError : std::runtime_error { using std::runtime_error::runtime_error; };

inline constexpr const char* statusAuto = "auto";
inline constexpr const char* statusReview = "review";
inline constexpr const char* statusConfirmed = "confirmed";
inline constexpr const char* statusRejected = "rejected";

struct Decision {
    std::string id, rule, target, status;
    Json params = Json::object();
    Json evidence = Json::object();
    std::string reason;
    bool applied() const { return status == statusAuto || status == statusConfirmed; }
    bool pending() const { return status == statusReview; }
    bool userSet() const { return status == statusConfirmed || status == statusRejected; }
    Json toJson() const;
    static Decision fromJson(const Json&);
};

struct LaneSpec {
    std::string id, name;
    std::vector<std::string> sources;
    std::string group, colour = "#808080", role = "unknown";
    std::string instrument;         // "" = null
    std::string instrumentSource;   // "" = null
    int comfortLow = 0, comfortHigh = 127, limitLow = 0, limitHigh = 127;
    Json toJson() const;
    static LaneSpec fromJson(const Json&);
};

struct NoteOverride { std::optional<bool> keyswitch; };

struct Sidecar {
    Json source = Json::object();
    Json streams = Json::array();
    std::vector<LaneSpec> lanes;
    std::vector<Decision> decisions;
    std::map<std::uint32_t, NoteOverride> noteOverrides;

    Decision* decision(const std::string& id);
    const Decision* decision(const std::string& id) const;
    LaneSpec* lane(const std::string& id);
    Json toJson() const;
    static Sidecar fromJson(const Json&);
    void validate() const;
};

std::string sidecarPathFor(const std::string& midiPath);   // song.mid → song.mid.reader.json
Sidecar loadSidecar(const std::string& path);
std::string dumpSidecar(const Sidecar&);
void saveSidecar(const Sidecar&, const std::string& path);
bool fileExists(const std::string& utf8Path);
void writeTextFile(const std::string& utf8Path, const std::string& text);

} // namespace reader::core
