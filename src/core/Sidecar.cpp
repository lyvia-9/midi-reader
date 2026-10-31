#include "core/Sidecar.h"
#include "core/Smf.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace reader::core {

namespace {
std::filesystem::path fsPath(const std::string& utf8) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}
Json nullable(const std::string& s) { return s.empty() ? Json(nullptr) : Json(s); }
std::string fromNullable(const Json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return {};
    return j[key].get<std::string>();
}
} // namespace

Json Decision::toJson() const {
    Json j;
    j["id"] = id;
    j["rule"] = rule;
    j["target"] = target;
    j["status"] = status;
    j["params"] = params;
    j["evidence"] = evidence;
    j["reason"] = reason;
    return j;
}

Decision Decision::fromJson(const Json& j) {
    Decision d;
    d.id = j.at("id").get<std::string>();
    d.rule = j.at("rule").get<std::string>();
    d.target = j.at("target").get<std::string>();
    d.status = j.at("status").get<std::string>();
    static const std::set<std::string> ok{statusAuto, statusReview, statusConfirmed, statusRejected};
    if (!ok.count(d.status)) throw SidecarError("decision " + d.id + ": bad status " + d.status);
    if (j.contains("params")) d.params = j["params"];
    if (j.contains("evidence")) d.evidence = j["evidence"];
    if (j.contains("reason")) d.reason = j["reason"].get<std::string>();
    return d;
}

Json LaneSpec::toJson() const {
    Json j;
    j["id"] = id;
    j["name"] = name;
    j["group"] = group;
    j["colour"] = colour;
    j["role"] = role;
    j["instrument"] = nullable(instrument);
    j["instrumentSource"] = nullable(instrumentSource);
    Json r;
    r["comfortLow"] = comfortLow;
    r["comfortHigh"] = comfortHigh;
    r["limitLow"] = limitLow;
    r["limitHigh"] = limitHigh;
    j["range"] = r;
    j["sources"] = sources;
    return j;
}

LaneSpec LaneSpec::fromJson(const Json& j) {
    LaneSpec l;
    l.id = j.at("id").get<std::string>();
    l.name = j.at("name").get<std::string>();
    l.sources = j.at("sources").get<std::vector<std::string>>();
    l.group = j.value("group", std::string());
    l.colour = j.value("colour", std::string("#808080"));
    l.role = j.value("role", std::string("unknown"));
    l.instrument = fromNullable(j, "instrument");
    l.instrumentSource = fromNullable(j, "instrumentSource");
    const Json r = j.value("range", Json::object());
    l.comfortLow = r.value("comfortLow", 0);
    l.comfortHigh = r.value("comfortHigh", 127);
    l.limitLow = r.value("limitLow", 0);
    l.limitHigh = r.value("limitHigh", 127);
    return l;
}

Decision* Sidecar::decision(const std::string& id) {
    for (auto& d : decisions)
        if (d.id == id) return &d;
    return nullptr;
}
const Decision* Sidecar::decision(const std::string& id) const {
    for (auto& d : decisions)
        if (d.id == id) return &d;
    return nullptr;
}
LaneSpec* Sidecar::lane(const std::string& id) {
    for (auto& l : lanes)
        if (l.id == id) return &l;
    return nullptr;
}

Json Sidecar::toJson() const {
    Json j;
    j["format"] = "midi-reader-sidecar";
    j["version"] = 1;
    j["source"] = source;
    j["streams"] = streams;
    j["lanes"] = Json::array();
    for (auto& l : lanes) j["lanes"].push_back(l.toJson());
    j["decisions"] = Json::array();
    for (auto& d : decisions) j["decisions"].push_back(d.toJson());
    Json o = Json::object();
    for (auto& [k, v] : noteOverrides) {
        Json e = Json::object();
        if (v.keyswitch) e["keyswitch"] = *v.keyswitch;
        o[std::to_string(k)] = e;
    }
    j["noteOverrides"] = o;
    return j;
}

Sidecar Sidecar::fromJson(const Json& j) {
    if (j.value("format", std::string()) != "midi-reader-sidecar") throw SidecarError("not a midi-reader sidecar");
    if (j.value("version", 0) != 1) throw SidecarError("unsupported sidecar version");
    Sidecar sc;
    sc.source = j.value("source", Json::object());
    sc.streams = j.value("streams", Json::array());
    for (auto& l : j.value("lanes", Json::array())) sc.lanes.push_back(LaneSpec::fromJson(l));
    for (auto& d : j.value("decisions", Json::array())) sc.decisions.push_back(Decision::fromJson(d));
    const Json overrides = j.value("noteOverrides", Json::object());
    for (auto& [k, v] : overrides.items()) {
        NoteOverride o;
        if (v.contains("keyswitch") && v["keyswitch"].is_boolean()) o.keyswitch = v["keyswitch"].get<bool>();
        sc.noteOverrides[(std::uint32_t)std::stoul(k)] = o;
    }
    sc.validate();
    return sc;
}

void Sidecar::validate() const {
    std::set<std::string> laneIds;
    std::map<std::string, std::string> owner;
    for (auto& l : lanes) {
        if (!laneIds.insert(l.id).second) throw SidecarError("duplicate lane id " + l.id);
        for (auto& s : l.sources) {
            if (owner.count(s)) throw SidecarError("stream " + s + " is in both " + owner[s] + " and " + l.id);
            owner[s] = l.id;
        }
    }
    std::set<std::string> ids;
    for (auto& d : decisions)
        if (!ids.insert(d.id).second) throw SidecarError("duplicate decision id");
}

std::string sidecarPathFor(const std::string& midiPath) { return midiPath + ".reader.json"; }

bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(fsPath(p), ec);
}

Sidecar loadSidecar(const std::string& path) {
    const auto text = readFileBytes(path);
    Json j;
    try {
        j = Json::parse(text);
    } catch (const std::exception& e) {
        throw SidecarError(std::string("sidecar is not valid JSON: ") + e.what());
    }
    return Sidecar::fromJson(j);
}

std::string dumpSidecar(const Sidecar& sc) {
    sc.validate();
    return sc.toJson().dump(2) + "\n";
}

void writeTextFile(const std::string& path, const std::string& text) {
    // 先写临时文件再替换，避免写一半崩溃留下坏 sidecar
    const auto target = fsPath(path);
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw SidecarError("cannot write " + path);
        out << text;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        std::filesystem::rename(tmp, target, ec);
        if (ec) throw SidecarError("cannot replace " + path);
    }
}

void saveSidecar(const Sidecar& sc, const std::string& path) { writeTextFile(path, dumpSidecar(sc)); }

} // namespace reader::core
