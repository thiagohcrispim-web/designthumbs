// Conta-gotas: CMYK (perfil de impressão do Windows) e PANTONE mais próximo (CIEDE2000). Tudo consultivo.
#pragma once
#include <windows.h>
#include <icm.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include "Pantone.h"

namespace cinfo {

// ---------- CMYK: sRGB -> RSWOP.icm (vem com o Windows) pela gestão de cores do sistema ----------
inline HTRANSFORM Transform() {
    static HTRANSFORM t = [] {
        wchar_t dir[MAX_PATH] = {}; DWORD sz = sizeof(dir);
        if (!GetColorDirectoryW(nullptr, dir, &sz)) return (HTRANSFORM)nullptr;
        std::wstring srgb = std::wstring(dir) + L"\\sRGB Color Space Profile.icm", swop = std::wstring(dir) + L"\\RSWOP.icm";
        PROFILE p1 = {PROFILE_FILENAME, (PVOID)srgb.c_str(), (DWORD)((srgb.size() + 1) * sizeof(wchar_t))};
        PROFILE p2 = {PROFILE_FILENAME, (PVOID)swop.c_str(), (DWORD)((swop.size() + 1) * sizeof(wchar_t))};
        HPROFILE h[2] = {OpenColorProfileW(&p1, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING),
                         OpenColorProfileW(&p2, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING)};
        HTRANSFORM r = nullptr;
        if (h[0] && h[1]) {
            DWORD intent[2] = {INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC};
            r = CreateMultiProfileTransform(h, 2, intent, 2, BEST_MODE, INDEX_DONT_CARE);
        }
        if (h[0]) CloseColorProfile(h[0]);
        if (h[1]) CloseColorProfile(h[1]);
        return r;
    }();
    return t;
}
// Porcentagens 0..100. Sem o perfil, cai na fórmula simples.
inline void Cmyk(COLORREF c, int out[4]) {
    double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    if (HTRANSFORM t = Transform()) {
        COLOR in = {}, res = {};
        in.rgb.red = (WORD)(GetRValue(c) * 257); in.rgb.green = (WORD)(GetGValue(c) * 257); in.rgb.blue = (WORD)(GetBValue(c) * 257);
        if (TranslateColors(t, &in, 1, COLOR_RGB, &res, COLOR_CMYK)) {
            WORD v[4] = {res.cmyk.cyan, res.cmyk.magenta, res.cmyk.yellow, res.cmyk.black};
            for (int i = 0; i < 4; ++i) out[i] = (int)lround(v[i] * 100.0 / 65535.0);
            return;
        }
    }
    double k = 1 - std::fmax(r, std::fmax(g, b));
    if (k >= 1) { out[0] = out[1] = out[2] = 0; out[3] = 100; return; }
    out[0] = (int)lround((1 - r - k) / (1 - k) * 100); out[1] = (int)lround((1 - g - k) / (1 - k) * 100);
    out[2] = (int)lround((1 - b - k) / (1 - k) * 100); out[3] = (int)lround(k * 100);
}

// ---------- PANTONE: Lab D65 + CIEDE2000 ----------
struct Lab { double L, a, b; };
inline Lab ToLab(unsigned rgb) { // 0xRRGGBB
    auto lin = [](double v) { v /= 255; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); };
    double r = lin(rgb >> 16 & 255), g = lin(rgb >> 8 & 255), b = lin(rgb & 255);
    double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047, y = 0.2126 * r + 0.7152 * g + 0.0722 * b,
           z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    auto f = [](double t) { return t > 0.008856 ? cbrt(t) : 7.787 * t + 16.0 / 116; };
    return {116 * f(y) - 16, 500 * (f(x) - f(y)), 200 * (f(y) - f(z))};
}
inline double DeltaE2000(const Lab& p, const Lab& q) {
    const double pi = 3.14159265358979323846, d2r = pi / 180;
    double C1 = hypot(p.a, p.b), C2 = hypot(q.a, q.b), Cm = (C1 + C2) / 2, Cm7 = pow(Cm, 7);
    double G = 0.5 * (1 - sqrt(Cm7 / (Cm7 + 6103515625.0)));
    double a1 = (1 + G) * p.a, a2 = (1 + G) * q.a, c1 = hypot(a1, p.b), c2 = hypot(a2, q.b);
    auto hue = [&](double b, double a) { if (a == 0 && b == 0) return 0.0; double h = atan2(b, a) / d2r; return h < 0 ? h + 360 : h; };
    double h1 = hue(p.b, a1), h2 = hue(q.b, a2), dL = q.L - p.L, dC = c2 - c1, dh = 0;
    if (c1 * c2 != 0) { dh = h2 - h1; if (dh > 180) dh -= 360; else if (dh < -180) dh += 360; }
    double dH = 2 * sqrt(c1 * c2) * sin(dh * d2r / 2), Lm = (p.L + q.L) / 2, cm = (c1 + c2) / 2, hm = h1 + h2;
    if (c1 * c2 != 0) { if (fabs(h1 - h2) > 180) hm += hm < 360 ? 360 : -360; hm /= 2; }
    double T = 1 - 0.17 * cos((hm - 30) * d2r) + 0.24 * cos(2 * hm * d2r) + 0.32 * cos((3 * hm + 6) * d2r) - 0.2 * cos((4 * hm - 63) * d2r);
    double dTh = 30 * exp(-pow((hm - 275) / 25, 2)), cm7 = pow(cm, 7), Rc = 2 * sqrt(cm7 / (cm7 + 6103515625.0));
    double Sl = 1 + 0.015 * pow(Lm - 50, 2) / sqrt(20 + pow(Lm - 50, 2)), Sc = 1 + 0.045 * cm, Sh = 1 + 0.015 * cm * T;
    double Rt = -sin(2 * dTh * d2r) * Rc, l = dL / Sl, c = dC / Sc, h = dH / Sh;
    return sqrt(l * l + c * c + h * h + Rt * c * h);
}
// Nome do PANTONE Coated mais próximo; vazio se nada estiver razoavelmente perto (ΔE > 10).
inline std::wstring NearestPantone(COLORREF c, double* deltaE = nullptr) {
    static const size_t n = sizeof(pms::kCoated) / sizeof(pms::kCoated[0]);
    static Lab* labs = [] { Lab* l = new Lab[n]; for (size_t i = 0; i < n; ++i) l[i] = ToLab(pms::kCoated[i].rgb); return l; }();
    Lab me = ToLab((unsigned)GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c));
    size_t best = 0; double bd = 1e9;
    for (size_t i = 0; i < n; ++i) {
        double cheap = fabs(labs[i].L - me.L); // poda: ΔL grande nunca vence
        if (cheap > bd * 2.5) continue;
        double d = DeltaE2000(me, labs[i]);
        if (d < bd) { bd = d; best = i; }
    }
    if (deltaE) *deltaE = bd;
    if (bd > 10) return L"";
    const char* s = pms::kCoated[best].name;
    return std::wstring(s, s + strlen(s));
}

// Resultado cacheado para a última cor (o desenho chama a cada movimento do mouse).
struct Info { std::wstring cmyk, pantone; };
inline const Info& For(COLORREF c) {
    static COLORREF last = CLR_INVALID; static Info info;
    if (c == last) return info;
    last = c;
    int k[4]; Cmyk(c, k);
    wchar_t t[64]; swprintf(t, 64, L"CMYK %d · %d · %d · %d", k[0], k[1], k[2], k[3]);
    info.cmyk = t;
    info.pantone = NearestPantone(c); // vazio = sem equivalente próximo
    return info;
}
} // namespace cinfo
