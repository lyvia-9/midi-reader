#include "core/TextCodec.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#include <cerrno>
#endif

namespace reader::core {

bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        int n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        for (int k = 1; k <= n; ++k) {
            if (i + k >= s.size()) return false;
            const auto d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF
            || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n + 1;
    }
    return true;
}

static bool decodeGbk(const std::string& raw, std::string& out) {
#if defined(_WIN32)
    const int wlen = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring w((std::size_t)wlen, L'\0');
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), w.data(), wlen);
    const int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    out.assign((std::size_t)ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, out.data(), ulen, nullptr, nullptr);
    return true;
#else
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) return false;
    std::string in = raw;
    out.assign(in.size() * 4 + 4, '\0');
    char* inp = in.data();
    std::size_t inleft = in.size();
    char* outp = out.data();
    std::size_t outleft = out.size();
    const auto r = iconv(cd, &inp, &inleft, &outp, &outleft);
    iconv_close(cd);
    if (r == (std::size_t)-1 || inleft != 0) return false;
    out.resize(out.size() - outleft);
    return true;
#endif
}

static std::string latin1ToUtf8(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static std::string strip(std::string s) {
    auto isNul = [](char c) { return c == '\0'; };
    while (!s.empty() && isNul(s.front())) s.erase(s.begin());
    while (!s.empty() && isNul(s.back())) s.pop_back();
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    std::size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string decodeText(const std::string& raw) {
    if (isValidUtf8(raw)) return strip(raw);
    std::string gbk;
    if (decodeGbk(raw, gbk)) return strip(gbk);
    return strip(latin1ToUtf8(raw));
}

} // namespace reader::core
