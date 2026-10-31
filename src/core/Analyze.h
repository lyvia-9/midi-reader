// 原始 MIDI → sidecar：建初始声部映射、跑规则（keyswitch ⇄ 八度互相喂），以及用户修正。
// 对照 proto/reader_proto/analyze.py。
#pragma once
#include "core/Sidecar.h"
#include "core/Smf.h"

#include <optional>
#include <string>
#include <vector>

namespace reader::core {

Sidecar analyze(const SmfFile&, const Sidecar* existing = nullptr, bool reset = false);

// 用户操作：都在原 sidecar 上改，然后重跑规则，返回新 sidecar。找不到 id 时抛 std::out_of_range。
Sidecar applyUserDecision(const SmfFile&, Sidecar, const std::string& decisionId, const std::string& verdict);
Sidecar resetUserDecision(const SmfFile&, Sidecar, const std::string& decisionId);
Sidecar setLaneInstrument(const SmfFile&, Sidecar, const std::string& laneId, const std::string& instrumentId);
Sidecar mergeLanes(const SmfFile&, Sidecar, const std::vector<std::string>& laneIds, const std::string& name);
Sidecar renameLane(const SmfFile&, Sidecar, const std::string& laneId, const std::string& name);
// 把一个 stream 从所在声部拆出来，单独成一条（放在原声部后面）
Sidecar splitOutStream(const SmfFile&, Sidecar, const std::string& streamId);
// 把一个 stream 挪到另一条声部
Sidecar moveStream(const SmfFile&, Sidecar, const std::string& streamId, const std::string& toLaneId);
Sidecar moveLane(const SmfFile&, Sidecar, const std::string& laneId, int newIndex);
Sidecar setNoteKeySwitch(const SmfFile&, Sidecar, const std::vector<std::uint32_t>& sourceNotes, std::optional<bool> keyswitch);

std::string shadeColour(const std::string& hex, int index);

} // namespace reader::core
