// MIT; see ../../LICENSE.
// The host tests' windows.h (Linux, macOS): only what the platform's l10n.cpp and fh2_mod_platform_eo_l10n.cpp use.
// GetUserDefaultLocaleName answers TEST_LOCALE (default en-US).
#pragma once
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <unistd.h>
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 8
#define LOCALE_NAME_MAX_LENGTH 85
#define EXCEPTION_EXECUTE_HANDLER 1
inline void Sleep(unsigned ms) { usleep(ms * 1000); }
inline int GetUserDefaultLocaleName(wchar_t* out, int n) {
    const char* l = std::getenv("TEST_LOCALE"); if (!l) l = "en-US";
    int i = 0; for (; l[i] && i < n - 1; ++i) out[i] = l[i]; out[i] = 0; return i + 1;
}
// UTF-8 <-> UTF-32 (wchar_t on Linux), with Win32's counting: n == -1 includes the NUL.
inline int MultiByteToWideChar(unsigned, unsigned, const char* s, int n, wchar_t* out, int cap) {
    size_t len = n < 0 ? std::strlen(s) + 1 : (size_t)n; int w = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i]; unsigned cp; int k;
        if (c < 0x80) { cp = c; k = 1; } else if ((c >> 5) == 6) { cp = c & 31; k = 2; } else if ((c >> 4) == 14) { cp = c & 15; k = 3; } else { cp = c & 7; k = 4; }
        for (int j = 1; j < k; ++j) cp = (cp << 6) | (s[i + j] & 63);
        if (cap) { if (w >= cap) return 0; out[w] = (wchar_t)cp; }
        ++w; i += k;
    }
    return w;
}
inline int WideCharToMultiByte(unsigned, unsigned, const wchar_t* w, int n, char* out, int cap, const char*, int*) {
    size_t len = n < 0 ? std::wcslen(w) + 1 : (size_t)n; int b = 0; char t[4];
    for (size_t i = 0; i < len; ++i) {
        unsigned cp = (unsigned)w[i]; int k;
        if (cp < 0x80) { t[0] = (char)cp; k = 1; } else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 63)); k = 2; }
        else if (cp < 0x10000) { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); k = 3; }
        else { t[0] = (char)(0xF0 | cp >> 18); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); k = 4; }
        if (cap) { if (b + k > cap) return 0; std::memcpy(out + b, t, k); }
        b += k;
    }
    return b;
}
