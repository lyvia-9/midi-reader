"""sunoapi.org：第三方 Suno 接口（用户自己的 key，自己充值）。

字段照 docs.sunoapi.org 的 OpenAPI（suno-api.json、file-upload-api.json，2026-11 读）：
  上传     POST {uploadBase}/api/file-stream-upload        multipart: file, uploadPath → data.downloadUrl（3 天后删）
  翻唱     POST /api/v1/generate/upload-cover              uploadUrl | taskId+audioId, model, style, title, …
           GET  /api/v1/generate/record-info?taskId=       data.status, data.response.sunoData[].{id, audio_url}
  分离     POST /api/v1/vocal-removal/generate             audioUrl(≤20MB) | taskId+audioId, type
           GET  /api/v1/vocal-removal/record-info?taskId=  data.successFlag, data.response.{vocalUrl, bassUrl, …, originData[]}
  出 MIDI  POST /api/v1/midi/generate                      taskId（分离任务）, audioId?（originData[].id）
           GET  /api/v1/midi/record-info?taskId=           data.successFlag 0/1/2/3, data.midiData.instruments[].notes[]
  余额     GET  /api/v1/generate/credit                    data = 剩余点数
所有请求 Authorization: Bearer <key>。外层统一 {code, msg, data}，code 200 才算成功。
callBackUrl 是必填，但桌面程序收不到回调，所以一律轮询；回调地址填一个不会解析的占位。
文档自己有不一致（quickstart 里写 response.data、status GENERATING），解析时两种都认。
每次调用都扣点数、服务端不缓存，所以结果全靠本地缓存复用。
"""
from __future__ import annotations

import json
import mimetypes
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path
from typing import Any, Callable

from ..base import (Artifact, Capability, Context, Manifest, PipelineError, Provider,
                    ProviderUnavailable, Task)
from ..midiwrite import TrackOut, seconds_to_ticks, write_smf

BASE_URL = "https://api.sunoapi.org"
UPLOAD_BASE_URL = "https://sunoapiorg.redpandaai.co"
PLACEHOLDER_CALLBACK = "https://callback.invalid/midi-reader"
DEFAULT_MODEL = "V6"
UPLOAD_LIMIT_SEPARATE = 20 * 1024 * 1024   # 分离接口 audioUrl 的上限

GENERATE_FAILED = {"CREATE_TASK_FAILED", "GENERATE_AUDIO_FAILED", "CALLBACK_EXCEPTION", "SENSITIVE_WORD_ERROR"}
SEPARATE_TYPES = ("separate_vocal", "split_stem", "split_stem_advanced")

# vocal-removal record-info 的 response 字段 → 分轨名
STEM_FIELDS = {
    "vocalUrl": "vocals", "instrumentalUrl": "instrumental", "backingVocalsUrl": "backing_vocals",
    "drumsUrl": "drums", "bassUrl": "bass", "guitarUrl": "guitar", "keyboardUrl": "keyboard",
    "percussionUrl": "percussion", "stringsUrl": "strings", "synthUrl": "synth", "fxUrl": "fx",
    "brassUrl": "brass", "woodwindsUrl": "woodwinds",
}

Http = Callable[..., dict]


class SunoApi(Provider):
    manifest = Manifest(
        id="sunoapi", version="api-v1", kind="remote",
        capabilities=(Capability.COVER, Capability.SEPARATE, Capability.TRANSCRIBE),
        description="sunoapi.org：翻唱/remix、最多 12 轨分离、分离后出 MIDI（按次扣点数）",
        stems=tuple(STEM_FIELDS.values()),
    )

    def __init__(self, config: dict[str, Any] | None = None, http: Http | None = None,
                 sleep: Callable[[float], None] = time.sleep):
        super().__init__(config)
        self._http = http or self._urllib
        self._sleep = sleep

    # ---- 配置 ----
    @property
    def key(self) -> str:
        return self.config.get("apiKey", "")

    @property
    def base(self) -> str:
        return self.config.get("baseUrl", BASE_URL).rstrip("/")

    @property
    def upload_base(self) -> str:
        return self.config.get("uploadBaseUrl", UPLOAD_BASE_URL).rstrip("/")

    def problems(self) -> list[str]:
        return [] if self.key else ["没有 API key：在 providers.json 的 sunoapi.apiKey 填，或设环境变量 SUNOAPI_KEY"]

    def cache_identity(self) -> dict:
        return {"model": self.config.get("model", DEFAULT_MODEL),
                "separateType": self.config.get("separateType", "separate_vocal")}

    # ---- HTTP ----
    def _urllib(self, method: str, url: str, body: dict | None = None,
                file: Path | None = None, fields: dict | None = None) -> dict:
        headers = {"Authorization": f"Bearer {self.key}", "Accept": "application/json"}
        data = None
        if file is not None:
            boundary = uuid.uuid4().hex
            data = _multipart(boundary, fields or {}, file)
            headers["Content-Type"] = f"multipart/form-data; boundary={boundary}"
        elif body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=float(self.config.get("timeoutSeconds", 60))) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")
            try:
                return json.loads(text)
            except ValueError:
                raise PipelineError(f"{method} {url}: HTTP {e.code} {text[:300]}") from None

    def _call(self, ctx: Context, name: str, method: str, url: str, **kw) -> Any:
        if not self.key:
            raise ProviderUnavailable(self.problems()[0])
        resp = self._http(method, url, **kw)
        ctx.save_raw(f"{name}.json", json.dumps(resp, ensure_ascii=False, indent=2))
        if resp.get("code") != 200:
            raise PipelineError(f"sunoapi {name}: code {resp.get('code')} {resp.get('msg')}")
        return resp.get("data")

    def _poll(self, ctx: Context, name: str, path: str, task_id: str, done: Callable[[dict], bool | None]) -> dict:
        """轮询 record-info；done 返回 True 完成、False 失败、None 继续等。"""
        interval = float(self.config.get("pollSeconds", 15))
        deadline = time.monotonic() + float(self.config.get("pollTimeoutSeconds", 1800))
        n = 0
        while True:
            ctx.check_cancelled()
            n += 1
            data = self._call(ctx, f"{name}-poll", "GET",
                              f"{self.base}{path}?{urllib.parse.urlencode({'taskId': task_id})}") or {}
            ok = done(data)
            if ok is True:
                return data
            if ok is False:
                raise PipelineError(f"sunoapi {name} 失败：{data.get('errorCode')} {data.get('errorMessage')}")
            if time.monotonic() > deadline:
                raise PipelineError(f"sunoapi {name} 等太久，任务 {task_id} 还没好")
            ctx.progress(None, f"{name}：等待中（第 {n} 次查询）")
            self._sleep(interval)

    def _download(self, ctx: Context, url: str, dest: Path) -> Path:
        ctx.check_cancelled()
        with urllib.request.urlopen(url, timeout=300) as r, open(dest, "wb") as f:
            while block := r.read(1 << 20):
                f.write(block)
        return dest

    def _upload(self, ctx: Context, path: Path) -> str:
        ctx.progress(None, f"上传 {path.name}")
        if not self.key:
            raise ProviderUnavailable(self.problems()[0])
        resp = self._http("POST", f"{self.upload_base}/api/file-stream-upload",
                          file=path, fields={"uploadPath": "midi-reader", "fileName": path.name})
        ctx.save_raw("upload.json", json.dumps(resp, ensure_ascii=False, indent=2))
        url = (resp.get("data") or {}).get("downloadUrl")
        if not resp.get("success", resp.get("code") == 200) or not url:
            raise PipelineError(f"sunoapi 上传失败：{resp.get('code')} {resp.get('msg')}")
        return url

    def _source(self, ctx: Context, art: Artifact, limit: int | None = None) -> dict:
        """输入 → 请求字段：自己产出的远程结果用 taskId+audioId，本地文件先上传。"""
        r = art.remote or {}
        if r.get("provider") == self.manifest.id and r.get("taskId") and r.get("audioId"):
            return {"taskId": r["taskId"], "audioId": r["audioId"]}
        if art.path is None:
            raise PipelineError("sunoapi：输入既不是本地文件，也不是 sunoapi 的结果")
        if limit is not None and Path(art.path).stat().st_size > limit:
            raise PipelineError(f"{Path(art.path).name} 超过 {limit // (1024 * 1024)}MB，先转成 mp3 或截短")
        return {"url": self._upload(ctx, Path(art.path))}

    # ---- 三件事 ----
    def run(self, task: Task, ctx: Context) -> list[Artifact]:
        if task.capability == Capability.COVER:
            return self._cover(task, ctx)
        if task.capability == Capability.SEPARATE:
            return self._separate(task, ctx)[0]
        if task.capability == Capability.TRANSCRIBE:
            return self._transcribe(task, ctx)
        raise PipelineError(f"sunoapi 不支持 {task.capability.value}")

    def _cover(self, task: Task, ctx: Context) -> list[Artifact]:
        src = self._source(ctx, task.inputs[0])
        body: dict[str, Any] = {"model": self.config.get("model", DEFAULT_MODEL),
                                "callBackUrl": self.config.get("callBackUrl", PLACEHOLDER_CALLBACK)}
        if "url" in src:
            body["uploadUrl"] = src["url"]
        else:
            body.update(src)
        for k in ("style", "title", "lyrics", "instrumental", "vocalGender", "styleWeight",
                  "weirdnessConstraint", "audioWeight", "variety", "negativeTags"):
            if task.params.get(k) is not None:
                body[k] = task.params[k]
        tid = self._call(ctx, "cover", "POST", f"{self.base}/api/v1/generate/upload-cover", body=body)["taskId"]
        ctx.progress(0.1, f"翻唱任务 {tid}")

        def done(d: dict) -> bool | None:
            s = d.get("status")
            return True if s == "SUCCESS" else False if s in GENERATE_FAILED else None

        data = self._poll(ctx, "cover", "/api/v1/generate/record-info", tid, done)
        resp = data.get("response") or {}
        items = resp.get("sunoData") or resp.get("data") or []
        out = []
        for i, it in enumerate(items):
            url = it.get("audio_url") or it.get("audioUrl") or it.get("source_audio_url")
            if not url:
                continue
            p = self._download(ctx, url, ctx.workdir / f"cover_{i + 1}{_ext(url, '.mp3')}")
            out.append(Artifact("audio", p, {"variant": i + 1, "title": it.get("title", "")},
                                {"provider": self.manifest.id, "taskId": tid, "audioId": it.get("id")}))
        if not out:
            raise PipelineError("sunoapi 翻唱成功了但没有音频链接，看 raw/cover-poll.json")
        return out

    def _separate(self, task: Task, ctx: Context) -> tuple[list[Artifact], str, dict]:
        stype = task.params.get("type") or self.config.get("separateType", "separate_vocal")
        if stype not in SEPARATE_TYPES:
            raise PipelineError(f"分离类型只能是 {', '.join(SEPARATE_TYPES)}")
        src = self._source(ctx, task.inputs[0], limit=UPLOAD_LIMIT_SEPARATE)
        body: dict[str, Any] = {"type": stype, "callBackUrl": self.config.get("callBackUrl", PLACEHOLDER_CALLBACK)}
        if "url" in src:
            body["audioUrl"] = src["url"]
        else:
            body.update(src)
        if stype == "split_stem_advanced":
            body["stemName"] = task.params["stemName"]
        tid = self._call(ctx, "separate", "POST", f"{self.base}/api/v1/vocal-removal/generate", body=body)["taskId"]
        ctx.progress(0.1, f"分离任务 {tid}（{stype}）")

        def done(d: dict) -> bool | None:
            s = d.get("successFlag")
            return True if s == "SUCCESS" else False if s in GENERATE_FAILED else None

        data = self._poll(ctx, "separate", "/api/v1/vocal-removal/record-info", tid, done)
        resp = data.get("response") or {}
        want = set(task.params.get("stems") or [])
        ids = {(o.get("stem_type_group_name") or "").lower().replace(" ", "_"): o.get("id")
               for o in resp.get("originData") or []}
        out = []
        for field, stem in STEM_FIELDS.items():
            url = resp.get(field)
            if not url or (want and stem not in want):
                continue
            p = self._download(ctx, url, ctx.workdir / f"{stem}{_ext(url, '.mp3')}")
            remote = {"provider": self.manifest.id, "kind": "separation", "taskId": tid}
            if ids.get(stem):
                remote["audioId"] = ids[stem]
            out.append(Artifact("audio", p, {"stem": stem}, remote))
        return out, tid, data

    def _transcribe(self, task: Task, ctx: Context) -> list[Artifact]:
        art = task.inputs[0]
        r = art.remote or {}
        if r.get("provider") == self.manifest.id and r.get("kind") == "separation":
            sep_tid, audio_id = r["taskId"], r.get("audioId")
        else:
            # 出 MIDI 只吃分离任务：本地音频先分离。文档说 split_stem 的 midiData 可能为空，所以默认 separate_vocal
            sep = Task(Capability.SEPARATE, [art], {"type": task.params.get("separateType")})
            _, sep_tid, _ = self._separate(sep, ctx)
            audio_id = None
        body: dict[str, Any] = {"taskId": sep_tid, "callBackUrl": self.config.get("callBackUrl", PLACEHOLDER_CALLBACK)}
        if audio_id:
            body["audioId"] = audio_id
        tid = self._call(ctx, "midi", "POST", f"{self.base}/api/v1/midi/generate", body=body)["taskId"]
        ctx.progress(0.6, f"MIDI 任务 {tid}")

        def done(d: dict) -> bool | None:
            s = d.get("successFlag")
            return True if s == 1 else False if s in (2, 3) else None

        data = self._poll(ctx, "midi", "/api/v1/midi/record-info", tid, done)
        instruments = ((data.get("midiData") or {}).get("instruments")) or []
        if not any(i.get("notes") for i in instruments):
            raise PipelineError("sunoapi 返回的 midiData 是空的（split_stem 分离时文档说可能这样），看 raw/midi-poll.json")
        out = ctx.workdir / "transcription.mid"
        midi_json_to_smf(instruments, out)
        return [Artifact("midi", out, {"instruments": [i.get("name", "") for i in instruments]},
                         {"provider": self.manifest.id, "kind": "midi", "taskId": tid})]

    def credits(self, ctx: Context) -> Any:
        return self._call(ctx, "credit", "GET", f"{self.base}/api/v1/generate/credit")


def midi_json_to_smf(instruments: list[dict], path: Path, ppq: int = 480) -> None:
    """midiData.instruments[] → SMF。时间是秒、力度 0–1；固定 120 BPM 写出（没有节拍信息）。"""
    tempos = [(0, 500_000)]
    tracks = []
    for inst in instruments:
        name = inst.get("name") or "?"
        notes = []
        for n in inst.get("notes") or []:
            s = seconds_to_ticks(tempos, ppq, float(n["start"]))
            e = max(s + 1, seconds_to_ticks(tempos, ppq, float(n["end"])))
            v = max(1, min(127, round(float(n.get("velocity", 0.8)) * 127)))
            notes.append((s, e, int(n["pitch"]), v))
        tracks.append(TrackOut(name, notes, drum="drum" in name.lower()))
    write_smf(path, tracks, ppq, tempos)


def _ext(url: str, default: str) -> str:
    suffix = Path(urllib.parse.urlparse(url).path).suffix
    return suffix if suffix and len(suffix) <= 5 else default


def _multipart(boundary: str, fields: dict, file: Path) -> bytes:
    parts = []
    for k, v in fields.items():
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode("utf-8"))
    ctype = mimetypes.guess_type(file.name)[0] or "application/octet-stream"
    parts.append((f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{file.name}"\r\n'
                  f"Content-Type: {ctype}\r\n\r\n").encode("utf-8"))
    parts.append(Path(file).read_bytes())
    parts.append(f"\r\n--{boundary}--\r\n".encode("utf-8"))
    return b"".join(parts)
