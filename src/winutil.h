// Small Windows helpers shared by the Windows platform files.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <string>

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
inline std::string wide_to_utf8(const wchar_t* w, size_t len) {
    if (!len) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, s.data(), n, nullptr, nullptr);
    return s;
}
inline std::string wide_to_utf8(const std::wstring& w) { return wide_to_utf8(w.data(), w.size()); }
#endif
