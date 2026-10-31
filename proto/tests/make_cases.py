"""生成合成测试样例的输入 MIDI（tests/cases/<name>/input.mid）。

这些是人造的、每个只考一件事的最小样例，用来钉住规则行为；真实工程导出的 MIDI
按 README 里的步骤另外加进 tests/cases/。

    python tests/make_cases.py          # 重写所有 input.mid（确定性，重复跑结果相同）
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import mido

ROOT = Path(__file__).resolve().parent
CASES = ROOT.parent.parent / "testdata" / "cases"
sys.path.insert(0, str(ROOT.parent))

PPQ = 480
Q = PPQ
BAR = 4 * Q


def track(name: str | None, events: list[tuple[int, mido.Message]]) -> mido.MidiTrack:
    """events 用绝对 tick；同 tick 保持给定顺序。"""
    t = mido.MidiTrack()
    if name is not None:
        t.append(mido.MetaMessage("track_name", name=name, time=0))
    now = 0
    for tick, msg in sorted(enumerate(events), key=lambda x: (x[1][0], x[0])):
        tick, msg = msg
        t.append(msg.copy(time=tick - now))
        now = tick
    return t


def notes(ch: int, items: list[tuple[int, int, int]], vel: int = 90) -> list[tuple[int, mido.Message]]:
    """items: (start, length, pitch)。note-off 用真正的 note_off。"""
    ev = []
    for start, length, pitch in items:
        ev.append((start, mido.Message("note_on", channel=ch, note=pitch, velocity=vel)))
        ev.append((start + length, mido.Message("note_off", channel=ch, note=pitch, velocity=0)))
    return ev


def program(ch: int, prog: int, tick: int = 0):
    return [(tick, mido.Message("program_change", channel=ch, program=prog))]


def conductor(meters=((0, 4, 4),), bpm=120) -> mido.MidiTrack:
    ev = [(0, mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(bpm)))]
    for tick, n, d in meters:
        ev.append((tick, mido.MetaMessage("time_signature", numerator=n, denominator=d)))
    return track("Conductor", ev)


def melody(pitches: list[int], start: int = 0, step: int = Q, length: int | None = None) -> list[tuple[int, int, int]]:
    length = length or step - 20
    return [(start + i * step, length, p) for i, p in enumerate(pitches)]


def save(name: str, mid: mido.MidiFile, description: str) -> None:
    d = CASES / name
    d.mkdir(parents=True, exist_ok=True)
    mid.save(d / "input.mid")
    (d / "case.json").write_text(json.dumps({"description": description}, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")


def new(fmt: int = 1) -> mido.MidiFile:
    return mido.MidiFile(type=fmt, ticks_per_beat=PPQ)


def phrase_with_keyswitch(ch, ks_pitch, phrase_start, pitches, step=Q):
    ks = [(phrase_start - Q // 8, Q // 4, ks_pitch)]   # 提前 1/8 拍按下，短
    return ks + melody(pitches, phrase_start, step)


# ---------------------------------------------------------------------------

def case_type0_channels():
    m = new(0)
    ev = [(0, mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(100))),
          (0, mido.MetaMessage("time_signature", numerator=4, denominator=4))]
    ev += program(0, 73) + program(1, 42)
    ev += notes(0, melody([72, 74, 76, 77, 79, 77, 76, 74] * 2))
    ev += notes(1, melody([48, 43, 45, 41] * 4, step=2 * Q), vel=70)
    ev += notes(9, melody([36, 42, 38, 42] * 8, step=Q // 2, length=60), vel=100)
    m.tracks.append(track("Untitled", ev))
    save("type0_channels", m, "type 0：长笛(ch1, prog 73)、大提琴(ch2, prog 42)、鼓(ch10) 挤在一条轨 → 自动按通道拆成三条，按总谱排成 长笛/鼓/大提琴")


def case_same_program_channels():
    m = new()
    m.tracks.append(conductor())
    ev = program(0, 48) + program(1, 48)
    ev += notes(0, melody([67, 69, 71, 72] * 4))
    ev += notes(1, melody([60, 62, 64, 65] * 4))
    m.tracks.append(track("Strings", ev))
    save("same_program_channels", m, "一条轨两个通道、音色都是 48 → 拆轨待确认，先合成一条声部，所有音符标黄")


def case_ks_violin():
    m = new()
    m.tracks.append(conductor())
    items = []
    tune = [64, 66, 67, 69, 71, 72, 74, 76]
    for i, ks in enumerate([12, 14, 12, 14, 12, 16]):
        items += phrase_with_keyswitch(0, ks, Q + i * 2 * BAR, [p + (i % 3) for p in tune])
    m.tracks.append(track("Violins 1", program(0, 40) + notes(0, items)))
    save("ks_violin", m, "小提琴轨，乐句前用 C0/D0/E0（MIDI 12/14/16）切演奏法 → keyswitch 自动识别；检查 MIDI 里另出一条 KS 轨")


def case_ks_unknown_synth():
    m = new()
    m.tracks.append(conductor())
    items = []
    for i in range(6):
        items.append(((i + 1) * BAR - Q // 8, Q // 4, 36 if i % 2 == 0 else 38))
        items += melody([48, 52, 55, 60, 64, 60, 55, 52], (i + 1) * BAR, step=Q // 2)
    m.tracks.append(track("Lead Synth", notes(0, items)))
    save("ks_unknown_synth", m, "不认识的乐器，低端 36/38 和正常音隔 10 个半音、但不在极端音区 → keyswitch 待确认，这些音标黄")


def case_piano_octaves():
    m = new()
    m.tracks.append(conductor())
    items = []
    for i in range(8):
        t = i * BAR
        if i % 2 == 0:
            items += [(t, BAR - 30, 21), (t, BAR - 30, 33)]     # A0 + A1 八度
        else:
            items += [(t, BAR - 30, 40), (t, BAR - 30, 33)]
        items += [(t, BAR - 30, 57), (t, BAR - 30, 60), (t, BAR - 30, 64)]
        items += melody([69, 72, 76, 72], t, Q)
    m.tracks.append(track("Piano", program(0, 0) + notes(0, items)))
    save("piano_octaves", m, "钢琴低音八度 A0+A1：位置极端、空档 12，但每个都和 A1 八度叠加 → 不能当 keyswitch，不报")


def case_octave_violin_low():
    m = new()
    m.tracks.append(conductor())
    tune = [43, 45, 47, 48, 50, 52, 54, 55, 57, 59, 60, 62, 64, 66, 67, 69, 71, 72, 74]
    m.tracks.append(track("Violin 2", notes(0, melody(tune + tune[::-1]))))
    save("octave_violin_low", m, "第二小提琴整体低一个八度（音源按 C3 记中央 C）→ 轨名认出小提琴，八度 +12 自动应用")


def case_octave_program_only():
    m = new()
    m.tracks.append(conductor())
    tune = [62, 64, 66, 67, 69, 71, 72, 74, 76, 78, 79, 81, 83, 84, 86]
    m.tracks.append(track(None, program(0, 72) + notes(0, melody(tune + tune[::-1]))))
    save("octave_program_only", m, "没轨名、音色 72（短笛），按记谱音高写低了一个八度 → 乐器只从音色号猜出，八度 +12 待确认，原位超极限的音标黄")


def case_octave_few_outliers():
    m = new()
    m.tracks.append(conductor())
    tune = [55, 57, 60, 62, 64, 65, 67, 69, 72, 69, 67, 65, 64, 62, 60, 62, 64, 67, 69, 72, 69, 67, 64, 62, 60, 79, 80, 78, 79, 72]
    m.tracks.append(track("Horn 1", program(0, 60) + notes(0, melody(tune))))
    save("octave_few_outliers", m, "圆号声部只有 4/30 个音写过了上限 → 不足以说明音源错了八度：降八度只报待确认，不自动应用")


def case_false_alarms():
    """仿真实工程 liubian-full-v3 里的四个误报：都不是 keyswitch，什么都不该报。"""
    m = new()
    m.tracks.append(conductor())
    lh = melody([45, 52, 57, 52] * 6, 0, Q)
    lh[0] = (0, Q - 20, 33)                       # 两个 A1，比其余低 12 个半音……
    lh[12] = (12 * Q, Q - 20, 33)
    m.tracks.append(track("Piano LH", notes(0, lh)))
    cel = melody([81, 84, 86, 88, 86, 84] * 4, 0, Q // 2)
    cel += melody([69, 71, 72], 13 * Q, Q // 2)   # 钢片琴一句低一点的旋律，空档 9
    m.tracks.append(track("Celesta", notes(1, cel)))
    vln = melody([55, 57, 59, 60, 62, 64] * 3, 0, Q) + [(18 * Q, Q, 71)]  # 第二小提琴一个高音
    m.tracks.append(track("Violins II", notes(2, vln)))
    pad = [(0, 4 * BAR, 52), (0, 4 * BAR, 57), (0, 4 * BAR, 60), (4 * BAR, 4 * BAR, 45), (4 * BAR, 4 * BAR, 57),
           (8 * BAR, 4 * BAR, 45), (8 * BAR, 4 * BAR, 60), (8 * BAR, 4 * BAR, 64)]
    m.tracks.append(track("Pad", notes(3, pad, vel=60)))
    save("false_alarms", m, "仿真实工程里的误报：钢琴左手两个 A1、钢片琴三个低一点的旋律音、第二小提琴一个高音、Pad 两个低音——都不是 keyswitch，不该报")


def case_ks_and_octave_cello():
    m = new()
    m.tracks.append(conductor())
    items = []
    tune = [60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 79, 81, 84, 86, 88, 91, 93, 96]
    for i, ks in enumerate([36, 38, 36, 38]):
        items += phrase_with_keyswitch(0, ks, Q + i * 5 * BAR, tune, step=Q)
    m.tracks.append(track("Cello", program(0, 42) + notes(0, items, vel=80)))
    save("ks_and_octave_cello", m,
         "大提琴整体高一个八度，且用 36/38 做 keyswitch。第一圈：36/38 不在极端音区、也还在大提琴音域内，只能待确认；"
         "八度 -12 自动应用后再转一圈，36/38 落到演奏极限外 → keyswitch 升为自动")


def case_manual_merge():
    m = new()
    m.tracks.append(conductor(meters=((0, 4, 4), (4 * BAR, 3, 4))))
    sus = melody([67, 69, 71, 72], 0, BAR, BAR - 30) + melody([74, 72, 71], 8 * BAR, 3 * Q, 3 * Q - 30)
    stac = melody([79, 79, 81, 79, 77, 76, 74, 72] * 2, 4 * BAR, Q // 2, 100)
    m.tracks.append(track("Vln1 sustain", program(0, 40) + notes(0, sus)))
    m.tracks.append(track("Vln1 staccato", program(1, 40) + notes(1, stac)))
    m.tracks.append(track("Viola", program(2, 41) + notes(2, melody([60, 62, 64, 65] * 3, 0, BAR))))
    save("manual_merge", m, "按演奏法拆开的两条小提琴轨（sustain/staccato），用户在 sidecar 里合成一条“小提琴 I”；中途 4/4 → 3/4")


def case_parse_edges():
    """手写字节：running status、velocity 0 当 note-off、同音重触发、孤立 note-off、未结束音、GBK 轨名。"""
    def vlq(n):
        out = [n & 0x7F]
        n >>= 7
        while n:
            out.append(0x80 | (n & 0x7F))
            n >>= 7
        return bytes(reversed(out))

    def chunk(body):
        return b"MTrk" + len(body).to_bytes(4, "big") + body

    name = "小提琴".encode("gbk")
    t0 = (vlq(0) + b"\xff\x51\x03\x07\xa1\x20"            # tempo 120
          + vlq(0) + b"\xff\x58\x04\x03\x02\x18\x08"      # 3/4
          + vlq(0) + b"\xff\x2f\x00")
    t1 = (vlq(0) + b"\xff\x03" + vlq(len(name)) + name
          + vlq(0) + b"\x90\x3c\x50"       # 0: on 60          -> sourceNote 0
          + vlq(0) + b"\x40\x50"           # running status: on 64 -> 1
          + vlq(240) + b"\x3c\x00"         # 240: off 60 (vel 0)
          + vlq(0) + b"\x40\x00"           # 240: off 64
          + vlq(240) + b"\x3e\x60"         # 480: on 62        -> 2
          + vlq(120) + b"\xb0\x40\x7f"     # cc64 (未解释事件)
          + vlq(120) + b"\x90\x3e\x61"     # 720: 重触发 62 -> 旧音在 720 结束；新音 -> 3
          + vlq(240) + b"\x80\x3e\x00"     # 960: off 62
          + vlq(0) + b"\x80\x45\x00"       # 960: 孤立 off 69
          + vlq(0) + b"\x90\x43\x64"       # 960: on 67，不关 -> 4，结束于 1440
          + vlq(480) + b"\xff\x2f\x00")
    t2 = (vlq(0) + b"\xff\x03\x04Bass"
          + vlq(100) + b"\x91\x24\x70"     # ch2 on 36 -> 5
          + vlq(380) + b"\x81\x24\x40"
          + vlq(0) + b"\xff\x2f\x00")
    data = (b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") + (3).to_bytes(2, "big")
            + PPQ.to_bytes(2, "big") + chunk(t0) + b"XFIH\x00\x00\x00\x02ab" + chunk(t1) + chunk(t2))
    d = CASES / "parse_edges"
    d.mkdir(parents=True, exist_ok=True)
    (d / "input.mid").write_bytes(data)
    (d / "case.json").write_text(json.dumps(
        {"description": "解析边界：running status、vel 0 当 note-off、同音重触发、孤立 note-off、未结束音、GBK 轨名、未知块"},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_merge_sidecar():
    """manual_merge 的用户 sidecar：在自动分析结果上把两条小提琴并成一条。"""
    from reader_proto import analyze as A
    from reader_proto import sidecar as S
    from reader_proto.smf import read_smf

    d = CASES / "manual_merge"
    smf = read_smf(d / "input.mid")
    sc = A.analyze(smf)
    vln = [l.id for l in sc.lanes if l.name.startswith("Vln1")]
    sc = A.merge_lanes(smf, sc, vln, "小提琴 I")
    S.save(sc, d / "input.reader.json")


ALL = [case_type0_channels, case_same_program_channels, case_ks_violin, case_ks_unknown_synth,
       case_piano_octaves, case_octave_violin_low, case_octave_few_outliers, case_false_alarms, case_octave_program_only, case_ks_and_octave_cello,
       case_manual_merge, case_parse_edges]

if __name__ == "__main__":
    for f in ALL:
        f()
    write_merge_sidecar()
    print(f"wrote {len(ALL)} cases to {CASES}")
