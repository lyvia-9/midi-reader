#pragma once
#include <string>

namespace reader::core {
// 轨名解码：UTF-8 → GBK（中文 Windows 工程常见）→ latin-1 保底；去掉首尾 NUL 和空白。结果是 UTF-8。
std::string decodeText(const std::string& raw);
bool isValidUtf8(const std::string&);
} // namespace reader::core
