"""从 proto/reader_proto/instruments.py 生成 src/core/InstrumentTable.inc（C++ 乐器表）。

乐器库以 Python 版为准；改了它就跑一次：python tools/gen_instruments.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "proto"))
from reader_proto import instruments as I  # noqa: E402


def cstr(s: str) -> str:
    return 'R"re(' + s + ')re"'


lines = ["// 自动生成：python tools/gen_instruments.py（来源 proto/reader_proto/instruments.py），不要手改", ""]
lines.append("static const GroupDef kGroups[] = {")
for key, (name, order) in I.GROUPS.items():
    lines.append(f'    {{"{key}", u8"{name}", {order}, "{I.GROUP_COLOURS[key]}"}},')
lines.append("};")
lines.append("")
lines.append("static const InstrumentDef kInstruments[] = {")
for i in I.INSTRUMENTS:
    kws = ", ".join(cstr(k) for k in i.keywords)
    progs = ", ".join(str(p) for p in i.programs)
    lines.append(f'    {{"{i.id}", u8"{i.nameZh}", "{i.group}", {i.order}, {i.comfort[0]}, {i.comfort[1]}, '
                 f'{i.limit[0]}, {i.limit[1]}, {{{kws}}}, {{{progs}}}, {"true" if i.hasRange else "false"}}},')
lines.append("};")
lines.append(f'static const char* const kUnassignedGroup = u8"{I.UNASSIGNED_GROUP}";')
(ROOT / "src" / "core" / "InstrumentTable.inc").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("wrote src/core/InstrumentTable.inc")
