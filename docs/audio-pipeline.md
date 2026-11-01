# 音频管线（B 线）设计

把音频变成阅读器能读的 MIDI，以及调闭源服务做 remix：

```
音频 ─┬─ 全曲路线：MuScriptor（以预设里的乐器列表作条件）─────────────┐
      │                                                              ├─→ MIDI + sidecar → 现有的声部整理、规则、读谱
      ├─ 分轨路线：Demucs 分离 → 每轨 Basic Pitch（鼓用 MuScriptor）→ 拼 ┘
      └─ remix：sunoapi.org 翻唱 → 新音频（可以接着走上面任一路线）
```

原型在 `proto/reader_proto/pipeline/`，预设在仓库根的 `presets/`。和 A 线一样，先在 Python 里定接口，阅读器以后通过进程外协议调用，不把模型链接进程序。

## 1. 边界

- **模型一律在别的进程或远程服务里跑。** 阅读器（GPL-3.0、C++）只通过 stdio 上的 JSON 和管线说话；管线再起外部命令或发 HTTP。好处：阅读器不背 torch，几个 GB 的环境可以不装；授权边界清楚；本地模型和远程 API 是同一个接口。
- **“原 MIDI 不动”照旧成立。** 扒出来的 MIDI 就是运行目录里的“原 MIDI”，整理结果写在它旁边的 sidecar 里。
- **sidecar 格式不改（仍是 v1）。** 来源信息写在同目录的 `run.json`，见第 6 节。

## 2. provider 接口

一个 provider 是“能做几件事的一个模型或服务”。代码：`pipeline/base.py`。

### 能力

| 能力 | 输入 → 输出 | 现在谁会 |
|---|---|---|
| `separate` | 音频 → 若干分轨音频（`meta.stem`） | demucs、sunoapi |
| `transcribe` | 音频（+ 可选乐器列表）→ MIDI | muscriptor、basic-pitch、sunoapi |
| `cover` | 音频 + 风格 → 新音频（remix） | sunoapi |
| `generate` / `extend` | 预留 | — |

### 清单（Manifest）

| 字段 | 含义 |
|---|---|
| `id`、`version` | `version` 进缓存键：本地工具钉死版本（`muscriptor@0.4.0`），换版本就不复用旧结果 |
| `kind` | `local`（起外部命令）或 `remote`（HTTP） |
| `capabilities` | 上表 |
| `instruments` | `transcribe` 能指定的乐器词表；非空时配方会把预设里的乐器传进去 |
| `stems` | `separate` 能出的分轨名 |
| `remoteInput` | 只吃本 provider 自己产出的远程结果的能力（预留） |
| `adapters` | 能否加载微调权重 / LoRA（第 10 节） |

### 产物（Artifact）

`kind`（`audio` / `midi` / `json`）＋本地 `path` ＋ `meta`（如 `{"stem": "bass"}`）＋可选的 `remote`（如 `{"provider": "sunoapi", "taskId": "…", "audioId": "…"}`）。带 `remote` 的产物交回同一个服务时不用重新上传：sunoapi 翻唱的结果拿去分离，直接传 `taskId + audioId`。

### Provider 要实现的

- `problems()`：跑不了的原因（缺命令、缺 key），不联网、无副作用。配方在跑之前先查，缺东西就直接报“不可用”。
- `cache_identity()`：影响结果的配置（模型大小等），进缓存键。**不放密钥**，也不放 `device` 这类不影响结果的配置。
- `run(task, ctx)`：在 `ctx.workdir` 里把活干完，返回产物。`ctx.progress()` 报进度，`ctx.cancelled` 查取消，`ctx.save_raw()` 给远程 API 的原始响应留档（解析对不上时有据可查）。

加一个新 provider：写一个 `Provider` 子类，在 `pipeline/providers/__init__.py` 里登记。

## 3. 内置 provider

| id | 能力 | 默认命令 / 接口 | 实测（2026-11，Linux CPU） | 注意 |
|---|---|---|---|---|
| `muscriptor` | transcribe | `uvx muscriptor@0.4.0 transcribe … --instruments a,b` | 跑到下载权重为止 | 权重在 Hugging Face 上是 gated：先在模型页同意许可，再填 `hfToken`。CC BY-NC 4.0 |
| `demucs` | separate | `uvx --python 3.12 --with numpy --with soundfile demucs@4.1.0 -n htdemucs_6s` | 跑通 | Python 3.13 下缺 numpy，所以钉 3.12。6 轨版的 piano、guitar 质量一般 |
| `basic-pitch` | transcribe | `uvx --python 3.11 --with "numpy<2" --with "setuptools<81" basic-pitch@0.4.0` | 跑通 | TensorFlow 不兼容 numpy 2；不扒鼓 |
| `sunoapi` | cover、separate、transcribe | api.sunoapi.org，见第 9 节 | 用假 HTTP 测过，没用真 key 跑 | 按次扣点数，服务端不缓存 |
| `fake` | 全部 | 不跑模型 | — | 测试和界面联调用 |

本地的三个都走 `uvx`，用户机器上只要装了 [uv](https://docs.astral.sh/uv/)，第一次跑会自动建好各自的隔离环境。要换成自己装好的环境，在配置里改 `command`。

## 4. 编制预设

一份预设说清楚“这首曲子有哪些声部”，管线和读谱两头都用它。代码：`pipeline/presets.py`；文件：`presets/*.json`。

```jsonc
{
  "format": "midi-reader-preset", "version": 1,
  "id": "pop-band", "name": "流行乐队",
  "route": "stems",            // 默认路线：full = 全曲扒谱；stems = 先分离再逐轨扒
  "separator": "demucs",       // 分轨路线用谁分离
  "transcriber": "muscriptor", // 全曲路线用谁扒
  "slots": [
    { "lane": "贝斯",                          // 声部名（画布上显示的）
      "instrument": "bass_guitar",             // 乐器库 id；可空
      "group": null,                           // instrument 为空时用：乐器库的组 key（voices、strings…）
      "role": "bass",                          // unknown / melody / harmony / bass / rhythm
      "muscriptor": ["electric_bass", "acoustic_bass"],  // 全曲路线：告诉 MuScriptor 曲中有这些乐器
      "stem": "bass",                          // 分轨路线：要哪一轨
      "transcriber": "basic-pitch",            // 分轨路线：这一轨用谁扒（鼓写 muscriptor）
      "match": ["^(?!.*drum).*bass"] }         // 认别家扒出来的轨名（小写正则）
  ]
}
```

内置三份：

| id | 路线 | 声部 |
|---|---|---|
| `pop-band` | stems | 主唱、吉他、键盘、贝斯、鼓 |
| `string-quartet` | full | 小提琴、中提琴、大提琴（两把小提琴会扒进同一轨，要在阅读器里手动拆） |
| `orchestra` | full | 木管四件、铜管四件、定音鼓、竖琴、弦乐五部（开源分离模型分不出管弦乐各组，所以只走全曲） |

### 套到 sidecar 上

扒完先照常跑 `analyze`（建声部、跑三条规则），再按预设整理：

1. **认领**：每条声部看它来源轨的轨名。先认确切的名字（声部名本身，或 MuScriptor 的轨名，即组名把下划线换成空格，如 `string ensemble`），都不中再试 `match` 正则；按槽位顺序，先到先得
2. **同一槽位认领到几条声部，就并成一条**（比如 MuScriptor 分别扒出的木吉他和电吉他都落到“吉他”）
3. 改名、定乐器、定职能。乐器来源记 `"user"`：预设是用户选的，规则会据此自动应用
4. 按槽位顺序排列；没被认领的声部原样留在后面
5. 乐器变了的声部，依赖它的自动结论作废，再跑一次 `analyze`

预设的 `muscriptor` 字段只能写 MuScriptor 0.4.0 的 35 个乐器组名（`providers/muscriptor.py` 的 `INSTRUMENTS`，和 `muscriptor list-instruments` 一致）；载入时会校验。

## 5. 配方

代码：`pipeline/run.py`。

**全曲**：一步 `transcribe`，provider 带乐器词表时传 `instruments = 预设里所有 muscriptor 名字`。

**分轨**：一步 `separate`（只要预设里用到的分轨），然后每个槽位一步 `transcribe`，最后拼成一个 MIDI，一个声部一块，块名就是声部名。

拼接要处理时间基准不同（`pipeline/merge.py`）：

- Basic Pitch：固定 tempo，tick 0 = 音频 0 秒
- MuScriptor：按检测到的节拍写 tempo map。为了让小节线落在强拍上，它会把开头往后挪，挪了多少记在 marker `muscriptor:bar_offset=<秒>` 里（在 muscriptor 0.4.0 的 `tokenizer/notes.py` 里确认过）

所以先把所有音符换回“音频秒”，再按主文件的 tempo map 写回 tick。主文件优先用带节拍信息的（MuScriptor 的），这样小节线是对的；都没有就用 120 BPM。

**remix**：一步 `cover`，产出两个版本（Suno 每次出两个）。命令行加 `--then-transcribe <预设>` 会接着把第一个版本扒成 MIDI。

## 6. 运行目录和 run.json

```
song.wav.pipeline/20261101-112159-transcribe-pop-band/
  run.json               来源记录
  01-separate/           每一步的产物（从缓存硬链接过来，不占双份空间）
  02-transcribe-vocals/
  …
  song.mid               最终 MIDI：阅读器打开这个
  song.mid.reader.json   按预设整理好的 sidecar（v1，格式不变）
```

```jsonc
{
  "format": "midi-reader-pipeline-run", "version": 1,
  "kind": "transcribe", "createdAt": "2026-11-01T11:21:59+00:00",
  "source": { "path": "/…/song.wav", "sha256": "…" },     // 远程输入时还有 "remote"
  "preset": "pop-band", "route": "stems",
  "steps": [
    { "id": "01-separate", "provider": "demucs", "version": "4.1.0", "capability": "separate",
      "params": { "stems": ["vocals", "guitar", "piano", "bass", "drums"] },
      "inputs": [ … ], "outputs": [ { "kind": "audio", "path": "01-separate/vocals.wav", "meta": { "stem": "vocals" } } ],
      "cacheKey": "…", "cached": false, "seconds": 7.9 }
  ],
  "result": { "midi": "song.mid", "sidecar": "song.mid.reader.json" }
}
```

**为什么来源信息不写进 sidecar**：Python 和 C++ 两边读 sidecar 时都只认 v1 的字段，写回时会丢掉不认识的字段；而升到 v2 要两边一起改、ParityTests 一起过。所以先放在同目录的 `run.json`。阅读器以后要显示“这个 MIDI 来自 song.wav，MuScriptor medium 扒的”，读它就行。

## 7. 缓存

分离和扒谱都慢，远程 API 还按次扣钱（sunoapi 的文档明确写了服务端不缓存），所以每一步先查缓存。代码：`pipeline/cache.py`。

- 键 = sha256(provider id + version + `cache_identity()` + 能力 + 参数 + 每个输入的指纹)。本地文件的指纹按内容哈希，远程结果按引用
- 条目是 `<缓存>/<键前两位>/<键>/` 目录，里面有 `result.json` 和产物文件
- 跑的时候写在 `<键>.partial/`，成功才改名，所以取消或失败留下的半截不会被当成命中

实测：同一个音频、同一个预设跑第二次，四步全部命中，0.16 秒。

## 8. 配置

`providers.json`，位置：环境变量 `MIDI_READER_CONFIG`；Windows 默认 `%APPDATA%\midi-reader\providers.json`，其他系统 `~/.config/midi-reader/providers.json`。缓存默认在 `%LOCALAPPDATA%\midi-reader\cache\pipeline`（或 `~/.cache/midi-reader/pipeline`），可以用 `cacheDir` 或 `MIDI_READER_CACHE` 改。

```json
{
  "providers": {
    "muscriptor": { "model": "medium", "hfToken": "hf_…", "quantize": true },
    "demucs": { "model": "htdemucs_6s" },
    "basic-pitch": { "onsetThreshold": 0.5 },
    "sunoapi": { "apiKey": "…", "model": "V6", "separateType": "separate_vocal", "pollSeconds": 15 }
  }
}
```

- 每个本地 provider 都可以用 `"command": [...]` 换掉默认命令。比如 Windows 上用 NVIDIA 显卡跑 MuScriptor：`["uvx", "--torch-backend=cu128", "muscriptor@0.4.0"]`
- `SUNOAPI_KEY` 环境变量会覆盖 `sunoapi.apiKey`
- 密钥只放这里或环境变量，不进 sidecar、不进 run.json、不进缓存键、不进仓库

## 9. sunoapi.org

照 docs.sunoapi.org 的 OpenAPI（`suno-api.json`、`file-upload-api.json`，2026-11 读）。所有请求带 `Authorization: Bearer <key>`；返回外层统一是 `{code, msg, data}`，`code` 为 200 才算成功。

| 用途 | 请求 | 完成判断 / 取结果 |
|---|---|---|
| 上传本地文件 | `POST sunoapiorg.redpandaai.co/api/file-stream-upload`（multipart：`file`、`uploadPath`） | `data.downloadUrl`；3 天后删 |
| 翻唱 | `POST /api/v1/generate/upload-cover`：`uploadUrl`（≤8 分钟）或 `taskId+audioId`，`model`，`style`… | `GET /api/v1/generate/record-info`：`status == SUCCESS`；`response.sunoData[].{id, audio_url}` |
| 分离 | `POST /api/v1/vocal-removal/generate`：`audioUrl`（≤20MB）或 `taskId+audioId`，`type` | `GET /api/v1/vocal-removal/record-info`：`successFlag == SUCCESS`；`response.{vocalUrl, bassUrl, …}` |
| 出 MIDI | `POST /api/v1/midi/generate`：`taskId`（分离任务），可选 `audioId` | `GET /api/v1/midi/record-info`：`successFlag` 0 等待 / 1 成功 / 2、3 失败；`midiData.instruments[].notes[].{pitch, start, end, velocity}` |
| 余额 | `GET /api/v1/generate/credit` | `data` = 剩余点数 |

实现上的几个决定：

- **一律轮询，不靠回调。** `callBackUrl` 是必填，但桌面程序没有公网地址，所以填一个不会解析的占位（`https://callback.invalid/…`，可以用 `callBackUrl` 配置改）
- **结果马上下载进缓存。** 生成的文件 14–15 天后删，上传的文件 3 天后删
- **出 MIDI 只吃分离任务**，所以本地音频要先分离再出 MIDI。文档说 `split_stem` 分离后 `midiData` 可能为空，所以默认用 `separate_vocal`（`separateType` 可改）
- **MIDI JSON 转 SMF**：时间是秒，力度是 0–1，没有节拍信息，所以按 120 BPM 写；名字里带 drum 的轨放第 10 通道。轨名照抄（如 `Electric Bass (finger)`），预设靠 `match` 正则认领
- **文档自己有不一致**：quickstart 写 `response.data`、`audioUrl`、`GENERATING`，OpenAPI 写 `response.sunoData`、`audio_url`。解析时两种都认，原始响应都存在步骤目录的 `raw/` 里
- 分离（`split_stem`）能分出 strings、brass、woodwinds，这是开源分离模型没有的。以后可以考虑让管弦乐预设也走 sunoapi 的分轨路线，但要先用真 key 看看效果

## 10. 微调 / LoRA（预留）

- 清单里有 `adapters` 字段，任务参数里有 `adapter` 字段
- MuScriptor 的 `--model` 接受本地 safetensors 路径或 `hf://` 地址，所以微调出来的完整权重现在就能接：任务参数 `adapter` 会传给 `--model`。它有没有 LoRA 形式的增量权重、怎么训练，还没看
- 训练做成单独的工具或 provider 能力（`train`），不放进阅读器

## 11. 阅读器怎么调：stdio JSON 行协议

阅读器启动 `python -m reader_proto.pipeline worker`（以后可以换成打包好的 exe），一行一个 JSON。代码：`pipeline/worker.py`。

```
→ {"id": 1, "method": "hello"}
← {"id": 1, "result": {"protocol": 1}}
→ {"id": 2, "method": "providers"}
← {"id": 2, "result": [{"id": "muscriptor", "capabilities": ["transcribe"], …, "problems": []}, …]}
→ {"id": 3, "method": "presets"}
→ {"id": 4, "method": "transcribe", "params": {"audio": "song.wav", "preset": "pop-band", "route": "stems"}}
← {"id": 4, "event": "step", "step": "01-separate", "provider": "demucs", "capability": "separate"}
← {"id": 4, "event": "progress", "step": "01-separate", "fraction": null, "message": "…"}
← {"id": 4, "result": {"run": "…", "midi": "…/song.mid", "sidecar": "…/song.mid.reader.json"}}
→ {"id": 5, "method": "cancel", "params": {"id": 4}}
← {"id": 5, "result": {"cancelled": true}}
← {"id": 4, "error": {"type": "cancelled", "message": "cancelled"}}
```

- `transcribe` 的参数：`audio`、`preset`（id 或 .json 路径），可选 `route`、`transcriber`、`separator`、`outDir`
- `cover` 的参数：`audio`，可选 `options`（`style`、`title`、`lyrics`、`instrumental`…）、`provider`、`outDir`
- 错误类型：`unavailable`（缺命令或缺 key）、`failed`、`cancelled`、`badRequest`、`internal`
- 每个任务一个线程，跑着也能收 `cancel`。stdout 只写协议，日志走 stderr

这一层以后也可以包成 MCP server，让 agent 直接驱动整条管线。

## 12. 命令行

都在 `proto/` 里跑：

```bash
.venv/bin/python -m reader_proto.pipeline providers
```

```bash
.venv/bin/python -m reader_proto.pipeline transcribe song.wav --preset pop-band
```

```bash
.venv/bin/python -m reader_proto.pipeline transcribe song.wav --preset orchestra --route full
```

```bash
.venv/bin/python -m reader_proto.pipeline cover song.wav --style "lofi jazz, rhodes" --then-transcribe pop-band
```

```bash
.venv/bin/python -m reader_proto.pipeline credits
```

## 13. 已知局限和下一步

- **扒谱出来的 MIDI 需要自己的一套规则。** 现有三条规则是为 DAW 导出的 MIDI 写的。实测里分轨串音的音符漏进贝斯轨，八度规则因此把整条贝斯降了一个八度。要补的：量化、去碎音和串音、同乐器多声部拆分（比如两把小提琴）
- **MuScriptor 还没真跑过**：要你的 HF token。拿到后先用 `pop-band` 和 `orchestra` 各跑一首真曲子
- **sunoapi 还没用真 key 跑过**：解析是照 OpenAPI 写的，第一次真跑后看 `raw/` 里的原始响应核对一遍
- 预设只能整轨认领，不能按音域或时间段拆分一条轨
- 阅读器这边（打开音频、选预设、看进度、取消）还没做，C++ 侧照第 11 节的协议接
