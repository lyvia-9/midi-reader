#include "core/Smf.h"
#include "core/TextCodec.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

namespace reader::core {

std::string streamId(int chunk, int channel) {
    return "t" + std::to_string(chunk) + "c" + std::to_string(channel);
}
std::string Stream::id() const { return streamId(chunk, channel); }

std::size_t SmfFile::noteCount() const {
    std::size_t n = 0;
    for (auto& s : streams) n += s.notes.size();
    return n;
}

const Stream* SmfFile::stream(const std::string& id) const {
    for (auto& s : streams)
        if (s.id() == id) return &s;
    return nullptr;
}

std::vector<const Stream*> SmfFile::chunkStreams(int chunk) const {
    std::vector<const Stream*> out;
    for (auto& s : streams)
        if (s.chunk == chunk) out.push_back(&s);
    return out;
}

// ---------------------------------------------------------------------------
// SHA-256（sidecar 用它认出原文件）

namespace {
struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t)p[i * 4] << 24 | (std::uint32_t)p[i * 4 + 1] << 16 | (std::uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const auto S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = hh + S1 + ch + k[i] + w[i];
            const auto S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const auto mj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};
} // namespace

std::string sha256Hex(const std::string& bytes) {
    Sha256 s;
    std::string msg = bytes;
    const std::uint64_t bits = (std::uint64_t)bytes.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 7; i >= 0; --i) msg += (char)((bits >> (i * 8)) & 0xFF);
    for (std::size_t i = 0; i < msg.size(); i += 64) s.block(reinterpret_cast<const unsigned char*>(msg.data() + i));
    char buf[65];
    for (int i = 0; i < 8; ++i) std::snprintf(buf + i * 8, 9, "%08x", s.h[i]);
    return std::string(buf, 64);
}

std::string readFileBytes(const std::string& utf8Path) {
#if defined(__cpp_char8_t)
    const auto p = std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
#else
    const auto p = std::filesystem::u8path(utf8Path);
#endif
    std::ifstream in(p, std::ios::binary);
    if (!in) throw SmfError("cannot open " + utf8Path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------

namespace {
const unsigned char* U(const std::string& s) { return reinterpret_cast<const unsigned char*>(s.data()); }

std::uint32_t be(const std::string& d, std::size_t pos, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | U(d)[pos + i];
    return v;
}

std::int64_t vlq(const std::string& d, std::size_t& pos, std::size_t end) {
    std::int64_t value = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= end) throw SmfError("truncated variable-length quantity");
        const auto b = U(d)[pos++];
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) return value;
    }
    throw SmfError("variable-length quantity longer than 4 bytes");
}

int programFor(const std::vector<std::pair<std::int64_t, int>>& changes, std::int64_t firstNote) {
    if (changes.empty()) return -1;
    int current = -1;
    for (auto& [t, prog] : changes)
        if (t <= firstNote) current = prog;
    return current >= 0 ? current : changes.front().second;
}

template<class T, class Value>
std::vector<T> dedupe(std::vector<T> items, Value value) {
    // 同 tick 保留最后一条（稳定排序后），再去掉与前一条相同值的
    std::stable_sort(items.begin(), items.end(), [](const T& a, const T& b) { return a.tick < b.tick; });
    std::vector<T> byTick;
    for (auto& it : items) {
        if (!byTick.empty() && byTick.back().tick == it.tick) byTick.back() = it;
        else byTick.push_back(it);
    }
    std::vector<T> out;
    for (auto& it : byTick) {
        if (!out.empty() && value(out.back()) == value(it)) continue;
        out.push_back(it);
    }
    return out;
}

const char* otherName(int kind) {
    switch (kind) {
        case 0xA0: return "polyAftertouch";
        case 0xB0: return "cc";
        case 0xD0: return "channelAftertouch";
        case 0xE0: return "pitchBend";
        default: return "other";
    }
}
} // namespace

SmfFile parseSmf(const std::string& input, const std::string& path) {
    std::string data = input;
    if (data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "RMID") == 0) {
        const auto idx = data.find("MThd");
        if (idx == std::string::npos) throw SmfError("RMID without MThd");
        data = data.substr(idx);
    }
    if (data.size() < 14 || data.compare(0, 4, "MThd") != 0) throw SmfError("not a Standard MIDI File (missing MThd)");
    const auto hlen = be(data, 4, 4);
    if (hlen < 6) throw SmfError("MThd too short");
    SmfFile f;
    f.path = path;
    f.sha256 = sha256Hex(data);
    f.format = (int)be(data, 8, 2);
    const auto division = be(data, 12, 2);
    if (division & 0x8000) throw SmfError("SMPTE time division is not supported");
    f.ppq = (int)division;

    std::map<std::pair<int, int>, Stream> streams;
    std::map<std::pair<int, int>, std::vector<std::pair<std::int64_t, int>>> programs;
    std::vector<TempoChange> tempos;
    std::vector<MeterChange> meters;
    std::uint32_t nextId = 0;
    static const int dataLen[8] = {2, 2, 2, 2, 1, 1, 2, 0};

    std::size_t pos = 8 + hlen;
    while (pos + 8 <= data.size()) {
        const auto ctype = data.substr(pos, 4);
        const std::size_t clen = be(data, pos + 4, 4);
        const std::size_t bodyStart = pos + 8;
        const std::size_t bodyEnd = std::min(bodyStart + clen, data.size());
        pos = bodyStart + clen;
        if (ctype != "MTrk") continue;
        const int ci = (int)f.chunks.size();
        bool hasName = false;
        std::string name;
        std::map<std::string, int> other;
        std::map<std::pair<int, int>, std::size_t> openIndex;
        std::int64_t tick = 0;
        int status = 0;
        std::size_t p = bodyStart;
        auto noteRef = [&](int ch, std::size_t idx) -> RawNote& { return streams[{ci, ch}].notes[idx]; };
        try {
            while (p < bodyEnd) {
                tick += vlq(data, p, bodyEnd);
                if (p >= bodyEnd) break;
                const int b = U(data)[p];
                if (b >= 0x80) {
                    ++p;
                    if (b < 0xF0) {
                        status = b;
                    } else if (b == 0xFF) {
                        if (p >= bodyEnd) break;
                        const int mtype = U(data)[p];
                        ++p;
                        const auto mlen = (std::size_t)vlq(data, p, bodyEnd);
                        const auto mdata = p < data.size() ? data.substr(p, mlen) : std::string();   // 与 Python 切片一致
                        p += mlen;
                        if (mtype == 0x2F) break;
                        if (mtype == 0x03 && !hasName) {
                            hasName = true;
                            name = decodeText(mdata);
                        } else if (mtype == 0x51 && mdata.size() >= 3) {
                            tempos.push_back({tick, (int)be(mdata, 0, 3)});
                        } else if (mtype == 0x58 && mdata.size() >= 2) {
                            meters.push_back({tick, U(mdata)[0], 1 << U(mdata)[1]});
                        } else {
                            char buf[16];
                            std::snprintf(buf, sizeof buf, "meta:%02x", mtype);
                            ++other[buf];
                        }
                        continue;
                    } else if (b == 0xF0 || b == 0xF7) {
                        const auto slen = (std::size_t)vlq(data, p, bodyEnd);
                        p += slen;
                        ++other["sysex"];
                        continue;
                    } else {
                        char buf[16];
                        std::snprintf(buf, sizeof buf, "system:%02x", b);
                        ++other[buf];
                        continue;
                    }
                } else if (status == 0) {
                    throw SmfError("data byte without status in track " + std::to_string(ci));
                }
                const int kind = status & 0xF0, ch = status & 0x0F;
                const int n = dataLen[(kind >> 4) - 8];
                if (p + n > bodyEnd) break;
                const int d1 = U(data)[p];
                const int d2 = n == 2 ? U(data)[p + 1] : 0;
                p += n;
                const std::pair<int, int> key{ch, d1};
                if (kind == 0x90 && d2 > 0) {
                    if (auto it = openIndex.find(key); it != openIndex.end()) {
                        noteRef(ch, it->second).end = tick;
                        openIndex.erase(it);
                        ++f.retriggeredNotes;
                    }
                    auto& st = streams[{ci, ch}];
                    st.chunk = ci;
                    st.channel = ch;
                    st.notes.push_back({nextId++, ci, ch, d1, d2, tick, tick});
                    openIndex[key] = st.notes.size() - 1;
                } else if (kind == 0x80 || kind == 0x90) {
                    if (auto it = openIndex.find(key); it != openIndex.end()) {
                        noteRef(ch, it->second).end = tick;
                        openIndex.erase(it);
                    } else {
                        ++f.orphanNoteOffs;
                    }
                } else if (kind == 0xC0) {
                    programs[{ci, ch}].push_back({tick, d1});
                } else {
                    ++other[otherName(kind)];
                }
            }
        } catch (const SmfError&) {
            bool any = !streams.empty();
            if (ci == 0 && !any) throw;
            ++other["truncated"];
        }
        for (auto& [key, idx] : openIndex) {
            noteRef(key.first, idx).end = tick;
            ++f.unterminatedNotes;
        }
        ChunkInfo info;
        info.index = ci;
        info.name = name;
        info.endTick = tick;
        info.otherEvents = other;
        for (auto& [k, st] : streams)
            if (k.first == ci) info.channels.push_back(k.second);
        f.chunks.push_back(std::move(info));
    }

    for (auto& [k, st] : streams) {
        st.trackName = f.chunks[(std::size_t)k.first].name;
        auto it = programs.find(k);
        st.program = programFor(it == programs.end() ? std::vector<std::pair<std::int64_t, int>>{} : it->second,
                                st.notes.front().start);
        f.streams.push_back(st);
    }
    f.tempos = dedupe(tempos, [](const TempoChange& t) { return t.microsPerQuarter; });
    f.meters = dedupe(meters, [](const MeterChange& m) { return std::make_pair(m.numerator, m.denominator); });
    for (auto& c : f.chunks) f.length = std::max(f.length, c.endTick);
    return f;
}

SmfFile readSmf(const std::string& path) {
    return parseSmf(readFileBytes(path), path);
}

} // namespace reader::core
