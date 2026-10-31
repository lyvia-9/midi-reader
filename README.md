# MIDI Reader

“看得懂乐队”的 MIDI 阅读器：拖一个 MIDI 进来，它把轨道理成逻辑声部、认出乐器、排成总谱的样子，并把常见的脏数据（keyswitch、音源八度偏移、一轨多通道）整理好。只看不改，原 MIDI 不动，整理结果存在旁边的 sidecar 文件里。

- 用户说明：[`docs/使用说明.md`](docs/使用说明.md)
- sidecar 格式与投影算法：[`docs/sidecar-format.md`](docs/sidecar-format.md)
- 三条清洗规则：[`docs/rules.md`](docs/rules.md)

GPL-3.0（链接 [pianoroll-core](https://github.com/rmkstudio/pianoroll-core)）。

## 结构

| 目录 | 内容 |
|---|---|
| `src/core/` | 纯 C++17，无 JUCE：SMF 读取、乐器库、sidecar、三条规则、映射、投影、tempo map |
| `src/app/` | JUCE 7 独立应用：多轨只读画布 `MultiLaneCanvas`、左栏、播放、只读细看窗口 |
| `proto/` | Python 原型（A 线）：规则的参考实现，改规则先在这里改 |
| `testdata/cases/` | “输入 MIDI → 期望结果”样例，Python 和 C++ 两边都跑 |
| `tests/ParityTests.cpp` | C++ 核心与 Python 原型逐字节对照 |
| `tools/gen_instruments.py` | 从 Python 乐器库生成 C++ 乐器表 |

## 编译

Windows 安装包由 GitHub Actions 自动编（`.github/workflows/build.yml`），在仓库的 Actions 页面下载 `MIDI-Reader-windows`。

本机编译需要 CMake 3.22+、C++17 编译器。JUCE 7.0.12 和 pianoroll-core（`codex/pianoroll-view-0.5.2` 分支的 `db18fea`）会自动下载；已有本地副本时可以指定：

```bash
cmake -S . -B build -DJUCE_DIR=/path/to/JUCE -DPIANO_ROLL_CORE_DIR=/path/to/pianoroll-core
```

```bash
cmake --build build --config Release
```

只编核心和对照测试（不需要 JUCE）：

```bash
cmake -S . -B build-core -DMIDI_READER_BUILD_APP=OFF
```

```bash
cmake --build build-core && ./build-core/ParityTests testdata/cases
```

## 改规则的流程

1. 在 `proto/reader_proto/` 里改 Python 版，加或改 `testdata/cases/` 的样例，`cd proto && python -m pytest`
2. `python proto/tests/golden.py --update` 更新期望结果
3. 照着改 `src/core/` 的 C++ 版；乐器库改了就跑 `python tools/gen_instruments.py`
4. `ParityTests` 全过才算搬对
