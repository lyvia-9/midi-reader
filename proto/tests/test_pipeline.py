"""音频管线：预设、MIDI 写出与拼接、缓存、配方、各 provider 的命令行/HTTP 形状、worker 协议。

不装任何模型：本地 provider 只检查拼出来的命令，整条管线用假 provider 跑；sunoapi 用假 HTTP。
"""
import io
import json
import threading
from pathlib import Path

import pytest

from reader_proto import sidecar as S
from reader_proto.pipeline import merge as M
from reader_proto.pipeline import presets as PR
from reader_proto.pipeline.base import Artifact, Capability, Context, PipelineError, Task
from reader_proto.pipeline.cache import Cache
from reader_proto.pipeline.config import Config
from reader_proto.pipeline.midiwrite import TrackOut, seconds_to_ticks, ticks_to_seconds, write_smf
from reader_proto.pipeline.providers import BUILTIN
from reader_proto.pipeline.providers.basic_pitch import BasicPitch
from reader_proto.pipeline.providers.demucs import Demucs
from reader_proto.pipeline.providers.fake import Fake, _silence
from reader_proto.pipeline.providers.muscriptor import MuScriptor
from reader_proto.pipeline.providers.sunoapi import SunoApi
from reader_proto.pipeline.run import Runner
from reader_proto.pipeline.worker import Worker
from reader_proto.smf import read_smf


@pytest.fixture
def audio(tmp_path):
    p = tmp_path / "song.wav"
    _silence(p)
    return p


def fakes(**ids):
    """把假 provider 登记成真名字，配方就按真 preset 的写法走。"""
    return {pid: Fake() for pid in ids or ("fake",)}


# ---- 预设 ----

def test_builtin_presets_valid():
    presets = PR.builtin()
    assert {"pop-band", "string-quartet", "orchestra"} <= set(presets)
    pop = presets["pop-band"]
    assert pop.route == "stems" and pop.stems == ["vocals", "guitar", "piano", "bass", "drums"]
    assert "drums" in pop.muscriptor_instruments


def test_preset_rejects_unknown_names():
    base = {"format": PR.FORMAT, "version": 1, "id": "x", "slots": [{"lane": "a", "instrument": "kazoo"}]}
    with pytest.raises(PR.PresetError, match="kazoo"):
        PR.Preset.from_json(base)
    base["slots"] = [{"lane": "a", "muscriptor": ["kazoo"]}]
    with pytest.raises(PR.PresetError, match="MuScriptor"):
        PR.Preset.from_json(base)
    base["slots"] = [{"lane": "a"}]
    base["route"] = "stems"
    with pytest.raises(PR.PresetError, match="stem"):
        PR.Preset.from_json(base)


def test_slot_matching_exact_before_regex():
    pop = PR.builtin()["pop-band"]
    lanes = [s.lane for s in pop.slots]
    assert lanes[PR.slot_for(pop, ["electric bass"])] == "贝斯"          # MuScriptor 轨名
    assert lanes[PR.slot_for(pop, ["Electric Bass (finger)"])] == "贝斯"  # sunoapi 轨名（正则）
    assert lanes[PR.slot_for(pop, ["Drums"])] == "鼓"
    assert lanes[PR.slot_for(pop, ["Bass Drum"])] == "鼓"                 # 不能被“贝斯”抢走
    assert PR.slot_for(pop, ["Theremin"]) is None


# ---- MIDI 写出 / 拼接 ----

def test_tempo_conversion_roundtrip():
    tempos = [(0, 500_000), (960, 250_000)]   # 120 BPM 两拍后变 240 BPM
    assert ticks_to_seconds(tempos, 480, 960) == pytest.approx(1.0)
    assert ticks_to_seconds(tempos, 480, 1440) == pytest.approx(1.25)
    for t in (0, 100, 960, 1500, 5000):
        assert seconds_to_ticks(tempos, 480, ticks_to_seconds(tempos, 480, t)) == t


def test_write_smf_reads_back(tmp_path):
    p = tmp_path / "x.mid"
    write_smf(p, [TrackOut("violin", [(0, 480, 67, 90), (480, 960, 69, 80)]),
                  TrackOut("drums", [(0, 10, 36, 100)], drum=True),
                  TrackOut("cello", [(0, 960, 48, 70)], program=42)],
              tempos=[(0, 600_000)], meters=[(0, 3, 4)])
    smf = read_smf(p)
    assert smf.format == 1 and smf.ppq == 480
    assert smf.tempos == [(0, 600_000)] and smf.meters == [(0, 3, 4)]
    assert [c.name for c in smf.chunks] == ["", "violin", "drums", "cello"]
    assert smf.streams["t2c9"].isDrum
    assert smf.streams["t3c1"].program == 42
    assert [(n.start, n.end, n.pitch) for n in smf.streams["t1c0"].notes] == [(0, 480, 67), (480, 960, 69)]


def test_merge_aligns_to_audio_time(tmp_path):
    # MuScriptor 风格：90 BPM，音频 0 秒落在 MIDI 的 0.5 秒（marker 记着），一个音在音频 1.0 秒
    ms = tmp_path / "drums.mid"
    tempos = [(0, 666_667)]
    write_smf(ms, [TrackOut("drums", [(seconds_to_ticks(tempos, 480, 1.5), seconds_to_ticks(tempos, 480, 1.6), 36, 100)],
                            drum=True)], tempos=tempos, meters=[(0, 4, 4)])
    data = ms.read_bytes()
    marker = b"\xff\x06" + bytes([len(b"muscriptor:bar_offset=0.5000")]) + b"muscriptor:bar_offset=0.5000"
    # 塞进第 0 块开头（块长度相应加上）
    i = data.index(b"MTrk") + 8
    n = int.from_bytes(data[i - 4:i], "big") + 1 + len(marker)
    ms.write_bytes(data[:i - 4] + n.to_bytes(4, "big") + b"\x00" + marker + data[i:])
    assert M.audio_offset(ms) == pytest.approx(0.5)
    # Basic Pitch 风格：120 BPM，tick 0 = 音频 0 秒，一个音也在音频 1.0 秒
    bp = tmp_path / "bass.mid"
    write_smf(bp, [TrackOut("p", [(960, 1200, 40, 80)])])

    out = M.merge([M.Part(bp, "贝斯"), M.Part(ms, "鼓", drum=True)], tmp_path / "merged.mid")
    smf = read_smf(out)
    assert smf.tempos == [(0, 666_667)] and smf.meters == [(0, 4, 4)]   # 主文件是带节拍的那个
    bass = smf.streams["t1c0"].notes[0]
    drum = smf.streams["t2c9"].notes[0]
    assert bass.start == drum.start                                       # 两边都落在音频 1.0 秒
    assert [c.name for c in smf.chunks[1:]] == ["贝斯", "鼓"]


# ---- 缓存与配方 ----

def test_cache_reuses_result(tmp_path, audio):
    fake = Fake()
    runner = Runner(cache=Cache(tmp_path / "cache"), providers={"fake": fake})
    preset = PR.builtin()["string-quartet"]
    r1 = runner.transcribe(Artifact("audio", audio), preset, transcriber="fake", out_dir=tmp_path / "r1")
    r2 = runner.transcribe(Artifact("audio", audio), preset, transcriber="fake", out_dir=tmp_path / "r2")
    assert fake.calls == 1
    run2 = json.loads((r2.run_dir / "run.json").read_text(encoding="utf-8"))
    assert run2["steps"][0]["cached"] is True
    assert r1.midi.read_bytes() == r2.midi.read_bytes()


def test_full_route_applies_preset(tmp_path, audio):
    events = []
    runner = Runner(cache=Cache(tmp_path / "cache"), providers={"muscriptor": _named(Fake(), MuScriptor)},
                    on_event=events.append)
    preset = PR.builtin()["orchestra"]
    r = runner.transcribe(Artifact("audio", audio), preset, out_dir=tmp_path / "run")
    run = json.loads((r.run_dir / "run.json").read_text(encoding="utf-8"))
    assert run["format"] == "midi-reader-pipeline-run" and run["route"] == "full"
    assert run["steps"][0]["params"]["instruments"] == preset.muscriptor_instruments
    assert run["source"]["sha256"] and run["result"]["midi"] == "song.mid"
    sc = S.load(r.sidecar)
    assert [l.name for l in sc.lanes] == [s.lane for s in preset.slots]
    horn = next(l for l in sc.lanes if l.name == "圆号")
    assert horn.instrument == "horn" and horn.instrumentSource == "user" and horn.role == "harmony"
    assert events[-1]["event"] == "done"


def test_stems_route_merges_per_slot(tmp_path, audio):
    providers = {"demucs": Fake(), "basic-pitch": Fake(), "muscriptor": Fake()}
    runner = Runner(cache=Cache(tmp_path / "cache"), providers=providers)
    preset = PR.builtin()["pop-band"]
    r = runner.transcribe(Artifact("audio", audio), preset, out_dir=tmp_path / "run")
    run = json.loads((r.run_dir / "run.json").read_text(encoding="utf-8"))
    assert [s["provider"] for s in run["steps"]] == ["demucs", "basic-pitch", "basic-pitch", "basic-pitch",
                                                     "basic-pitch", "muscriptor"]
    assert run["steps"][0]["params"]["stems"] == preset.stems
    assert run["steps"][-1]["params"]["instruments"] == ["drums"]
    smf = read_smf(r.midi)
    assert [c.name for c in smf.chunks[1:]] == [s.lane for s in preset.slots]
    sc = S.load(r.sidecar)
    assert [l.name for l in sc.lanes] == [s.lane for s in preset.slots]
    assert sc.lanes[0].group == "人声" and sc.lanes[0].instrument is None
    assert next(l for l in sc.lanes if l.name == "鼓").instrument == "drums"


def test_unavailable_provider_fails_before_running(tmp_path, audio):
    runner = Runner(Config({"providers": {"muscriptor": {"command": "definitely-not-installed-xyz"}}}),
                    cache=Cache(tmp_path / "cache"))
    with pytest.raises(PipelineError, match="definitely-not-installed-xyz"):
        runner.transcribe(Artifact("audio", audio), PR.builtin()["orchestra"], out_dir=tmp_path / "run")


def _named(provider, cls):
    """假 provider 但带真 provider 的清单（这样配方会给它传乐器条件）。"""
    provider.manifest = cls.manifest
    return provider


# ---- 本地 provider 拼出来的命令 ----

def test_muscriptor_command(tmp_path, audio):
    m = MuScriptor({"model": "large", "device": "cuda", "quantize": True})
    cmd = m.build_command(Task(Capability.TRANSCRIBE, [Artifact("audio", audio)],
                               {"instruments": ["violin", "cello"]}), tmp_path / "o.mid")
    assert cmd[:3] == ["uvx", "muscriptor@0.4.0", "transcribe"]
    assert cmd[cmd.index("--instruments") + 1] == "violin,cello"
    assert cmd[cmd.index("--model") + 1] == "large" and "--quantize" in cmd
    assert cmd[cmd.index("--device") + 1] == "cuda"
    assert "device" not in m.cache_identity()                 # 换设备不该让缓存失效
    with pytest.raises(PipelineError, match="kazoo"):
        m.build_command(Task(Capability.TRANSCRIBE, [Artifact("audio", audio)], {"instruments": ["kazoo"]}), "o")


def test_demucs_and_basic_pitch_commands(tmp_path, audio):
    d = Demucs({"command": ["python", "-m", "demucs"]})
    cmd = d.build_command(Task(Capability.SEPARATE, [Artifact("audio", audio)]), tmp_path)
    assert cmd[:3] == ["python", "-m", "demucs"] and cmd[cmd.index("-n") + 1] == "htdemucs_6s"
    assert cmd[cmd.index("--filename") + 1] == "{stem}.{ext}"
    assert Demucs({"model": "nope"}).problems()[-1].startswith("不认识")
    assert Demucs().build_command(Task(Capability.SEPARATE, [Artifact("audio", audio)]), tmp_path)[0] == "uvx"
    b = BasicPitch({"onsetThreshold": 0.6})
    cmd = b.build_command(Task(Capability.TRANSCRIBE, [Artifact("audio", audio)]), tmp_path)
    assert cmd[0] == "uvx" and cmd[cmd.index("basic-pitch@0.4.0") + 1:][:2] == [str(tmp_path), str(audio)]
    assert cmd[cmd.index("--onset-threshold") + 1] == "0.6"


def test_all_builtin_providers_have_manifests():
    assert set(BUILTIN) == {"muscriptor", "demucs", "basic-pitch", "sunoapi", "fake"}
    for cls in BUILTIN.values():
        assert cls.manifest.capabilities


# ---- sunoapi（假 HTTP）----

class FakeSuno:
    """按 docs.sunoapi.org 的 OpenAPI 回响应；记下每个请求。"""

    def __init__(self, polls_before_done=1):
        self.calls = []
        self.polls_before_done = polls_before_done
        self.polls = {}

    def __call__(self, method, url, body=None, file=None, fields=None):
        self.calls.append((method, url, body, fields))
        path = url.split("?")[0]
        if path.endswith("/api/file-stream-upload"):
            return {"success": True, "code": 200, "msg": "ok", "data": {"downloadUrl": "https://tmp/song.wav"}}
        if method == "POST":
            return {"code": 200, "msg": "success", "data": {"taskId": path.rsplit("/", 2)[-2] + "-task"}}
        n = self.polls[path] = self.polls.get(path, 0) + 1
        done = n > self.polls_before_done
        if "vocal-removal" in path:
            return {"code": 200, "msg": "success", "data": {
                "successFlag": "SUCCESS" if done else "PENDING",
                "response": {"vocalUrl": "https://x/v.mp3", "instrumentalUrl": "https://x/i.mp3",
                             "originData": [{"id": "vid", "stem_type_group_name": "Vocals"}]} if done else None}}
        if "midi" in path:
            return {"code": 200, "msg": "success", "data": {"successFlag": 1 if done else 0, "midiData": {
                "state": "complete", "instruments": [
                    {"name": "Drums", "notes": [{"pitch": 36, "start": 0.0, "end": 0.1, "velocity": 1}]},
                    {"name": "Electric Bass (finger)", "notes": [{"pitch": 40, "start": 0.5, "end": 1.0, "velocity": 0.5}]},
                ]} if done else None}}
        if "generate/record-info" in path:
            return {"code": 200, "msg": "success", "data": {
                "status": "SUCCESS" if done else "FIRST_SUCCESS",
                "response": {"sunoData": [{"id": "a1", "audio_url": "https://x/c1.mp3", "title": "t"},
                                          {"id": "a2", "audio_url": "https://x/c2.mp3", "title": "t"}]}}}
        raise AssertionError(url)


def _suno(http, **cfg):
    s = SunoApi({"apiKey": "k", **cfg}, http=http, sleep=lambda s: None)
    s._download = lambda ctx, url, dest: (dest.write_bytes(b"audio"), dest)[1]
    return s


def test_sunoapi_transcribe_local_audio(tmp_path, audio):
    http = FakeSuno()
    s = _suno(http)
    ctx = Context(tmp_path / "w")
    ctx.workdir.mkdir()
    out = s.run(Task(Capability.TRANSCRIBE, [Artifact("audio", audio)]), ctx)
    urls = [c[1].split("?")[0].replace("https://api.sunoapi.org", "") for c in http.calls]
    assert urls[0].endswith("/api/file-stream-upload")
    assert urls[1:] == ["/api/v1/vocal-removal/generate", "/api/v1/vocal-removal/record-info",
                        "/api/v1/vocal-removal/record-info", "/api/v1/midi/generate",
                        "/api/v1/midi/record-info", "/api/v1/midi/record-info"]
    sep_body = http.calls[1][2]
    assert sep_body["audioUrl"] == "https://tmp/song.wav" and sep_body["type"] == "separate_vocal"
    assert sep_body["callBackUrl"].startswith("https://")
    assert http.calls[4][2]["taskId"] == "vocal-removal-task"
    smf = read_smf(out[0].path)
    assert [c.name for c in smf.chunks[1:]] == ["Drums", "Electric Bass (finger)"]
    assert smf.streams["t1c9"].isDrum
    bass = smf.streams["t2c0"].notes[0]
    assert (bass.start, bass.end, bass.velocity) == (480, 960, 64)   # 120 BPM、480 PPQ：0.5 秒 = 480 tick
    assert (ctx.workdir / "raw" / "midi-poll.json").exists()        # 原始响应留档


def test_sunoapi_cover_uses_upload_and_returns_remote_refs(tmp_path, audio):
    http = FakeSuno()
    s = _suno(http, model="V6_MINI")
    ctx = Context(tmp_path / "w")
    ctx.workdir.mkdir()
    out = s.run(Task(Capability.COVER, [Artifact("audio", audio)], {"style": "lofi", "title": None}), ctx)
    body = http.calls[1][2]
    assert body["uploadUrl"] == "https://tmp/song.wav" and body["model"] == "V6_MINI" and body["style"] == "lofi"
    assert "title" not in body
    assert [a.remote["audioId"] for a in out] == ["a1", "a2"]
    # 再拿翻唱结果去分离：不重新上传，用 taskId + audioId
    http.calls.clear()
    s.run(Task(Capability.SEPARATE, [out[0]]), ctx)
    assert not any(c[1].endswith("file-stream-upload") for c in http.calls)
    assert (http.calls[0][2]["taskId"], http.calls[0][2]["audioId"]) == ("generate-task", "a1")
    assert "audioUrl" not in http.calls[0][2]


def test_sunoapi_needs_key():
    assert SunoApi({}).problems()
    assert not SunoApi({"apiKey": "k"}).problems()


def test_sunoapi_error_code_raises(tmp_path, audio):
    s = _suno(lambda *a, **k: {"code": 430, "msg": "insufficient credits"})
    ctx = Context(tmp_path)
    with pytest.raises(PipelineError, match="上传失败|430"):
        s.run(Task(Capability.COVER, [Artifact("audio", audio)], {"style": "x"}), ctx)


# ---- worker 协议 ----

def _worker(tmp_path):
    out = io.StringIO()
    providers = {"muscriptor": _named(Fake(), MuScriptor)}
    w = Worker(out, Config(), runner_factory=lambda on_event, cancel: Runner(
        cache=Cache(tmp_path / "cache"), on_event=on_event, cancel=cancel, providers=providers))
    return w, out


def test_worker_transcribe(tmp_path, audio):
    w, out = _worker(tmp_path)
    reqs = [{"id": 1, "method": "hello"}, {"id": 2, "method": "presets"},
            {"id": 3, "method": "transcribe", "params": {"audio": str(audio), "preset": "string-quartet",
                                                          "outDir": str(tmp_path / "run")}}]
    w.serve(io.StringIO("\n".join(json.dumps(r) for r in reqs) + "\n"))
    msgs = [json.loads(l) for l in out.getvalue().splitlines()]
    assert msgs[0] == {"id": 1, "result": {"protocol": 1}}
    assert any(p["id"] == "pop-band" for p in msgs[1]["result"])
    job = [m for m in msgs if m["id"] == 3]
    assert job[0]["event"] == "step"
    assert Path(job[-1]["result"]["midi"]).exists()


def test_worker_reports_errors_without_dying(tmp_path):
    w, out = _worker(tmp_path)
    w.serve(io.StringIO('not json\n{"id": 5, "method": "nope"}\n'
                        '{"id": 6, "method": "transcribe", "params": {"audio": "x.wav", "preset": "no-such"}}\n'))
    msgs = {m["id"]: m for m in map(json.loads, out.getvalue().splitlines())}
    assert msgs[None]["error"]["type"] == "badRequest"
    assert msgs[5]["error"]["type"] == "badRequest"
    assert "error" in msgs[6]


def test_worker_cancel(tmp_path, audio):
    started, release = threading.Event(), threading.Event()

    class Slow(Fake):
        def run(self, task, ctx):
            started.set()
            while not ctx.cancelled:
                release.wait(0.01)
            ctx.check_cancelled()

    out = io.StringIO()
    w = Worker(out, Config(), runner_factory=lambda on_event, cancel: Runner(
        cache=Cache(tmp_path / "cache"), on_event=on_event, cancel=cancel, providers={"muscriptor": Slow()}))
    w.handle(json.dumps({"id": 1, "method": "transcribe", "params": {
        "audio": str(audio), "preset": "orchestra", "outDir": str(tmp_path / "run")}}))
    assert started.wait(5)
    w.handle(json.dumps({"id": 2, "method": "cancel", "params": {"id": 1}}))
    for t in w.threads:
        t.join(5)
    msgs = [json.loads(l) for l in out.getvalue().splitlines()]
    assert {"id": 2, "result": {"cancelled": True}} in msgs
    assert any(m.get("id") == 1 and m.get("error", {}).get("type") == "cancelled" for m in msgs)
    assert not list((tmp_path / "cache").glob("*/*.partial"))      # 半截的结果不留在缓存里
