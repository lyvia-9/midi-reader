"""第零步：数据格式。

与《多轨只读画布 接口设计》“头文件草稿”里的 C++ 定义一一对应：
    reader::NoteFlag / LaneNote / LaneRole / LaneInfo
任何一边要加字段、改字段，先改那份文档里的定义，再同步这里和 C++。
JSON 里的键名与 C++ 成员名保持一致（camelCase）。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, IntFlag


class NoteFlag(IntFlag):
    """可叠加；由投影层/清洗规则算好，画布只上色。位值与 C++ 相同。"""
    NONE = 0
    SOFT_RANGE = 1 << 0   # noteFlagSoftRange 超出舒适音域
    HARD_RANGE = 1 << 1   # noteFlagHardRange 超出演奏极限
    REVIEW = 1 << 2       # noteFlagReview    清洗规则存疑，待确认
    KEY_SWITCH = 1 << 3   # noteFlagKeySwitch 判为 keyswitch，默认不画


@dataclass(frozen=True)
class LaneNote:
    start: int            # 原始 tick（C++ 里是 double，值总是整数）
    length: int           # 原始 tick
    pitch: int
    velocity: int
    sourceNote: int       # 原 MIDI 音符编号，定义见 smf.py / docs/sidecar-format.md
    flags: int = NoteFlag.NONE

    def to_json(self) -> list:
        # 紧凑数组，顺序 = C++ 成员顺序：start, length, pitch, velocity, sourceNote, flags
        return [self.start, self.length, self.pitch, self.velocity, self.sourceNote, int(self.flags)]

    @staticmethod
    def from_json(a: list) -> "LaneNote":
        return LaneNote(*a)

    @property
    def end(self) -> int:
        return self.start + self.length


class LaneRole(str, Enum):
    UNKNOWN = "unknown"
    MELODY = "melody"
    HARMONY = "harmony"
    BASS = "bass"
    RHYTHM = "rhythm"


@dataclass
class LaneInfo:
    name: str
    group: str = ""               # 空 = 不分组
    colour: str = "#808080"       # "#RRGGBB"
    role: LaneRole = LaneRole.UNKNOWN
    sourceTrackCount: int = 1     # 合并了几条原始轨（按 MTrk 块计，不按通道）
    comfortLow: int = 0
    comfortHigh: int = 127
    limitLow: int = 0
    limitHigh: int = 127

    def to_json(self) -> dict:
        return {
            "name": self.name,
            "group": self.group,
            "colour": self.colour,
            "role": self.role.value,
            "sourceTrackCount": self.sourceTrackCount,
            "comfortLow": self.comfortLow,
            "comfortHigh": self.comfortHigh,
            "limitLow": self.limitLow,
            "limitHigh": self.limitHigh,
        }

    @staticmethod
    def from_json(d: dict) -> "LaneInfo":
        d = dict(d)
        d["role"] = LaneRole(d.get("role", "unknown"))
        return LaneInfo(**d)


@dataclass
class Lane:
    """一条逻辑声部的投影结果 = LaneModel 对一条 lane 能回答的全部内容。"""
    id: str
    info: LaneInfo
    notes: list[LaneNote] = field(default_factory=list)

    @property
    def longestNote(self) -> int:
        return max((n.length for n in self.notes), default=0)


def note_sort_key(n: LaneNote) -> tuple[int, int, int]:
    """LaneModel 要求按 start 升序；同 start 再按 pitch、sourceNote，保证两边结果逐条一致。"""
    return (n.start, n.pitch, n.sourceNote)
