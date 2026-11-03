// I18n.h - textos em português, inglês e espanhol. Idioma do Windows por padrão; pode ser trocado nas configurações.
#pragma once
#include <windows.h>

namespace i18n {

enum { kPt = 0, kEn = 1, kEs = 2 };

inline int& Override() { static int v = -1; return v; } // -1 = automático

inline int Current() {
    if (Override() >= 0) return Override();
    static int sys = -1;
    if (sys < 0) {
        WORD p = PRIMARYLANGID(GetUserDefaultUILanguage());
        sys = p == LANG_PORTUGUESE ? kPt : p == LANG_SPANISH ? kEs : kEn;
    }
    return sys;
}

// Lê a escolha salva nas configurações (0 = automático, 1 PT, 2 EN, 3 ES).
inline void Load() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\DesignThumbs", L"Language", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    Override() = v >= 1 && v <= 3 ? (int)v - 1 : -1;
}

inline const wchar_t* T(const wchar_t* pt, const wchar_t* en, const wchar_t* es) {
    int l = Current();
    return l == kPt ? pt : l == kEn ? en : es;
}

} // namespace i18n

using i18n::T;
