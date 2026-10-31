"""最小乐器库：音域（实际音高，MIDI 号，60 = 中央 C）、所属组、总谱顺序、轨名关键词。

音域是常见配器教材里的通行范围，按“舒适”和“极限”两档粗略取值，未逐条核对任何
单一出处；接到真实工程后按需要调。移调乐器这里一律写实际音高。
"""
from __future__ import annotations

import re
from dataclasses import dataclass

# 组：显示名、总谱里的先后
GROUPS: dict[str, tuple[str, int]] = {
    "woodwinds": ("木管", 10),
    "brass": ("铜管", 20),
    "percussion": ("打击乐", 30),
    "keyboards": ("键盘与竖琴", 40),
    "guitars": ("吉他与贝斯", 45),
    "chinese": ("民族乐器", 47),
    "voices": ("人声", 50),
    "strings": ("弦乐", 60),
    "other": ("其他", 90),
}

GROUP_COLOURS: dict[str, str] = {
    "woodwinds": "#4caf7a",
    "brass": "#e0a030",
    "percussion": "#9e8a78",
    "keyboards": "#8c6fd6",
    "guitars": "#d16b9a",
    "chinese": "#c9a227",
    "voices": "#4fb3c8",
    "strings": "#d9604c",
    "other": "#808a99",
}

UNASSIGNED_GROUP = "未归类"


@dataclass(frozen=True)
class Instrument:
    id: str
    nameZh: str
    group: str
    order: int                   # 组内顺序
    comfort: tuple[int, int]
    limit: tuple[int, int]
    keywords: tuple[str, ...] = ()   # 轨名正则（小写、已规整空白），按表顺序匹配，先到先得
    programs: tuple[int, ...] = ()   # GM 音色号（0 起）
    hasRange: bool = True            # 鼓：不做音域/八度判断


_I = Instrument
INSTRUMENTS: list[Instrument] = [
    # 木管 —— 更具体的名字放在前面（低音单簧管先于单簧管、英国管先于圆号）
    _I("piccolo", "短笛", "woodwinds", 1, (76, 103), (74, 108), (r"piccolo(?! trumpet)", r"\bpicc?\b", r"ottavino", r"短笛"), (72,)),
    _I("flute", "长笛", "woodwinds", 2, (62, 93), (59, 96), (r"\bflute?s?\b", r"\bflauti?\b", r"\bfl\b", r"\bfl\.?\s?\d", r"长笛"), (73,)),
    _I("english_horn", "英国管", "woodwinds", 4, (55, 79), (52, 81), (r"english horn", r"cor anglais", r"\bc\.?\s?a\.?\b", r"englischhorn", r"英国管"), (69,)),
    _I("oboe", "双簧管", "woodwinds", 3, (60, 86), (58, 91), (r"\boboe?s?\b", r"\bob\b", r"\bob\.?\s?\d", r"双簧管"), (68,)),
    _I("bass_clarinet", "低音单簧管", "woodwinds", 6, (38, 70), (34, 75), (r"bass clarinet", r"\bb\.?\s?cl", r"bassklarinette", r"低音单簧管"), ()),
    _I("clarinet", "单簧管", "woodwinds", 5, (52, 86), (50, 91), (r"clarinet", r"klarinette", r"\bcl\b", r"\bcl\.?\s?\d", r"\bclar\b", r"单簧管", r"黑管"), (71,)),
    _I("contrabassoon", "低音大管", "woodwinds", 8, (24, 50), (22, 53), (r"contra\s?bassoon", r"contrafag", r"kontrafagott", r"\bcbsn\b", r"低音大管"), ()),
    _I("bassoon", "大管", "woodwinds", 7, (36, 70), (34, 75), (r"bassoon", r"fagott?o?", r"\bbsn\b", r"\bfg\b", r"大管", r"巴松"), (70,)),
    _I("soprano_sax", "高音萨克斯", "woodwinds", 11, (58, 85), (56, 87), (r"soprano sax", r"高音萨克斯"), (64,)),
    _I("alto_sax", "中音萨克斯", "woodwinds", 12, (51, 78), (49, 80), (r"alto sax", r"中音萨克斯"), (65,)),
    _I("tenor_sax", "次中音萨克斯", "woodwinds", 13, (46, 73), (44, 75), (r"tenor sax", r"次中音萨克斯"), (66,)),
    _I("baritone_sax", "上低音萨克斯", "woodwinds", 14, (38, 66), (36, 68), (r"bari(tone)? sax", r"上低音萨克斯"), (67,)),
    # 铜管
    _I("horn", "圆号", "brass", 1, (41, 72), (34, 77), (r"\bhorns?\b", r"french horn", r"\bcorni?\b", r"\bhrn?\b", r"\bhn\.?\s?\d", r"\bhorn\s?\d", r"圆号"), (60,)),
    _I("trumpet", "小号", "brass", 2, (58, 79), (54, 82), (r"trumpets?", r"\btpts?\b", r"\btrp\b", r"\btpt?\.?\s?\d", r"\btrombe\b", r"小号"), (56, 59)),
    _I("bass_trombone", "低音长号", "brass", 4, (36, 62), (34, 67), (r"bass trombone", r"\bb\.?\s?tbn", r"bassposaune", r"低音长号"), ()),
    _I("trombone", "长号", "brass", 3, (43, 67), (40, 72), (r"trombones?", r"\btbn\b", r"\btbn\.?\s?\d", r"\btb\b", r"posaune", r"tromboni", r"长号"), (57,)),
    _I("tuba", "大号", "brass", 5, (29, 58), (26, 65), (r"\btubas?\b", r"大号"), (58,)),
    # 打击乐（定音鼓有音高，其余按鼓处理）
    _I("timpani", "定音鼓", "percussion", 1, (40, 57), (38, 60), (r"timpani", r"\btimp\b", r"pauken", r"定音鼓"), (47,)),
    _I("glockenspiel", "钟琴", "percussion", 3, (79, 108), (79, 108), (r"glockenspiel", r"\bglock\b", r"钟琴"), (9,)),
    _I("xylophone", "木琴", "percussion", 4, (65, 108), (65, 108), (r"xylophone", r"\bxylo\b", r"木琴"), (13,)),
    _I("marimba", "马林巴", "percussion", 5, (45, 96), (36, 96), (r"marimba", r"马林巴"), (12,)),
    _I("vibraphone", "颤音琴", "percussion", 6, (53, 89), (53, 89), (r"vibraphone", r"\bvibes?\b", r"颤音琴"), (11,)),
    _I("taiko", "太鼓", "percussion", 8, (0, 127), (0, 127), (r"taiko", r"太鼓"), (116,), False),
    _I("drums", "鼓", "percussion", 9, (0, 127), (0, 127), (r"\bdrums?\b", r"\bkit\b", r"percussion", r"\bperc\b", r"架子鼓", r"打击乐"), (), False),
    # 键盘与竖琴
    _I("harp", "竖琴", "keyboards", 1, (24, 103), (23, 104), (r"\bharps?\b", r"\barpa\b", r"harfe", r"竖琴"), (46,)),
    _I("celesta", "钢片琴", "keyboards", 2, (60, 108), (60, 108), (r"celest[ae]", r"钢片琴"), (8,)),
    _I("music_box", "八音盒", "keyboards", 4, (60, 96), (53, 103), (r"music ?box", r"八音盒"), (10,)),
    _I("piano", "钢琴", "keyboards", 3, (21, 108), (21, 108), (r"piano", r"klavier", r"\bpno\b", r"钢琴"), ()),
    # 吉他与贝斯
    _I("guitar", "吉他", "guitars", 1, (40, 84), (40, 88), (r"guitar", r"\bgtr\b", r"吉他"), tuple(range(24, 32))),
    _I("bass_guitar", "贝斯", "guitars", 2, (28, 60), (28, 67), (r"bass guitar", r"e-?bass", r"electric bass", r"\bbass gtr\b", r"贝斯"), (32, 33, 34, 35, 36, 37)),
    # 民族乐器（音域按常见规格粗取）
    _I("guzheng", "古筝", "chinese", 1, (38, 86), (38, 86), (r"guzheng", r"\bzheng\b", r"古筝"), (107,)),
    _I("pipa", "琵琶", "chinese", 2, (45, 86), (45, 89), (r"\bpipa\b", r"琵琶"), (105,)),
    _I("yangqin", "扬琴", "chinese", 3, (43, 88), (43, 91), (r"yangqin", r"扬琴"), ()),
    _I("dizi", "笛子", "chinese", 4, (64, 96), (60, 98), (r"\bdizi\b", r"笛子"), ()),
    _I("erhu", "二胡", "chinese", 5, (62, 93), (62, 98), (r"\berhu\b", r"二胡"), (110,)),
    # 人声
    _I("soprano", "女高音", "voices", 1, (60, 81), (59, 88), (r"^soprano$", r"\bsopranos?\b(?! sax)", r"女高音"), ()),
    _I("alto", "女中音", "voices", 2, (55, 74), (53, 77), (r"^altos?$", r"\bcontralto\b", r"女中音"), ()),
    _I("tenor", "男高音", "voices", 3, (48, 69), (47, 72), (r"^tenors?$", r"男高音"), ()),
    _I("bass_voice", "男低音", "voices", 4, (41, 62), (40, 64), (r"^bass(es)? voice", r"\bbaritone\b(?! sax)", r"男低音"), ()),
    # 弦乐
    _I("violin", "小提琴", "strings", 1, (55, 93), (55, 103), (r"violins?", r"violini", r"violon\b", r"geigen?", r"\bvln?s?\b", r"\bvln?\.?\s?(\d|i+)\b", r"\bvn\.?\s?(\d|i+)\b", r"\bvn\b", r"小提琴"), (40, 110)),
    _I("viola", "中提琴", "strings", 2, (48, 84), (48, 91), (r"violas?\b", r"viole\b", r"bratsche", r"\bvla\b", r"\bva\b", r"中提琴"), (41,)),
    _I("cello", "大提琴", "strings", 3, (36, 76), (36, 84), (r"cellos?", r"celli", r"violoncell", r"\bvc\b", r"\bvcl?\b", r"大提琴"), (42,)),
    _I("string_section", "弦乐组", "strings", 5, (36, 93), (28, 103), (r"\bstrings?\b", r"string ensemble", r"弦乐"), (44, 45, 48, 49)),
    _I("contrabass", "低音提琴", "strings", 4, (28, 60), (24, 67), (r"contra\s?bass(?!oon)", r"double bass", r"kontrabass", r"\bbasses\b", r"\bbassi\b", r"\bcb\b", r"\bdb\b", r"低音提琴"), (43,)),
]

BY_ID: dict[str, Instrument] = {i.id: i for i in INSTRUMENTS}
_COMPILED = [(i, [re.compile(k) for k in i.keywords]) for i in INSTRUMENTS]


def normalize_name(name: str) -> str:
    s = name.lower()
    s = re.sub(r"[_\-–—/|·•:()\[\]{}]+", " ", s)
    return re.sub(r"\s+", " ", s).strip()


def match_by_name(track_name: str) -> Instrument | None:
    s = normalize_name(track_name)
    if not s:
        return None
    for inst, pats in _COMPILED:
        if any(p.search(s) for p in pats):
            return inst
    return None


def match_by_program(program: int | None) -> Instrument | None:
    if program is None or program == 0:   # 0 是大多数宿主的默认值，不当证据
        return None
    for inst in INSTRUMENTS:
        if program in inst.programs:
            return inst
    return None


def guess(track_name: str, program: int | None, is_drum: bool) -> tuple[Instrument | None, str | None]:
    """返回 (乐器, 来源)。来源："drumChannel" | "name" | "program" | None。"""
    if is_drum:
        return BY_ID["drums"], "drumChannel"
    inst = match_by_name(track_name)
    if inst is not None:
        return inst, "name"
    inst = match_by_program(program)
    if inst is not None:
        return inst, "program"
    return None, None


# 下列来源的乐器判断，足以让依赖它的规则自动应用；"program" 来源只给到“待确认”
TRUSTED_INSTRUMENT_SOURCES = {"user", "name", "drumChannel"}


def group_name(group_key: str) -> str:
    return GROUPS.get(group_key, (group_key, 99))[0]


def score_order_key(inst: Instrument | None) -> tuple[int, int]:
    if inst is None:
        return (GROUPS["other"][1], 0)
    return (GROUPS[inst.group][1], inst.order)
