# MIDI 阅读器 · A 线：Python 清洗原型

> 现在是规则的参考实现：改规则先在这里改，再搬到 `../src/core/`，见上一级 README。以下命令都在 `proto/` 目录里运行。

《多轨只读画布 接口设计》“开工顺序与验收”里的 A 线：吃进一个 MIDI，输出 sidecar 和一个“一个逻辑声部一条轨”的检查用 MIDI，用来验证三条基础清洗规则。
**只在开发机上用，不进安装包。** 这里定稿的 sidecar 格式和测试样例，是合体时 C++ 要对齐的东西。

## Windows 最快上手

1. 装 Python 3.10 以上（python.org 下载，安装时勾上 “Add python.exe to PATH”）
2. 双击 `setup.bat`（只需一次，会在本目录建 `.venv` 并装好依赖）
3. 把一个或几个 `.mid` 文件拖到 `run.bat` 上

输出文件写在 MIDI 旁边，终端里会打印报告。下面是命令行的完整用法。

## 装

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install symusic mido pytest
```

Windows 上把 `.venv/bin/` 换成 `.venv\Scripts\`。

## 用

```bash
.venv/bin/python -m reader_proto run song.mid
```

在 `song.mid` 旁边写出三个文件：

| 文件 | 是什么 |
|---|---|
| `song.mid.reader.json` | sidecar：声部映射、规则结论、逐音修正。原 MIDI 不动 |
| `song.lanes.json` | 投影结果：每条逻辑声部的 `LaneInfo` 和 `LaneNote`，格式与画布的数据接口一致 |
| `song.lanes.mid` | 检查用 MIDI：一个逻辑声部一条轨，轨名 = 声部名 |

同时在终端打印每条声部和每条规则结论（带 id 和理由）。再跑一次 `run` 会读回已有的 sidecar：你整理过的映射和确认/否决过的结论都不会被覆盖。

其他命令：

```bash
.venv/bin/python -m reader_proto report song.mid                          # 只看结论，不写文件
.venv/bin/python -m reader_proto decide song.mid ks:t2c0:low confirm      # 或 reject / reset
.venv/bin/python -m reader_proto note song.mid 1234 1240 --as not-ks      # 逐音：ks / not-ks / clear
.venv/bin/python -m reader_proto instrument song.mid L3 cello             # 给声部指定乐器（none 清除）
.venv/bin/python -m reader_proto merge song.mid L4 L5 --name "小提琴 I"     # 几条声部合成一条
.venv/bin/python -m reader_proto run song.mid --reset                     # 重建映射（保留确认/否决和逐音修正）
```

改完后再 `run` 一次，重新生成投影 JSON 和检查 MIDI。

## 在 FL Studio 里检查

把 `song.lanes.mid` 拖进 FL（作为新工程导入），逐条看：

- **合并 / 拆分**：每条轨是不是一个完整的声部？按通道拆开的有没有拆错，该合的有没有合上
- **去 keyswitch**：被判成 keyswitch 的音不在声部轨里，而是放在紧跟其后的 `<声部名> · KS` 轨。看 KS 轨里有没有误伤的正常音，声部轨里有没有漏网的 keyswitch
- **八度修正**：声部轨的音高已经是修正后的，听一下八度对不对
- 终端报告里标“待确认”的，在 FL 里定下来后用 `decide` 确认或否决

## 测试样例

```bash
.venv/bin/python -m pytest -q
```

`../testdata/cases/<名字>/` 每个目录是一个“输入 MIDI → 期望结果”样例：`input.mid`、可选的 `input.reader.json`（你已有的 sidecar），以及 `expected.reader.json` / `expected.lanes.json`。现有 11 个是合成样例（`tests/make_cases.py` 生成），每个只考一件事，`case.json` 里写着考什么。

**加真实工程的样例**：

1. 建 `../testdata/cases/<名字>/`，把 FL 导出的 MIDI 放进去，命名为 `input.mid`
2. 跑 `run`，在 FL 里核对 `*.lanes.mid`；有不对的，用 `decide` / `note` / `merge` / `instrument` 改到对，或者把规则改好
3. 改出来的 sidecar 如果要作为样例的一部分，复制成 `input.reader.json`
4. `.venv/bin/python tests/golden.py --update <名字>`，把当前结果钉成期望

以后改规则时，`pytest` 会把所有样例的变化以 diff 的形式列出来。合体时，C++ 拿同一批样例各跑一次，结果一致才算搬对。

## 音频管线（B 线）

`reader_proto/pipeline/`：音频 → 分离 → 扒谱 → 按编制预设整理成 MIDI + sidecar，以及调 sunoapi.org 做 remix。设计和格式见 [`../docs/audio-pipeline.md`](../docs/audio-pipeline.md)。

模型都通过 [uv](https://docs.astral.sh/uv/) 的 `uvx` 在各自的隔离环境里跑，本目录的 `.venv` 不用装 torch。第一次跑会下载模型环境，比较慢。

```bash
.venv/bin/python -m reader_proto.pipeline providers                          # 有哪些 provider、缺什么
```

```bash
.venv/bin/python -m reader_proto.pipeline transcribe song.wav --preset pop-band
```

结果在 `song.wav.pipeline/<时间>-…/` 里：`song.mid`（阅读器打开这个）、`song.mid.reader.json`、`run.json`（来源记录），以及每一步的产物。API key 和 HF token 写在 `providers.json` 里，见设计文档第 8 节。

## 文档

- [`docs/sidecar-format.md`](docs/sidecar-format.md)：sidecar 格式 v1；“原始音符”和 sourceNote 的精确定义；投影算法（C++ 投影层照这个实现）
- [`docs/rules.md`](docs/rules.md)：三条规则的定义、阈值、各证据防的是什么、已知局限

## 代码

| 文件 | 内容 |
|---|---|
| `reader_proto/model.py` | 第零步：`LaneNote` / `LaneInfo` / `NoteFlag` / `LaneRole`，与接口设计文档的 C++ 草稿逐字段对应 |
| `reader_proto/smf.py` | 规范 SMF 读取器，定义 stream 和 sourceNote |
| `reader_proto/instruments.py` | 最小乐器库：音域、组、总谱顺序、轨名关键词、GM 音色号 |
| `reader_proto/rules/` | 三条规则 |
| `reader_proto/analyze.py` | 建初始映射、跑规则（keyswitch ⇄ 八度互相喂），以及用户修正后的重算 |
| `reader_proto/project.py` | 投影：原 MIDI + sidecar → 逻辑声部 |
| `reader_proto/export.py` | 写检查用 MIDI（用 symusic） |
| `reader_proto/pipeline/` | 音频管线：`base.py` 接口，`providers/` 各家模型和服务，`presets.py` 编制预设，`run.py` 配方，`merge.py` 拼分轨 MIDI，`worker.py` stdio 协议和阅读器用的单次模式 |
| `pipeline_worker.py` | 阅读器启动管线的入口（`python pipeline_worker.py job request.json`），见 `docs/audio-pipeline.md` 第 11 节 |

为什么读 MIDI 不用 symusic：symusic 载入时按 (轨, 通道, 音色) 重新分组，丢掉了原始轨号和通道号，没法做“按通道拆轨”，sourceNote 也对不上 C++ 那边的 JUCE `MidiFile`。所以读取用了一个两百行的规范解析器，symusic 用来写导出文件，并在测试里交叉校验音符是否一致。
