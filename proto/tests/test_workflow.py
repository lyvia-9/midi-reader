"""用户修正回流：重新分析不覆盖用户的结论；逐音修正生效；原文件不动。"""
import hashlib
import shutil

from reader_proto import analyze as A
from reader_proto import cli
from reader_proto import project as P
from reader_proto import sidecar as S
from reader_proto.model import NoteFlag
from reader_proto.smf import read_smf

from golden import CASES


def test_rejected_decision_survives_reanalysis():
    smf = read_smf(CASES / "ks_violin" / "input.mid")
    sc = A.apply_user_decision(smf, A.analyze(smf), "ks:t1c0:low", S.STATUS_REJECTED)
    sc2 = A.analyze(smf, S.Sidecar.from_json(sc.to_json()))
    assert sc2.decision("ks:t1c0:low").status == "rejected"
    proj = P.project(smf, sc2)
    assert not any(n.flags & (NoteFlag.KEY_SWITCH | NoteFlag.REVIEW) for n in proj.lanes[0].notes)


def test_note_override_beats_rule():
    smf = read_smf(CASES / "ks_violin" / "input.mid")
    sc = A.analyze(smf)
    first_ks = next(n for n in smf.all_notes() if n.pitch == 12)
    sc.noteOverrides[first_ks.sourceNote] = {"keyswitch": False}
    sc = A.analyze(smf, sc)
    proj = P.project(smf, sc)
    n = next(n for n in proj.lanes[0].notes if n.sourceNote == first_ks.sourceNote)
    assert not n.flags & NoteFlag.KEY_SWITCH
    assert n.flags & NoteFlag.HARD_RANGE        # 不是 keyswitch，那就是超出小提琴音域的音
    assert sc.decision("ks:t1c0:low").status == "auto"   # 其余照旧


def test_user_instrument_enables_auto():
    smf = read_smf(CASES / "octave_program_only" / "input.mid")
    sc = A.analyze(smf)
    sc = A.set_lane_instrument(smf, sc, sc.lanes[0].id, "piccolo")
    assert sc.lanes[0].instrumentSource == "user"
    assert sc.decision("oct:t1c0").status == "auto"


def test_cli_run_leaves_midi_untouched(tmp_path, capsys):
    src = tmp_path / "song.mid"
    shutil.copy(CASES / "ks_and_octave_cello" / "input.mid", src)
    before = hashlib.sha256(src.read_bytes()).hexdigest()
    assert cli.main(["run", str(src), "-q"]) == 0
    assert hashlib.sha256(src.read_bytes()).hexdigest() == before
    assert (tmp_path / "song.mid.reader.json").exists()
    assert (tmp_path / "song.lanes.json").exists() and (tmp_path / "song.lanes.mid").exists()
    # 第二次 run 读回已有 sidecar，结果不变
    first = (tmp_path / "song.mid.reader.json").read_text(encoding="utf-8")
    assert cli.main(["run", str(src), "-q"]) == 0
    assert (tmp_path / "song.mid.reader.json").read_text(encoding="utf-8") == first


def test_cli_decide_and_note(tmp_path, capsys):
    src = tmp_path / "s.mid"
    shutil.copy(CASES / "ks_unknown_synth" / "input.mid", src)
    assert cli.main(["run", str(src), "-q"]) == 0
    assert cli.main(["decide", str(src), "ks:t1c0:low", "confirm"]) == 0
    sc = S.load(tmp_path / "s.mid.reader.json")
    assert sc.decision("ks:t1c0:low").status == "confirmed"
    assert cli.main(["note", str(src), "0", "--as", "not-ks"]) == 0
    assert S.load(tmp_path / "s.mid.reader.json").noteOverrides == {0: {"keyswitch": False}}
    assert cli.main(["decide", str(src), "nope", "confirm"]) == 2


def test_unreferenced_stream_goes_to_unassigned():
    smf = read_smf(CASES / "manual_merge" / "input.mid")
    sc = A.analyze(smf)
    sc.lanes = [l for l in sc.lanes if "t3c2" not in l.sources]
    proj = P.project(smf, sc)
    u = proj.lanes[-1]
    assert u.id == "U:t3c2" and u.info.group == "未归类" and len(u.notes) == 12


def test_sha_mismatch_warns():
    smf = read_smf(CASES / "ks_violin" / "input.mid")
    sc = A.analyze(smf)
    sc.source["sha256"] = "0" * 64
    assert any("sha256" in w for w in P.project(smf, sc).warnings)
