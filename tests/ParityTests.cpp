// C++ 核心与 Python 原型的对照：testdata/cases 里每个样例，C++ 跑出来的 sidecar 必须与
// expected.reader.json 逐字节相同，投影必须与 expected.lanes.json 内容相同。
// 用法：ParityTests <testdata/cases 目录>
#include "core/Analyze.h"
#include "core/Instruments.h"
#include "core/Project.h"
#include "core/Sidecar.h"
#include "core/Smf.h"
#include "core/TempoMap.h"

#include <cmath>
#include <filesystem>
#include <iostream>

using namespace reader::core;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ++failures;                                                                  \
            std::cerr << "FAIL " << msg << "  (" #cond ")\n";                            \
        }                                                                                \
    } while (0)

static std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static void firstDiff(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const auto from = i > 200 ? i - 200 : 0;
    std::cerr << "  --- expected around byte " << i << ":\n" << a.substr(from, 400) << "\n  --- got:\n" << b.substr(from, 400) << "\n";
}

static void runCase(const fs::path& dir) {
    const auto name = u8(dir.filename());
    try {
        const auto smf = readSmf(u8(dir / "input.mid"));
        std::optional<Sidecar> existing;
        if (fs::exists(dir / "input.reader.json")) existing = loadSidecar(u8(dir / "input.reader.json"));
        const auto sc = analyze(smf, existing ? &*existing : nullptr);
        const auto got = dumpSidecar(sc);
        const auto want = readFileBytes(u8(dir / "expected.reader.json"));
        CHECK(got == want, name << ": sidecar differs from Python");
        if (got != want) firstDiff(want, got);

        const auto proj = project(smf, sc);
        const auto wantLanes = Json::parse(readFileBytes(u8(dir / "expected.lanes.json")));
        const auto gotLanes = proj.toJson();
        CHECK(gotLanes == wantLanes, name << ": projection differs from Python");
        if (gotLanes != wantLanes) firstDiff(wantLanes.dump(1), gotLanes.dump(1));

        // 读回自己写的 sidecar 再分析一遍，结果不变（用户重开文件）
        const auto again = analyze(smf, &sc);
        CHECK(dumpSidecar(again) == got, name << ": re-analysis with own sidecar is not stable");
        std::cout << (failures ? "" : "ok   ") << name << "\n";
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "FAIL " << name << ": exception " << e.what() << "\n";
    }
}

static void unitChecks(const fs::path& cases) {
    // tempo map：120 BPM 起，第 2 拍处变 60 BPM
    TempoMap tm({{0, 500000}, {960, 1000000}}, 480);
    CHECK(std::abs(tm.secondsAt(960) - 1.0) < 1e-9, "tempo: 960 ticks at 120bpm = 1s");
    CHECK(std::abs(tm.secondsAt(1440) - 2.0) < 1e-9, "tempo: +480 ticks at 60bpm = +1s");
    CHECK(std::abs(tm.tickAt(2.0) - 1440) < 1e-6, "tempo: inverse");

    CHECK(shadeColour("#d9604c", 0) == "#d9604c", "shade 0");
    CHECK(matchByName("Pizz Strings") && matchByName("Pizz Strings")->id == "string_section", "match Pizz Strings");
    CHECK(matchByName(u8"小提琴 I") && matchByName(u8"小提琴 I")->id == "violin", "match 小提琴");
    CHECK(matchByName("Trombone 1") && matchByName("Trombone 1")->id == "trombone", "match trombone not trumpet");
    CHECK(!matchByName("Pad"), "Pad unknown");

    // parse_edges：GBK 轨名、孤立 note-off 等
    const auto smf = readSmf(u8(cases / "parse_edges" / "input.mid"));
    CHECK(smf.chunks.size() == 3 && smf.chunks[1].name == u8"小提琴", "parse_edges: GBK name");
    CHECK(smf.orphanNoteOffs == 1 && smf.unterminatedNotes == 1 && smf.retriggeredNotes == 1, "parse_edges: counters");

    // 用户操作：确认拆轨后每通道一条、不再标黄
    const auto sp = readSmf(u8(cases / "same_program_channels" / "input.mid"));
    auto sc = analyze(sp);
    sc = applyUserDecision(sp, sc, "split:t1", statusConfirmed);
    CHECK(sc.lanes.size() == 2, "confirm split → 2 lanes");
    const auto pr = project(sp, sc);
    bool anyReview = false;
    for (auto& l : pr.lanes)
        for (auto& n : l.notes) anyReview |= (n.flags & noteFlagReview) != 0;
    CHECK(!anyReview, "confirm split → no review flags");

    // 拆出、挪回、改名
    const auto mm = readSmf(u8(cases / "manual_merge" / "input.mid"));
    auto m = analyze(mm, nullptr);
    const auto before = m.lanes.size();
    m = mergeLanes(mm, m, {m.lanes[0].id, m.lanes[1].id}, u8"小提琴 I");
    CHECK(m.lanes.size() == before - 1, "merge");
    m = splitOutStream(mm, m, m.lanes[0].sources.back());
    CHECK(m.lanes.size() == before, "split out");
    m = moveStream(mm, m, m.lanes[1].sources.front(), m.lanes[0].id);
    CHECK(m.lanes.size() == before - 1, "move stream removes emptied lane");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ParityTests <testdata/cases>\n";
        return 2;
    }
    if (std::string(argv[1]) == "--dump" && argc >= 3) {   // 打印 C++ 对某个 MIDI 的 sidecar 和投影，供模糊对照
        const auto smf = readSmf(argv[2]);
        const auto sc = analyze(smf);
        std::cout << dumpSidecar(sc) << "@@@\n" << project(smf, sc).toJson().dump() << "\n";
        return 0;
    }
    const fs::path cases = fs::u8path(argv[1]);
    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(cases))
        if (fs::exists(e.path() / "input.mid")) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (auto& d : dirs) runCase(d);
    unitChecks(cases);
    std::cout << (failures ? "FAILED: " + std::to_string(failures) : "all " + std::to_string(dirs.size()) + " cases match Python") << "\n";
    return failures ? 1 : 0;
}
