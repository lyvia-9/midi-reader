"""规则二：识别 keyswitch。

对每个 stream 的低端（和高端）各找一次“候选簇”：一小撮音高，和正常演奏的音之间隔着
一段空档。再看三个证据决定 auto / review / 不报：

- position：候选全在极端音区（低端 ≤ 23，高端 ≥ 108），或乐器已知且候选全在乐器演奏极限之外
- lead：候选音按下后 4 拍内（或提前不超过 1/8 拍）有正常音符出现的比例——keyswitch 总是为后面的音服务
- doubled：候选音和正常音构成八度叠加（同音名、起音收音都对得上）的比例
- inline：候选音“接在旋律线上”的比例——起音时恰好有音结束、收音时恰好有音起，而起音时
  没有正常音一起起。
  写过了音域的旋律音是这样；keyswitch 是额外加的，起音时总伴着正常音起音或落在休止里——钢琴低音八度会大量出现，keyswitch 几乎不会

整数比较，便于 C++ 逐位复现。
"""
from __future__ import annotations

import bisect
from collections import Counter

from ..instruments import TRUSTED_INSTRUMENT_SOURCES, Instrument
from ..sidecar import STATUS_AUTO, STATUS_REVIEW, Decision
from ..smf import RawNote, Stream

MIN_STREAM_NOTES = 4
MAX_CLUSTER_PITCHES = 12      # 一套 keyswitch 很少超过一个八度
GAP_CANDIDATE = 5             # 不靠乐器音域时，候选簇和正常音之间至少空 5 个半音
GAP_STRONG = 7                # auto 需要的空档（乐器音域已证明在音域外时不要求）
GAP_INSTRUMENT = 2            # 靠乐器音域找候选时的最小空档
INSTRUMENT_MAX_SHARE = (1, 4) # 靠乐器音域找候选时，候选音最多占这一路的 25%（再多更像整体八度错位）
LOW_EXTREME = 23              # ≤ B0（C4=60 记法；pianoroll-core 的 C5 记法里是 B1）
HIGH_EXTREME = 108            # ≥ C8
LEAD_WINDOW_QUARTERS = 4
TOL_DIVISOR = 8               # 容差 = ppq / 8
AUTO_LEAD = (3, 5)            # lead ≥ 60%
AUTO_DOUBLED = (1, 5)         # doubled ≤ 20%
REVIEW_DOUBLED = (1, 2)       # doubled ≤ 50%
REVIEW_LEAD = (1, 2)          # review 也要 lead ≥ 50%：keyswitch 总得为后面的音服务
MIN_NOTES_NOT_EXTREME = 3     # 不在极端音区的候选至少 3 个音才报（极端音区里一个就够：开头切一次很常见）
GAP_INSIDE_INSTRUMENT = 12    # 乐器已知、候选又在它能演奏的音域里：空档至少一个八度才报
AUTO_INLINE = (1, 5)          # inline ≤ 20%
REVIEW_INLINE = (1, 2)        # inline ≤ 50%


def _cluster_by_gap(pitches: list[int], counts: Counter, total: int) -> tuple[list[int], int] | None:
    """pitches 升序去重。返回最低端的候选簇和空档；取最大空档，并列取更小的簇。"""
    best: tuple[list[int], int] | None = None
    running = 0
    for i in range(len(pitches) - 1):
        running += counts[pitches[i]]
        if i + 1 > MAX_CLUSTER_PITCHES or running * 2 >= total:
            break
        gap = pitches[i + 1] - pitches[i]
        if gap >= GAP_CANDIDATE and (best is None or gap > best[1]):
            best = (pitches[: i + 1], gap)
    return best


def _cluster_by_instrument(pitches: list[int], counts: Counter, total: int, bound: int) -> tuple[list[int], int] | None:
    """乐器已知：所有低于 bound（已换算到原始音高的演奏下限）的音高作为候选。"""
    lower = [p for p in pitches if p < bound]
    if not lower or len(lower) == len(pitches) or len(lower) > MAX_CLUSTER_PITCHES:
        return None
    if sum(counts[p] for p in lower) * INSTRUMENT_MAX_SHARE[1] > total * INSTRUMENT_MAX_SHARE[0]:
        return None
    gap = pitches[len(lower)] - lower[-1]
    return (lower, gap) if gap >= GAP_INSTRUMENT else None


def _doubles(k: RawNote, q: RawNote, tol: int) -> bool:
    """八度叠加：同音名，起音和收音都对得上（收音容差取 tol 与较长音长 1/4 的较大者）。"""
    if k.pitch % 12 != q.pitch % 12:
        return False
    end_tol = max(tol, max(k.length, q.length) // 4)
    return abs(k.start - q.start) <= tol and abs(k.end - q.end) <= end_tol


def _ratio_at_least(num: int, den: int, frac: tuple[int, int]) -> bool:
    return num * frac[1] >= den * frac[0]


def _ratio_at_most(num: int, den: int, frac: tuple[int, int]) -> bool:
    return num * frac[1] <= den * frac[0]


def analyze(stream: Stream, ppq: int, inst: Instrument | None, inst_source: str | None,
            octave_shift: int, forced: dict[int, bool]) -> list[Decision]:
    """forced：sourceNote -> 用户逐音指定的 keyswitch 真假，不参与统计。"""
    if stream.isDrum:
        return []
    notes = [n for n in stream.notes if n.sourceNote not in forced]
    if len(notes) < MIN_STREAM_NOTES:
        return []
    out = []
    for side in ("low", "high"):
        d = _analyze_side(stream, notes, ppq, inst, inst_source, octave_shift, side)
        if d is not None:
            out.append(d)
    return out


def _analyze_side(stream: Stream, notes: list[RawNote], ppq: int, inst: Instrument | None,
                  inst_source: str | None, shift: int, side: str) -> Decision | None:
    # 高端：把音高镜像（127 - p）后复用低端逻辑
    mirror = side == "high"

    def key(p: int) -> int:
        return 127 - p if mirror else p

    counts = Counter(key(n.pitch) for n in notes)
    pitches = sorted(counts)
    total = len(notes)
    has_range = inst is not None and inst.hasRange
    # 乐器演奏极限换回原始音高、镜像后的下界
    bound = None
    if has_range:
        bound = (127 - (inst.limit[1] - shift)) if mirror else (inst.limit[0] - shift)

    found = _cluster_by_gap(pitches, counts, total)
    via = "gap"
    if found is None and bound is not None:
        found = _cluster_by_instrument(pitches, counts, total, bound)
        via = "instrument"
    if found is None:
        return None
    cluster, gap = found
    cset = set(cluster)
    ks = [n for n in notes if key(n.pitch) in cset]
    play = [n for n in notes if key(n.pitch) not in cset]

    tol = max(1, ppq // TOL_DIVISOR)
    starts = [n.start for n in play]          # stream.notes 已按起音排列
    lead = 0
    for k in ks:
        i = bisect.bisect_left(starts, k.start - tol)
        if i < len(starts) and starts[i] <= k.start + LEAD_WINDOW_QUARTERS * ppq:
            lead += 1
    doubled = 0
    for k in ks:
        lo = bisect.bisect_left(starts, k.start - tol)
        hi = bisect.bisect_right(starts, k.start + tol)
        if any(_doubles(k, play[j], tol) for j in range(lo, hi)):
            doubled += 1

    # 前一个音可以是任何音（连续几个出界音也算在线上），但必须比 k 先起音（不能是 k 自己）
    ends = sorted((n.end, n.start) for n in notes)
    all_starts = [n.start for n in notes]
    inline = 0
    for k in ks:
        lo = bisect.bisect_left(starts, k.start - tol)
        starts_with = lo < len(starts) and starts[lo] <= k.start + tol
        e = bisect.bisect_left(ends, (k.start - tol, -1))
        ends_at = False
        while e < len(ends) and ends[e][0] <= k.start + tol:
            if ends[e][1] < k.start:
                ends_at = True
                break
            e += 1
        # 收音时恰好有下一个音起（不能是 k 自己）
        a = bisect.bisect_left(all_starts, k.end - tol)
        next_at = False
        while a < len(all_starts) and all_starts[a] <= k.end + tol:
            if all_starts[a] > k.start:
                next_at = True
                break
            a += 1
        if ends_at and next_at and not starts_with:
            inline += 1

    real = sorted(127 - c for c in cluster) if mirror else list(cluster)
    extreme = all(p >= HIGH_EXTREME for p in real) if mirror else all(p <= LOW_EXTREME for p in real)
    outside_inst = bound is not None and all(c < bound for c in cluster)
    trusted = inst_source in TRUSTED_INSTRUMENT_SOURCES
    position = extreme or (outside_inst and trusted)
    n = len(ks)
    lead_ok = _ratio_at_least(lead, n, AUTO_LEAD)
    doubled_ok = _ratio_at_most(doubled, n, AUTO_DOUBLED)
    inline_ok = _ratio_at_most(inline, n, AUTO_INLINE)
    gap_ok = gap >= GAP_STRONG or (outside_inst and trusted)

    # 真实工程里的误报（liubian-full-v3）：钢琴左手 2 个 A1、钢片琴 3 个低一点的旋律音、
    # 小提琴 1 个高音、Pad 2 个低音，全被报成待确认。这些音都在乐器音域内、音数很少。
    if not extreme:
        if n < MIN_NOTES_NOT_EXTREME:
            return None
        if has_range and not outside_inst and gap < GAP_INSIDE_INSTRUMENT:
            return None

    if position and gap_ok and lead_ok and doubled_ok and inline_ok:
        status = STATUS_AUTO
    elif ((gap >= GAP_STRONG or outside_inst or extreme)
          and _ratio_at_most(doubled, n, REVIEW_DOUBLED) and _ratio_at_most(inline, n, REVIEW_INLINE)
          and _ratio_at_least(lead, n, REVIEW_LEAD)):
        status = STATUS_REVIEW
    else:
        return None

    why = []
    if extreme:
        why.append("音高在极端音区")
    if outside_inst:
        why.append(f"在{inst.nameZh}演奏极限之外")
    why.append(f"与正常音相隔 {gap} 个半音")
    why.append(f"{lead}/{n} 个后面紧跟正常音")
    if doubled:
        why.append(f"{doubled}/{n} 个与正常音八度叠加")
    if inline:
        why.append(f"{inline}/{n} 个接在旋律线上")
    if status == STATUS_REVIEW:
        missing = []
        if not position:
            missing.append("位置不够极端" + ("（乐器只从音色号猜出，不作数）" if outside_inst and not trusted else ""))
        if not gap_ok:
            missing.append("空档不够大")
        if not lead_ok:
            missing.append("和后面的音符关系不明显")
        if not doubled_ok:
            missing.append("八度叠加偏多")
        if not inline_ok:
            missing.append("像是旋律里的音")
        why.append("没把握：" + "、".join(missing))

    return Decision(
        id=f"ks:{stream.id}:{side}",
        rule="keySwitch",
        target=stream.id,
        status=status,
        params={"side": side, "pitches": real},
        evidence={
            "via": via, "gap": gap, "notes": n, "lead": lead, "doubled": doubled, "inline": inline,
            "extreme": extreme, "outsideInstrument": outside_inst,
            "instrument": inst.id if inst else None, "instrumentSource": inst_source,
        },
        reason="；".join(why),
    )
