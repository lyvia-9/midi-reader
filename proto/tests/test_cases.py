"""对每个样例：golden 文件一致 + 针对它要考的那件事的显式断言（防止 golden 把错的结果钉死）。"""
from __future__ import annotations

from pathlib import Path

import pytest
import symusic

from reader_proto import analyze as A
from reader_proto import export as E
from reader_proto import project as P
from reader_proto import sidecar as S
from reader_proto.model import NoteFlag
from reader_proto.smf import read_smf

from golden import CASES, case_dirs, check


@pytest.mark.parametrize("d", case_dirs(), ids=lambda d: d.name)
def test_golden(d: Path):
    assert check(d) == []


def load(name: str):
    d = CASES / name
    smf = read_smf(d / "input.mid")
    existing = S.load(d / "input.reader.json") if (d / "input.reader.json").exists() else None
    sc = A.analyze(smf, existing)
    return smf, sc, P.project(smf, sc)


def statuses(sc):
    return {d.id: d.status for d in sc.decisions}


def test_type0_split_and_score_order():
    smf, sc, proj = load("type0_channels")
    assert statuses(sc) == {"split:t0": "auto"}
    assert [l.instrument for l in sc.lanes] == ["flute", "drums", "cello"]
    assert [l.info.group for l in proj.lanes] == ["木管", "打击乐", "弦乐"]
    assert sum(len(l.notes) for l in proj.lanes) == smf.noteCount


def test_same_program_channels_held_for_review():
    smf, sc, proj = load("same_program_channels")
    assert statuses(sc) == {"split:t1": "review"}
    assert len(proj.lanes) == 1 and sorted(sc.lanes[0].sources) == ["t1c0", "t1c1"]
    assert all(n.flags & NoteFlag.REVIEW for n in proj.lanes[0].notes)


def test_same_program_channels_confirm_splits_lane():
    smf, sc, _ = load("same_program_channels")
    sc = A.apply_user_decision(smf, sc, "split:t1", S.STATUS_CONFIRMED)
    proj = P.project(smf, sc)
    assert [l.sources for l in sc.lanes] == [["t1c0"], ["t1c1"]]
    assert not any(n.flags & NoteFlag.REVIEW for l in proj.lanes for n in l.notes)


def test_ks_violin():
    smf, sc, proj = load("ks_violin")
    d = sc.decision("ks:t1c0:low")
    assert d.status == "auto" and d.params["pitches"] == [12, 14, 16]
    ks = [n for n in proj.lanes[0].notes if n.flags & NoteFlag.KEY_SWITCH]
    assert len(ks) == 6 and {n.pitch for n in ks} == {12, 14, 16}
    # 去掉 keyswitch 后没有任何超音域
    assert not any(n.flags & (NoteFlag.SOFT_RANGE | NoteFlag.HARD_RANGE) for n in proj.lanes[0].notes
                   if not n.flags & NoteFlag.KEY_SWITCH)


def test_ks_unknown_instrument_is_review_only():
    smf, sc, proj = load("ks_unknown_synth")
    assert statuses(sc) == {"ks:t1c0:low": "review"}
    flagged = [n for n in proj.lanes[0].notes if n.flags & NoteFlag.REVIEW]
    assert {n.pitch for n in flagged} == {36, 38}
    assert not any(n.flags & NoteFlag.KEY_SWITCH for n in proj.lanes[0].notes)


def test_piano_octaves_not_keyswitch():
    _, sc, _ = load("piano_octaves")
    assert sc.decisions == []


def test_octave_violin_low():
    smf, sc, proj = load("octave_violin_low")
    d = sc.decision("oct:t1c0")
    assert d.status == "auto" and d.params["shift"] == 12
    raw = {n.sourceNote: n.pitch for n in smf.all_notes()}
    assert all(n.pitch == raw[n.sourceNote] + 12 and n.flags == 0 for n in proj.lanes[0].notes)


def test_octave_from_program_is_review():
    smf, sc, proj = load("octave_program_only")
    assert sc.lanes[0].instrument == "piccolo" and sc.lanes[0].instrumentSource == "program"
    assert statuses(sc) == {"oct:t1c0": "review"}
    for n in proj.lanes[0].notes:   # 未应用；原位超极限的才标黄
        assert bool(n.flags & NoteFlag.REVIEW) == (n.pitch < 74)


def test_octave_few_outliers_not_auto():
    _, sc, proj = load("octave_few_outliers")
    assert statuses(sc) == {"oct:t1c0": "review"}
    assert sorted(n.pitch for n in proj.lanes[0].notes if n.flags & NoteFlag.REVIEW) == [78, 79, 79, 80]


def test_false_alarms_from_real_project():
    _, sc, _ = load("false_alarms")
    assert sc.decisions == []


def test_ks_and_octave_feed_each_other():
    smf, sc, proj = load("ks_and_octave_cello")
    assert statuses(sc) == {"ks:t1c0:low": "auto", "oct:t1c0": "auto"}
    assert sc.decision("oct:t1c0").params["shift"] == -12
    assert sc.decision("ks:t1c0:low").evidence["outsideInstrument"] is True
    # keyswitch 不跟着平移
    assert {n.pitch for n in proj.lanes[0].notes if n.flags & NoteFlag.KEY_SWITCH} == {36, 38}


def test_manual_merge():
    smf, sc, proj = load("manual_merge")
    lane = proj.lanes[0]
    assert lane.info.name == "小提琴 I" and lane.info.sourceTrackCount == 2
    assert len(lane.notes) == 7 + 16
    assert [n.start for n in lane.notes] == sorted(n.start for n in lane.notes)
    assert proj.meterChanges == [(0, 4, 4), (4 * 4 * 480, 3, 4)]


def test_export_roundtrip(tmp_path):
    smf, sc, proj = load("ks_violin")
    out = tmp_path / "check.mid"
    E.export(proj, sc, smf, out)
    s = symusic.Score(str(out))
    names = [t.name for t in s.tracks]
    assert names == ["Violins 1", "Violins 1 · KS"]
    assert len(s.tracks[0].notes) == 48 and len(s.tracks[1].notes) == 6
    assert s.tpq == 480


def test_export_octave_applied(tmp_path):
    smf, sc, proj = load("octave_violin_low")
    out = tmp_path / "check.mid"
    E.export(proj, sc, smf, out)
    s = symusic.Score(str(out))
    assert min(n.pitch for n in s.tracks[0].notes) == 55


def test_short_keyswitch_not_counted_inline():
    """很短的 keyswitch（≤ 1/8 拍）不能把自己的收音当成“前一个音结束”。"""
    from reader_proto.rules import keyswitch
    from reader_proto.smf import RawNote, Stream
    st = Stream(1, 0, "Violins 1", 40)
    sn = 0
    for i in range(6):
        t = 1920 * (i + 1)
        st.notes.append(RawNote(sn, 1, 0, 12, 100, t - 120, t - 90)); sn += 1   # 30 tick 长的 KS，提前 1/4 拍
        for j in range(4):
            st.notes.append(RawNote(sn, 1, 0, 64 + j, 90, t + j * 480, t + j * 480 + 400)); sn += 1
    from reader_proto.instruments import BY_ID
    [d] = keyswitch.analyze(st, 480, BY_ID["violin"], "name", 0, {})
    assert d.evidence["inline"] == 0 and d.status == "auto"
