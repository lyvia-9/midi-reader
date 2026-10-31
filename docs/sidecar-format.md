# sidecar 格式 v1

sidecar 是放在 MIDI 旁边的一个 JSON 文件：`song.mid` → `song.mid.reader.json`。
它存“怎么看这个 MIDI”：轨道怎么组成逻辑声部、清洗规则得出了什么结论、用户改过什么。
**原 MIDI 一个字节不改**；原 MIDI + sidecar 经过投影，就得到画布要的逻辑声部（`LaneModel`）。

C++ 那边直接读这个文件，格式不改（见接口设计文档“合体”第 1 步）。

## 1. 先定义清楚“原始音符”

sidecar 里的一切都靠两种 id 回指原文件：**stream id** 和 **sourceNote**。两边必须按同一套规则从字节里得到它们。

| 概念 | 定义 |
|---|---|
| 块序号 `chunk` | 文件里第几个 `MTrk` 块，从 0 起。其他类型的块跳过，不占序号 |
| 通道 `channel` | 0–15（DAW 里显示的通道号 = channel + 1） |
| stream | 一个 (块, 通道) 上的全部音符。id 写作 `t{chunk}c{channel}`，如 `t3c0` |
| `sourceNote` | 按 (块序, 块内事件序) 遍历全文件时，第几个 velocity > 0 的 note-on，从 0 起 |

配对规则（与 JUCE `MidiMessageSequence::updateMatchedPairs` 的处理一致）：

- note-off，或 velocity = 0 的 note-on，结束同一 (通道, 音高) 上正在响的音
- 同一 (通道, 音高) 还在响又来一个 note-on：旧音在这个 tick 结束，再开新音
- 找不到对应 note-on 的 note-off：丢弃，计数
- 块结束时还没关的音：结束于该块最后一个事件（通常是 End of Track）的 tick
- running status 在 meta / sysex 之后仍然有效（宽松读法）
- 只支持 PPQ 计时；SMPTE 计时的文件直接报错

音色 `program`：该 stream 第一个音符处生效的 program change（同块同通道里找）；第一个音之前没设过，就取这一路里第一次设的；一次都没设过为 `null`。

全曲长度 `documentLength` = 所有块最后一个事件 tick 的最大值。

参考实现：`reader_proto/smf.py`。测试样例 `tests/cases/parse_edges` 覆盖了上面每一条。

## 2. 文件结构

```jsonc
{
  "format": "midi-reader-sidecar",
  "version": 1,
  "source":    { ... },   // 写 sidecar 时原文件的指纹
  "streams":   [ ... ],   // 写 sidecar 时原文件里有哪些 stream（只作参考，投影不读）
  "lanes":     [ ... ],   // 逻辑声部：映射
  "decisions": [ ... ],   // 清洗规则的结论
  "noteOverrides": { ... } // 逐音修正
}
```

### source

```json
{ "fileName": "song.mid", "sha256": "…", "format": 1, "ppq": 480, "noteCount": 12345 }
```

打开时 sha256 对不上（MIDI 被重新导出过），照样按 stream id 和 sourceNote 套用，并给出警告。

### lanes —— 逻辑声部

数组顺序 = 画布上从上到下的顺序。

```json
{
  "id": "L3",
  "name": "小提琴 I",
  "group": "弦乐",
  "colour": "#d9604c",
  "role": "unknown",
  "instrument": "violin",
  "instrumentSource": "name",
  "range": { "comfortLow": 55, "comfortHigh": 93, "limitLow": 55, "limitHigh": 103 },
  "sources": ["t2c0", "t3c0"]
}
```

| 字段 | 说明 |
|---|---|
| `id` | 稳定 id，`L` + 数字；画布、待确认队列、以后的备注都用它 |
| `name` `group` `colour` `role` | 直接成为 `LaneInfo` 的同名字段。`group` 是显示名，空串 = 不分组；`role` 取 `unknown/melody/harmony/bass/rhythm` |
| `instrument` | 乐器库 id，或 `null` |
| `instrumentSource` | 乐器从哪来：`user` 用户指定 / `name` 轨名 / `program` 音色号 / `drumChannel` 第 10 通道 / `null`。决定依赖乐器的规则能不能自动应用 |
| `range` | 直接成为 `LaneInfo` 的四个音域字段（实际音高）。**存在 sidecar 里**，所以投影不需要乐器库，用户也可以逐条改 |
| `sources` | 这条声部由哪些 stream 组成。几条轨合成一个声部 = 列几个；一条轨按通道拆成几个声部 = 分在几条 lane 里 |

约束：一个 stream 最多属于一条 lane。没被任何 lane 引用的 stream 不会消失，投影时放进“未归类”（见下）。

### decisions —— 规则结论

```json
{
  "id": "ks:t2c0:low",
  "rule": "keySwitch",
  "target": "t2c0",
  "status": "auto",
  "params": { "side": "low", "pitches": [12, 14, 16] },
  "evidence": { "gap": 48, "lead": 6, "notes": 6, "...": "..." },
  "reason": "音高在极端音区；在小提琴演奏极限之外；…"
}
```

| `rule` | `id` | `target` | `params` |
|---|---|---|---|
| `channelSplit` | `split:t{chunk}` | 块 `t{chunk}` | `streams`: 这一块里有音符的 stream |
| `keySwitch` | `ks:{stream}:low` / `ks:{stream}:high` | stream | `side`, `pitches`: 判为 keyswitch 的原始音高 |
| `octaveShift` | `oct:{stream}` | stream | `shift`: ±12 / ±24，加在原始音高上 |

`status`：

| 值 | 谁写的 | 投影时 |
|---|---|---|
| `auto` | 规则，有把握 | 应用 |
| `review` | 规则，没把握 | 不应用，相关音符标 `noteFlagReview` |
| `confirmed` | 用户 | 应用 |
| `rejected` | 用户 | 不应用，也不标黄 |

重新分析时，`confirmed` / `rejected` 原样保留（包括 params），`auto` / `review` 重新算。`evidence` 和 `reason` 只给人看，投影不读。

### noteOverrides —— 逐音修正

```json
{ "1234": { "keyswitch": false } }
```

键是 sourceNote（字符串）。目前只有 `keyswitch`：`true` 强制当 keyswitch，`false` 强制当普通音，都优先于规则结论。以后加别的逐音修正（换声部、备注）也放这里。

## 3. 投影：原 MIDI + sidecar → LaneModel

对 `lanes` 里的每一条，按顺序；然后为每个没被引用的 stream 追加一条“未归类”lane（id `U:{stream}`，group `未归类`，音域 0–127）。

对 lane 里每个 stream 的每个原始音符：

1. 是否 keyswitch：逐音修正优先；否则看音高是否在该 stream 已应用的 keyswitch 结论的 `pitches` 里
2. 是 keyswitch → `pitch` 不变，flags = `noteFlagKeySwitch`，不再做下面几步
3. 否则：
   - 有已应用的 `octaveShift` → `pitch += shift`；结果超出 0–127 就不平移，标 `noteFlagReview`
   - 音高在待确认的 keyswitch 结论里 → `noteFlagReview`
   - 有待确认的 `octaveShift`，且原始音高在 lane 演奏极限外 → `noteFlagReview`
   - 平移后的音高超出 `comfortLow..comfortHigh` → `noteFlagSoftRange`；超出 `limitLow..limitHigh` → `noteFlagHardRange`
4. 所在块的 `channelSplit` 结论待确认 → `noteFlagReview`（keyswitch 也标）
5. 产出 `LaneNote{start, length, pitch, velocity, sourceNote, flags}`，时间是原始 tick

每条 lane 的音符按 (start, pitch, sourceNote) 升序。`LaneInfo.sourceTrackCount` = 这条 lane 的 stream 来自几个不同的块。

参考实现：`reader_proto/project.py`。

## 4. 投影结果文件（测试用）

`*.lanes.json` 是投影的输出，用来做“输入 MIDI → 期望结果”的对照，C++ 投影层要逐条对上。画布本身不读它。

```jsonc
{
  "format": "midi-reader-lanes", "version": 1,
  "unitsPerQuarter": 480,
  "documentLength": 37900,
  "meterChanges": [{ "tick": 0, "numerator": 4, "denominator": 4 }],  // 字段名待对照 pianoroll::MeterChange
  "warnings": [],
  "lanes": [{
    "id": "L1",
    "info": { "name": "...", "group": "...", "colour": "#rrggbb", "role": "unknown", "sourceTrackCount": 1,
              "comfortLow": 36, "comfortHigh": 76, "limitLow": 36, "limitHigh": 84 },
    "longestNote": 460,
    "notes": [[start, length, pitch, velocity, sourceNote, flags], ...]
  }]
}
```

`info` 的键与 `reader::LaneInfo` 成员同名；每个音符数组的顺序与 `reader::LaneNote` 成员顺序相同；flags 位值与 `reader::NoteFlag` 相同（1 超舒适、2 超极限、4 待确认、8 keyswitch）。
