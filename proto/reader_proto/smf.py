"""规范 SMF 读取器：定义“原始音符”和 sourceNote 编号。

为什么不用 symusic 读：symusic 载入时按 (轨, 通道, 音色) 重新分组，丢掉了原始
MTrk 序号和通道号，于是“按通道拆轨”无从判断，sourceNote 也没法和 C++ 那边
（JUCE MidiFile 保留每个 MTrk 块和通道）对上。这里按文件字节顺序读，规则写死，
C++ 照着同一套规则实现即可逐条对齐。symusic 只用来写导出文件和做交叉校验。

规则（docs/sidecar-format.md 里有同样的文字版）：
- 只计 "MTrk" 块，块序号 chunk 从 0 起；其他块类型跳过。
- sourceNote = 全文件按 (块序, 块内事件序) 遍历时，第几个 velocity>0 的 note-on，从 0 起。
- note-off 或 velocity=0 的 note-on 结束同 (通道, 音高) 上正在响的音。
- 同 (通道, 音高) 还在响又来一个 note-on：先在此 tick 结束旧音，再开新音（与 JUCE
  MidiMessageSequence::updateMatchedPairs 的处理一致）。
- 找不到对应 note-on 的 note-off：丢弃并计入 orphanNoteOffs。
- 块结束时还没关的音：结束于该块最后一个事件的 tick，计入 unterminatedNotes。
- running status 在 meta/sysex 之后仍保留（宽松读法）。
"""
from __future__ import annotations

import hashlib
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path


class SmfError(ValueError):
    pass


@dataclass
class RawNote:
    sourceNote: int
    chunk: int
    channel: int      # 0..15
    pitch: int
    velocity: int
    start: int
    end: int

    @property
    def length(self) -> int:
        return self.end - self.start


@dataclass
class Stream:
    """一个 (MTrk 块, 通道) 上的全部音符：映射和清洗规则的最小单位。"""
    chunk: int
    channel: int
    trackName: str
    program: int | None
    notes: list[RawNote] = field(default_factory=list)

    @property
    def id(self) -> str:
        return stream_id(self.chunk, self.channel)

    @property
    def isDrum(self) -> bool:
        return self.channel == 9


def stream_id(chunk: int, channel: int) -> str:
    return f"t{chunk}c{channel}"


@dataclass
class ChunkInfo:
    index: int
    name: str
    endTick: int
    channels: list[int]          # 有音符的通道，升序
    otherEvents: Counter         # 没有被解释的事件：cc / pitchBend / sysex / meta:xx ...


@dataclass
class SmfFile:
    path: str
    sha256: str
    format: int
    ppq: int
    chunks: list[ChunkInfo]
    streams: dict[str, Stream]   # 按 (chunk, channel) 排序
    tempos: list[tuple[int, int]]             # (tick, 微秒/四分音符)
    meters: list[tuple[int, int, int]]        # (tick, 分子, 分母)
    length: int                               # 全曲长度 tick = 所有块最后一个事件的最大 tick
    orphanNoteOffs: int = 0
    unterminatedNotes: int = 0
    retriggeredNotes: int = 0

    @property
    def noteCount(self) -> int:
        return sum(len(s.notes) for s in self.streams.values())

    def all_notes(self) -> list[RawNote]:
        out = [n for s in self.streams.values() for n in s.notes]
        out.sort(key=lambda n: n.sourceNote)
        return out


def _vlq(data: bytes, pos: int, end: int) -> tuple[int, int]:
    value = 0
    for _ in range(4):
        if pos >= end:
            raise SmfError("truncated variable-length quantity")
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not b & 0x80:
            return value, pos
    raise SmfError("variable-length quantity longer than 4 bytes")


def decode_text(raw: bytes) -> str:
    """轨名解码：先 UTF-8，再 GBK（中文 Windows 工程常见），最后 latin-1 保底。"""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc).strip("\x00").strip()
        except UnicodeDecodeError:
            pass
    return raw.decode("latin-1").strip("\x00").strip()


_CHANNEL_DATA_LEN = {0x80: 2, 0x90: 2, 0xA0: 2, 0xB0: 2, 0xC0: 1, 0xD0: 1, 0xE0: 2}
_OTHER_NAMES = {0xA0: "polyAftertouch", 0xB0: "cc", 0xD0: "channelAftertouch", 0xE0: "pitchBend"}


def read_smf(path: str | Path) -> SmfFile:
    data = Path(path).read_bytes()
    return parse_smf(data, str(path))


def parse_smf(data: bytes, path: str = "<bytes>") -> SmfFile:
    if data[:4] == b"RIFF" and data[8:12] == b"RMID":
        # RMID 包装：取里面的 data 块
        idx = data.find(b"MThd")
        if idx < 0:
            raise SmfError("RMID without MThd")
        data = data[idx:]
    if data[:4] != b"MThd":
        raise SmfError("not a Standard MIDI File (missing MThd)")
    hlen = int.from_bytes(data[4:8], "big")
    if hlen < 6:
        raise SmfError("MThd too short")
    fmt = int.from_bytes(data[8:10], "big")
    division = int.from_bytes(data[12:14], "big")
    if division & 0x8000:
        raise SmfError("SMPTE time division is not supported")
    ppq = division

    pos = 8 + hlen
    chunks: list[ChunkInfo] = []
    streams: dict[tuple[int, int], Stream] = {}
    programs: dict[tuple[int, int], list[tuple[int, int]]] = {}
    tempos: list[tuple[int, int]] = []
    meters: list[tuple[int, int, int]] = []
    next_id = 0
    orphan = unterminated = retrig = 0

    while pos + 8 <= len(data):
        ctype = data[pos:pos + 4]
        clen = int.from_bytes(data[pos + 4:pos + 8], "big")
        body_start = pos + 8
        body_end = min(body_start + clen, len(data))   # 截断的文件：读到哪算哪
        pos = body_start + clen
        if ctype != b"MTrk":
            continue
        ci = len(chunks)
        name: str | None = None
        other: Counter = Counter()
        open_notes: dict[tuple[int, int], RawNote] = {}
        tick = 0
        status = 0
        p = body_start
        try:
            while p < body_end:
                delta, p = _vlq(data, p, body_end)
                tick += delta
                if p >= body_end:
                    break
                b = data[p]
                if b >= 0x80:
                    p += 1
                    if b < 0xF0:
                        status = b
                    elif b == 0xFF:
                        if p >= body_end:
                            break
                        mtype = data[p]
                        mlen, p = _vlq(data, p + 1, body_end)
                        mdata = data[p:p + mlen]
                        p += mlen
                        if mtype == 0x2F:
                            break
                        if mtype == 0x03 and name is None:
                            name = decode_text(mdata)
                        elif mtype == 0x51 and len(mdata) >= 3:
                            tempos.append((tick, int.from_bytes(mdata[:3], "big")))
                        elif mtype == 0x58 and len(mdata) >= 2:
                            meters.append((tick, mdata[0], 1 << mdata[1]))
                        else:
                            other[f"meta:{mtype:02x}"] += 1
                        continue
                    elif b in (0xF0, 0xF7):
                        slen, p = _vlq(data, p, body_end)
                        p += slen
                        other["sysex"] += 1
                        continue
                    else:
                        # 系统公共/实时消息在文件里不该出现；按 0 长度跳过
                        other[f"system:{b:02x}"] += 1
                        continue
                elif status == 0:
                    raise SmfError(f"data byte without status in track {ci}")
                kind = status & 0xF0
                ch = status & 0x0F
                n = _CHANNEL_DATA_LEN[kind]
                if p + n > body_end:
                    break
                d1 = data[p]
                d2 = data[p + 1] if n == 2 else 0
                p += n
                if kind == 0x90 and d2 > 0:
                    key = (ch, d1)
                    prev = open_notes.pop(key, None)
                    if prev is not None:
                        prev.end = tick
                        retrig += 1
                    note = RawNote(next_id, ci, ch, d1, d2, tick, tick)
                    next_id += 1
                    open_notes[key] = note
                    st = streams.get((ci, ch))
                    if st is None:
                        st = streams[(ci, ch)] = Stream(ci, ch, "", None)
                    st.notes.append(note)
                elif kind == 0x80 or kind == 0x90:
                    prev = open_notes.pop((ch, d1), None)
                    if prev is None:
                        orphan += 1
                    else:
                        prev.end = tick
                elif kind == 0xC0:
                    programs.setdefault((ci, ch), []).append((tick, d1))
                else:
                    other[_OTHER_NAMES[kind]] += 1
        except SmfError:
            if ci == 0 and not streams:
                raise
            # 坏块：保留已读到的内容
            other["truncated"] += 1
        for note in open_notes.values():
            note.end = tick
            unterminated += 1
        chans = sorted(c for (k, c) in streams if k == ci)
        chunks.append(ChunkInfo(ci, name or "", tick, chans, other))

    for (ci, ch), st in streams.items():
        st.trackName = chunks[ci].name
        st.program = _program_for(programs.get((ci, ch), []), st.notes[0].start)

    tempos.sort(key=lambda x: x[0])
    meters.sort(key=lambda x: x[0])
    return SmfFile(
        path=path,
        sha256=hashlib.sha256(data).hexdigest(),
        format=fmt,
        ppq=ppq,
        chunks=chunks,
        streams={st.id: st for _, st in sorted(streams.items())},
        tempos=_dedupe(tempos),
        meters=_dedupe(meters),
        length=max((c.endTick for c in chunks), default=0),
        orphanNoteOffs=orphan,
        unterminatedNotes=unterminated,
        retriggeredNotes=retrig,
    )


def _program_for(changes: list[tuple[int, int]], first_note_tick: int) -> int | None:
    """第一个音符处生效的音色；第一个音符之前没设过，就取这一路里第一次设的音色。"""
    if not changes:
        return None
    current = None
    for t, prog in changes:
        if t <= first_note_tick:
            current = prog
    return current if current is not None else changes[0][1]


def _dedupe(items: list) -> list:
    """同一 tick 有多条（比如 type 1 每块都写一遍）时保留最后一条，并去掉与前一条相同的值。"""
    by_tick: dict[int, tuple] = {}
    for it in items:
        by_tick[it[0]] = it
    out: list = []
    for t in sorted(by_tick):
        it = by_tick[t]
        if out and out[-1][1:] == it[1:]:
            continue
        out.append(it)
    return out
