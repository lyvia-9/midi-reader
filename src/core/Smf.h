// 规范 SMF 读取器：定义 stream 和 sourceNote。规则见 docs/sidecar-format.md 第 1 节，
// 与 proto/reader_proto/smf.py 逐条一致。
#pragma once
#include "core/Model.h"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace reader::core {

struct SmfError : std::runtime_error { using std::runtime_error::runtime_error; };

struct RawNote {
    std::uint32_t sourceNote = 0;
    int chunk = 0, channel = 0, pitch = 0, velocity = 0;
    std::int64_t start = 0, end = 0;
    std::int64_t length() const { return end - start; }
};

struct Stream {
    int chunk = 0, channel = 0;
    std::string trackName;
    int program = -1;            // -1 = 没设过（Python 的 None）
    std::vector<RawNote> notes;  // 按起音顺序
    std::string id() const;
    bool isDrum() const { return channel == 9; }
};

std::string streamId(int chunk, int channel);

struct ChunkInfo {
    int index = 0;
    std::string name;
    std::int64_t endTick = 0;
    std::vector<int> channels;               // 有音符的通道，升序
    std::map<std::string, int> otherEvents;  // 未解释的事件计数
};

struct SmfFile {
    std::string path, sha256;
    int format = 1, ppq = 480;
    std::vector<ChunkInfo> chunks;
    std::vector<Stream> streams;             // 按 (chunk, channel) 排序
    std::vector<TempoChange> tempos;
    std::vector<MeterChange> meters;
    std::int64_t length = 0;
    int orphanNoteOffs = 0, unterminatedNotes = 0, retriggeredNotes = 0;

    std::size_t noteCount() const;
    const Stream* stream(const std::string& id) const;
    std::vector<const Stream*> chunkStreams(int chunk) const;   // 该块有音符的通道，按通道
};

SmfFile parseSmf(const std::string& bytes, const std::string& path = "<bytes>");
SmfFile readSmf(const std::string& path);   // UTF-8 路径
std::string sha256Hex(const std::string& bytes);
std::string readFileBytes(const std::string& utf8Path);

} // namespace reader::core
