// 三条基础清洗规则，逐行对照 proto/reader_proto/rules/*.py。阈值与说明见 docs/rules.md。
#pragma once
#include "core/Instruments.h"
#include "core/Sidecar.h"
#include "core/Smf.h"

#include <map>
#include <optional>
#include <vector>

namespace reader::core::rules {

std::vector<Decision> channelSplit(const SmfFile&);

// forced：sourceNote → 用户逐音指定的 keyswitch 真假（不参与统计）
std::vector<Decision> keySwitch(const Stream&, int ppq, const Instrument* inst, const std::string& instSource,
                                int octaveShift, const std::map<std::uint32_t, bool>& forced);

std::optional<Decision> octaveShift(const Stream&, const Instrument* inst, const std::string& instSource,
                                    const std::vector<RawNote>& playing);

} // namespace reader::core::rules
