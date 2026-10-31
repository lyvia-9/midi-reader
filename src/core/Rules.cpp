#include "core/Rules.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace reader::core::rules {

namespace {
std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
    return out;
}
std::string S(long long v) { return std::to_string(v); }
Json nullable(const std::string& s) { return s.empty() ? Json(nullptr) : Json(s); }
bool atLeast(long long num, long long den, int a, int b) { return num * b >= den * a; }
bool atMost(long long num, long long den, int a, int b) { return num * b <= den * a; }
} // namespace

// ---------------------------------------------------------------------------
// 规则一：按通道拆轨

std::vector<Decision> channelSplit(const SmfFile& smf) {
    std::vector<Decision> out;
    for (auto& chunk : smf.chunks) {
        if (chunk.channels.size() < 2) continue;
        const auto streams = smf.chunkStreams(chunk.index);
        std::vector<std::string> idents;
        for (auto* s : streams)
            idents.push_back(s->isDrum() ? "drum" : (s->program >= 0 ? "prog" + S(s->program) : "prog?"));
        const std::set<std::string> uniq(idents.begin(), idents.end());
        const bool distinct = uniq.size() == idents.size() && !uniq.count("prog?");
        const bool type0 = smf.format == 0;
        Decision d;
        d.id = "split:t" + S(chunk.index);
        d.rule = "channelSplit";
        d.target = "t" + S(chunk.index);
        d.status = (type0 || distinct) ? statusAuto : statusReview;
        Json ids = Json::array(), chans = Json::array(), progs = Json::array(), counts = Json::array();
        for (auto* s : streams) {
            ids.push_back(s->id());
            chans.push_back(s->channel + 1);
            progs.push_back(s->program >= 0 ? Json(s->program) : Json(nullptr));
            counts.push_back(s->notes.size());
        }
        d.params["streams"] = ids;
        d.evidence["fileFormat"] = smf.format;
        d.evidence["channels"] = chans;
        d.evidence["programs"] = progs;
        d.evidence["noteCounts"] = counts;
        if (type0) d.reason = "type 0 文件，所有乐器挤在一条轨里，按通道拆开";
        else if (distinct) d.reason = "同一轨里各通道音色不同，按通道拆开";
        else d.reason = "同一轨里有通道音色相同或未设音色，可能是同一乐器分通道，暂不拆";
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 规则二：识别 keyswitch

namespace ks {
constexpr int minStreamNotes = 4, maxClusterPitches = 12, gapCandidate = 5, gapStrong = 7, gapInstrument = 2;
constexpr int lowExtreme = 23, highExtreme = 108, leadWindowQuarters = 4, tolDivisor = 8;
constexpr int minNotesNotExtreme = 3, gapInsideInstrument = 12;

struct Cluster { std::vector<int> pitches; int gap = 0; };

std::optional<Cluster> byGap(const std::vector<int>& pitches, const std::map<int, int>& counts, int total) {
    std::optional<Cluster> best;
    long long running = 0;
    for (std::size_t i = 0; i + 1 < pitches.size(); ++i) {
        running += counts.at(pitches[i]);
        if ((int)i + 1 > maxClusterPitches || running * 2 >= total) break;
        const int gap = pitches[i + 1] - pitches[i];
        if (gap >= gapCandidate && (!best || gap > best->gap))
            best = Cluster{std::vector<int>(pitches.begin(), pitches.begin() + (long)i + 1), gap};
    }
    return best;
}

std::optional<Cluster> byInstrument(const std::vector<int>& pitches, const std::map<int, int>& counts, int total, int bound) {
    std::vector<int> lower;
    for (int p : pitches)
        if (p < bound) lower.push_back(p);
    if (lower.empty() || lower.size() == pitches.size() || (int)lower.size() > maxClusterPitches) return std::nullopt;
    long long n = 0;
    for (int p : lower) n += counts.at(p);
    if (n * 4 > (long long)total * 1) return std::nullopt;
    const int gap = pitches[lower.size()] - lower.back();
    if (gap < gapInstrument) return std::nullopt;
    return Cluster{lower, gap};
}

bool doubles(const RawNote& k, const RawNote& q, long long tol) {
    if (k.pitch % 12 != q.pitch % 12) return false;
    const long long endTol = std::max<long long>(tol, std::max(k.length(), q.length()) / 4);
    return std::llabs(k.start - q.start) <= tol && std::llabs(k.end - q.end) <= endTol;
}

std::optional<Decision> side(const Stream& stream, const std::vector<RawNote>& notes, int ppq, const Instrument* inst,
                             const std::string& instSource, int shift, bool mirror) {
    auto key = [mirror](int p) { return mirror ? 127 - p : p; };
    std::map<int, int> counts;
    for (auto& n : notes) ++counts[key(n.pitch)];
    std::vector<int> pitches;
    for (auto& [p, c] : counts) pitches.push_back(p);
    const int total = (int)notes.size();
    const bool hasRange = inst && inst->hasRange;
    std::optional<int> bound;
    if (hasRange) bound = mirror ? 127 - (inst->limitHigh - shift) : inst->limitLow - shift;

    auto found = byGap(pitches, counts, total);
    std::string via = "gap";
    if (!found && bound) {
        found = byInstrument(pitches, counts, total, *bound);
        via = "instrument";
    }
    if (!found) return std::nullopt;
    const auto& cluster = found->pitches;
    const int gap = found->gap;
    const std::set<int> cset(cluster.begin(), cluster.end());
    std::vector<RawNote> ksn, play;
    for (auto& n : notes) (cset.count(key(n.pitch)) ? ksn : play).push_back(n);

    const long long tol = std::max(1, ppq / tolDivisor);
    std::vector<long long> starts;
    for (auto& n : play) starts.push_back(n.start);
    auto lowerBound = [](const std::vector<long long>& v, long long x) { return (std::size_t)(std::lower_bound(v.begin(), v.end(), x) - v.begin()); };
    auto upperBound = [](const std::vector<long long>& v, long long x) { return (std::size_t)(std::upper_bound(v.begin(), v.end(), x) - v.begin()); };

    int lead = 0;
    for (auto& k : ksn) {
        const auto i = lowerBound(starts, k.start - tol);
        if (i < starts.size() && starts[i] <= k.start + (long long)leadWindowQuarters * ppq) ++lead;
    }
    int doubled = 0;
    for (auto& k : ksn) {
        const auto lo = lowerBound(starts, k.start - tol), hi = upperBound(starts, k.start + tol);
        for (auto j = lo; j < hi; ++j)
            if (doubles(k, play[j], tol)) { ++doubled; break; }
    }
    std::vector<std::pair<long long, long long>> ends;
    std::vector<long long> allStarts;
    for (auto& n : notes) { ends.push_back({n.end, n.start}); allStarts.push_back(n.start); }
    std::sort(ends.begin(), ends.end());
    int inlineCount = 0;
    for (auto& k : ksn) {
        const auto lo = lowerBound(starts, k.start - tol);
        const bool startsWith = lo < starts.size() && starts[lo] <= k.start + tol;
        auto e = (std::size_t)(std::lower_bound(ends.begin(), ends.end(), std::make_pair(k.start - tol, -1LL)) - ends.begin());
        bool endsAt = false;
        for (; e < ends.size() && ends[e].first <= k.start + tol; ++e)
            if (ends[e].second < k.start) { endsAt = true; break; }
        auto a = lowerBound(allStarts, k.end - tol);
        bool nextAt = false;
        for (; a < allStarts.size() && allStarts[a] <= k.end + tol; ++a)
            if (allStarts[a] > k.start) { nextAt = true; break; }
        if (endsAt && nextAt && !startsWith) ++inlineCount;
    }

    std::vector<int> real;
    for (int c : cluster) real.push_back(mirror ? 127 - c : c);
    std::sort(real.begin(), real.end());
    const bool extreme = std::all_of(real.begin(), real.end(), [&](int p) { return mirror ? p >= highExtreme : p <= lowExtreme; });
    const bool outsideInst = bound && std::all_of(cluster.begin(), cluster.end(), [&](int c) { return c < *bound; });
    const bool trusted = trustedInstrumentSource(instSource);
    const bool position = extreme || (outsideInst && trusted);
    const int n = (int)ksn.size();
    const bool leadOk = atLeast(lead, n, 3, 5);
    const bool doubledOk = atMost(doubled, n, 1, 5);
    const bool inlineOk = atMost(inlineCount, n, 1, 5);
    const bool gapOk = gap >= gapStrong || (outsideInst && trusted);

    if (!extreme) {
        if (n < minNotesNotExtreme) return std::nullopt;
        if (hasRange && !outsideInst && gap < gapInsideInstrument) return std::nullopt;
    }
    std::string status;
    if (position && gapOk && leadOk && doubledOk && inlineOk)
        status = statusAuto;
    else if ((gap >= gapStrong || outsideInst || extreme) && atMost(doubled, n, 1, 2) && atMost(inlineCount, n, 1, 2)
             && atLeast(lead, n, 1, 2))
        status = statusReview;
    else
        return std::nullopt;

    std::vector<std::string> why;
    if (extreme) why.push_back("音高在极端音区");
    if (outsideInst) why.push_back("在" + inst->nameZh + "演奏极限之外");
    why.push_back("与正常音相隔 " + S(gap) + " 个半音");
    why.push_back(S(lead) + "/" + S(n) + " 个后面紧跟正常音");
    if (doubled) why.push_back(S(doubled) + "/" + S(n) + " 个与正常音八度叠加");
    if (inlineCount) why.push_back(S(inlineCount) + "/" + S(n) + " 个接在旋律线上");
    if (status == statusReview) {
        std::vector<std::string> missing;
        if (!position) missing.push_back(std::string("位置不够极端") + (outsideInst && !trusted ? "（乐器只从音色号猜出，不作数）" : ""));
        if (!gapOk) missing.push_back("空档不够大");
        if (!leadOk) missing.push_back("和后面的音符关系不明显");
        if (!doubledOk) missing.push_back("八度叠加偏多");
        if (!inlineOk) missing.push_back("像是旋律里的音");
        why.push_back("没把握：" + join(missing, "、"));
    }

    const std::string sideName = mirror ? "high" : "low";
    Decision d;
    d.id = "ks:" + stream.id() + ":" + sideName;
    d.rule = "keySwitch";
    d.target = stream.id();
    d.status = status;
    d.params["side"] = sideName;
    d.params["pitches"] = real;
    d.evidence["via"] = via;
    d.evidence["gap"] = gap;
    d.evidence["notes"] = n;
    d.evidence["lead"] = lead;
    d.evidence["doubled"] = doubled;
    d.evidence["inline"] = inlineCount;
    d.evidence["extreme"] = extreme;
    d.evidence["outsideInstrument"] = (bool)outsideInst;
    d.evidence["instrument"] = inst ? Json(inst->id) : Json(nullptr);
    d.evidence["instrumentSource"] = nullable(instSource);
    d.reason = join(why, "；");
    return d;
}
} // namespace ks

std::vector<Decision> keySwitch(const Stream& stream, int ppq, const Instrument* inst, const std::string& instSource,
                                int octaveShift, const std::map<std::uint32_t, bool>& forced) {
    if (stream.isDrum()) return {};
    std::vector<RawNote> notes;
    for (auto& n : stream.notes)
        if (!forced.count(n.sourceNote)) notes.push_back(n);
    if ((int)notes.size() < ks::minStreamNotes) return {};
    std::vector<Decision> out;
    for (bool mirror : {false, true})
        if (auto d = ks::side(stream, notes, ppq, inst, instSource, octaveShift, mirror)) out.push_back(*d);
    return out;
}

// ---------------------------------------------------------------------------
// 规则三：找音源八度偏移

std::optional<Decision> octaveShift(const Stream& stream, const Instrument* inst, const std::string& instSource,
                                    const std::vector<RawNote>& playing) {
    if (!inst || !inst->hasRange || stream.isDrum()) return std::nullopt;
    if (playing.size() < 8) return std::nullopt;
    const long long n = (long long)playing.size();
    struct Row { long long hard, absK, soft; int k; };
    std::vector<Row> table;
    for (int k : {-24, -12, 0, 12, 24}) {
        long long hard = 0, soft = 0;
        for (auto& note : playing) {
            const int p = note.pitch + k;
            if (p < inst->limitLow || p > inst->limitHigh) ++hard;
            if (p < inst->comfortLow || p > inst->comfortHigh) ++soft;
        }
        table.push_back({hard, std::abs(k), soft, k});
    }
    const auto best = *std::min_element(table.begin(), table.end(), [](const Row& a, const Row& b) {
        return std::tie(a.hard, a.absK, a.soft, a.k) < std::tie(b.hard, b.absK, b.soft, b.k);
    });
    const Row zero = table[2];
    const int k = best.k;
    if (k == 0) return std::nullopt;
    if (zero.hard < 2 || zero.hard * 10 < n * 1) return std::nullopt;
    if (best.hard * 20 > n * 1) return std::nullopt;
    if (best.soft >= zero.soft) return std::nullopt;
    const bool trusted = trustedInstrumentSource(instSource);
    const bool strong = zero.hard * 5 >= n * 1;
    const bool autoOk = trusted && best.hard == 0 && strong && best.soft * 2 <= zero.soft;

    Decision d;
    d.id = "oct:" + stream.id();
    d.rule = "octaveShift";
    d.target = stream.id();
    d.status = autoOk ? statusAuto : statusReview;
    d.params["shift"] = k;
    d.evidence["instrument"] = inst->id;
    d.evidence["instrumentSource"] = nullable(instSource);
    d.evidence["notes"] = n;
    Json hardBy = Json::object(), softBy = Json::object();
    for (auto& r : table) { hardBy[S(r.k)] = r.hard; softBy[S(r.k)] = r.soft; }
    d.evidence["outsideLimitByShift"] = hardBy;
    d.evidence["outsideComfortByShift"] = softBy;
    d.reason = "按" + inst->nameZh + "的音域，原位有 " + S(zero.hard) + "/" + S(n) + " 个音超出演奏极限，整体"
               + (k > 0 ? "升" : "降") + " " + S(std::abs(k) / 12) + " 个八度后剩 " + S(best.hard) + " 个";
    if (!autoOk) {
        std::vector<std::string> missing;
        if (!trusted) missing.push_back("乐器只从音色号猜出");
        if (best.hard) missing.push_back("平移后仍有超极限的音");
        if (!strong) missing.push_back("出界的音不到两成");
        if (best.soft * 2 > zero.soft) missing.push_back("平移后超舒适音域的音没减少一半");
        d.reason += "；没把握：" + join(missing, "、");
    }
    return d;
}

} // namespace reader::core::rules
