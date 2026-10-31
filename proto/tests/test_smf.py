from collections import Counter

import pytest
import symusic

from reader_proto.smf import read_smf

from golden import CASES, case_dirs


def test_parse_edges():
    smf = read_smf(CASES / "parse_edges" / "input.mid")
    assert smf.ppq == 480 and len(smf.chunks) == 3   # 未知块 XFIH 被跳过、不计入序号
    assert smf.chunks[1].name == "小提琴"            # GBK 轨名
    assert smf.tempos == [(0, 500000)] and smf.meters == [(0, 3, 4)]
    got = [(n.sourceNote, n.chunk, n.channel, n.pitch, n.start, n.end) for n in smf.all_notes()]
    assert got == [
        (0, 1, 0, 60, 0, 240),
        (1, 1, 0, 64, 0, 240),      # running status
        (2, 1, 0, 62, 480, 720),    # 被重触发截断
        (3, 1, 0, 62, 720, 960),
        (4, 1, 0, 67, 960, 1440),   # 未结束，止于块尾
        (5, 2, 1, 36, 100, 480),
    ]
    assert (smf.orphanNoteOffs, smf.unterminatedNotes, smf.retriggeredNotes) == (1, 1, 1)
    assert smf.chunks[1].otherEvents["cc"] == 1
    assert smf.length == 1440


CLEAN = [d for d in case_dirs() if d.name != "parse_edges"]


@pytest.mark.parametrize("d", CLEAN, ids=lambda d: d.name)
def test_matches_symusic(d):
    """交叉校验：同一个文件，按 (音色, 是否鼓) 汇总的音符多重集合和 symusic 一致。"""
    smf = read_smf(d / "input.mid")
    ours = Counter()
    for st in smf.streams.values():
        for n in st.notes:
            ours[(st.isDrum, n.start, n.length, n.pitch, n.velocity)] += 1
    theirs = Counter()
    for tr in symusic.Score(str(d / "input.mid")).tracks:
        for n in tr.notes:
            theirs[(tr.is_drum, n.time, n.duration, n.pitch, n.velocity)] += 1
    assert ours == theirs
    assert smf.ppq == symusic.Score(str(d / "input.mid")).tpq
