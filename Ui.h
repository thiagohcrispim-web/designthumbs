// Ui.h - visual "vidro" inspirado no visionOS: tema do Windows, desfoque acrylic e desenho em Direct2D.
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include "Feather.h"

namespace ui {

// ---------- tema ----------

enum { kThemeLight = 0, kThemeDark, kThemeWindows, kThemeCrispy, kThemeClassic }; // Claro, Escuro, Windows, Vizu Crispy, Vizu Classic

struct Theme {
    bool dark = true, glass = false, neon = false; // neon: brilho laranja (tema Crispy)
    const wchar_t* font = L"Bahnschrift";
    D2D1_COLOR_F tint;        // fundo da janela (sobre o desfoque)
    D2D1_COLOR_F panel;       // cartões / cápsulas
    D2D1_COLOR_F panelHot;    // cartão sob o mouse
    D2D1_COLOR_F stroke;      // borda de luz
    D2D1_COLOR_F text, text2; // texto principal / secundário
    D2D1_COLOR_F accent, onAccent, danger, track, knob, knobOn;
    D2D1_COLOR_F segFill, segText; // item selecionado do controle segmentado
    D2D1_COLOR_F icon;             // cor dos ícones
    bool classic = false;          // Vizu Classic: chapado, laranja só como preenchimento
    UINT glow = 0xFFFFFF;     // cor do brilho que segue o mouse no anel
    float glowA = 0.10f;
};

inline D2D1_COLOR_F Rgba(UINT rgb, float a = 1) { return D2D1::ColorF(rgb, a); }

// Cor do neon do tema Crispy (escolhida pelo usuário).
inline UINT& NeonColor() { static UINT c = 0xFF6633; return c; }

inline bool SystemDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

// Cor de destaque do Windows (Configurações > Personalização > Cores).
inline UINT SystemAccent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF); // ABGR -> RGB
    return 0x0067C0;
}

// Crispy: preto, neon cyberpunk #FF6633 e fonte Bahnschrift.
inline Theme CrispyTheme(bool glass) {
    Theme t;
    t.dark = true; t.glass = glass; t.neon = true;
    t.font = L"Bahnschrift";
    t.tint = glass ? Rgba(0x0A0A0B, 0.85f) : Rgba(0x0A0A0B); // 85%: vidro translúcido
    t.panel = Rgba(0xFFFFFF, 0.045f); t.panelHot = Rgba(NeonColor(), 0.16f); t.stroke = Rgba(NeonColor(), 0.32f);
    t.text = Rgba(0xF2F2F2); t.text2 = Rgba(0x9A9AA1);
    t.accent = Rgba(NeonColor()); t.onAccent = Rgba(0x0E0E10); t.danger = Rgba(0xFF3B5C);
    t.track = Rgba(0xFFFFFF, 0.10f); t.knob = Rgba(0xF2F2F2); t.knobOn = Rgba(0xF2F2F2);
    t.glow = NeonColor(); t.glowA = 0.40f;
    t.segFill = Rgba(NeonColor(), 0.18f); t.segText = Rgba(NeonColor()); t.icon = t.text;
    return t;
}

// Vizu Classic: o Design System Vizu (grafite e Ardósia, laranja #FF6633 só como preenchimento, sem brilhos).
inline Theme ClassicTheme() {
    Theme t;
    t.dark = true; t.glass = false; t.classic = true;
    t.tint = Rgba(0x2E333B);
    t.panel = Rgba(0x4A4F57); t.panelHot = Rgba(0x5C6472); t.stroke = Rgba(0x5C6472);
    t.text = Rgba(0xFFFFFF); t.text2 = Rgba(0xBCC2CC);
    t.accent = Rgba(0xFF6633); t.onAccent = Rgba(0x000000); t.danger = Rgba(0xFF6961);
    t.track = Rgba(0x23272E); t.knob = Rgba(0xBCC2CC); t.knobOn = Rgba(0x000000);
    t.segFill = Rgba(0xF2F2F2); t.segText = Rgba(0x1E2126); t.icon = Rgba(0xFFFFFF);
    t.glow = 0xFFFFFF; t.glowA = 0.05f;
    return t;
}

inline Theme MakeTheme(int mode, bool glass) {
    if (mode == kThemeCrispy) return CrispyTheme(glass);
    if (mode == kThemeClassic) return ClassicTheme();
    Theme t;
    bool dark = mode == kThemeDark || (mode == kThemeWindows && SystemDark());
    t.dark = dark; t.glass = glass;
    if (mode == kThemeDark) { // preto x chumbo, sem cor
        t.tint = glass ? Rgba(0x0B0B0C, 0.80f) : Rgba(0x0B0B0C);
        t.panel = Rgba(0xFFFFFF, 0.05f); t.panelHot = Rgba(0xFFFFFF, 0.10f); t.stroke = Rgba(0xFFFFFF, 0.10f);
        t.text = Rgba(0xEDEDEF); t.text2 = Rgba(0x8D8D93);
        t.accent = Rgba(0x4A4F57); t.onAccent = Rgba(0xFFFFFF); t.danger = Rgba(0xFF6961); // grafite (Ardósia) da Vizu
        t.track = Rgba(0xFFFFFF, 0.12f); t.knob = Rgba(0x9AA1AD); t.knobOn = Rgba(0xFFFFFF);
        t.segFill = Rgba(0x4A4F57); t.segText = Rgba(0xFFFFFF); t.icon = Rgba(0x9AA1AD);
        t.glow = 0xFFFFFF; t.glowA = 0.10f;
    } else if (dark) { // Padrão Windows, escuro
        t.tint = glass ? Rgba(0x1C1C1E, 0.62f) : Rgba(0x1C1C1E);
        t.panel = Rgba(0xFFFFFF, 0.08f); t.panelHot = Rgba(0xFFFFFF, 0.14f); t.stroke = Rgba(0xFFFFFF, 0.14f);
        t.text = Rgba(0xFFFFFF, 0.96f); t.text2 = Rgba(0xFFFFFF, 0.58f);
        t.accent = Rgba(SystemAccent()); t.onAccent = Rgba(0xFFFFFF); t.danger = Rgba(0xFF6961);
        t.track = Rgba(0xFFFFFF, 0.18f); t.knob = Rgba(0xFFFFFF); t.knobOn = Rgba(0xFFFFFF);
        t.segFill = Rgba(0xFFFFFF, 0.14f); t.segText = t.text; t.icon = t.text;
        t.glow = SystemAccent(); t.glowA = 0.22f;
    } else { // claro: minimalista (grafite) ou Padrão Windows (cor de destaque do sistema)
        t.tint = glass ? Rgba(0xF5F5F7, 0.70f) : Rgba(0xF5F5F7);
        t.panel = Rgba(0xFFFFFF, 0.62f); t.panelHot = Rgba(0xFFFFFF, 0.88f); t.stroke = Rgba(0x000000, 0.07f);
        t.text = Rgba(0x1D1D1F); t.text2 = Rgba(0x000000, 0.52f);
        bool win = mode == kThemeWindows;
        t.accent = win ? Rgba(SystemAccent()) : Rgba(0x1D1D1F); t.onAccent = Rgba(0xFFFFFF); t.danger = Rgba(0xD70015);
        t.track = Rgba(0x000000, 0.10f); t.knob = Rgba(0xFFFFFF); t.knobOn = Rgba(0xFFFFFF);
        t.glow = win ? SystemAccent() : 0xFFFFFF; t.glowA = win ? 0.16f : 0.30f;
        t.segFill = Rgba(0xFFFFFF); t.segText = t.text; t.icon = t.text;
    }
    return t;
}

// Fundo de vidro: halos suaves de luz (no Crispy, degradê neon laranja sobre preto).
inline void Backdrop(ID2D1RenderTarget* rt, const Theme& th, float w, float h) {
    if (th.classic) return; // Vizu Classic: fundo chapado
    struct B { float x, y, r; UINT c; float a; };
    B bl[3] = {{w * 0.05f, h * 0.0f, (w > h ? w : h) * 0.55f, th.neon ? NeonColor() : th.dark ? 0x5A5A62u : 0xFFFFFFu, th.neon ? 0.22f : th.dark ? 0.16f : 0.55f},
               {w * 1.0f, h * 1.05f, (w > h ? w : h) * 0.55f, th.neon ? NeonColor() : th.dark ? 0x3A3A40u : 0xD0D4DCu, th.neon ? 0.05f : th.dark ? 0.14f : 0.35f},
               {w * 0.85f, h * 0.1f, (w > h ? w : h) * 0.30f, th.neon ? NeonColor() : th.glow, th.neon ? 0.10f : th.dark ? 0.04f : 0.10f}};
    ID2D1SolidColorBrush* b = nullptr;
    for (auto& x : bl) {
        const int steps = 24; // anéis concêntricos com pouca opacidade = degradê radial suave
        for (int i = steps; i >= 1; --i) {
            float r = x.r * i / steps;
            if (SUCCEEDED(rt->CreateSolidColorBrush(Rgba(x.c, x.a / steps * 1.6f), &b))) { rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x.x, x.y), r, r), b); b->Release(); }
        }
    }
}

// Desfoque "acrylic" atrás da janela (funciona mesmo com a janela inativa). Retorna false se indisponível.
inline bool EnableGlass(HWND h, bool dark) {
    MARGINS m = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(h, &m);
    DWORD corner = 2; // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(h, 33, &corner, sizeof(corner));
    BOOL d = dark;
    DwmSetWindowAttribute(h, 20, &d, sizeof(d));
    struct AccentPolicy { int state, flags; DWORD color; int anim; };
    struct CompositionData { int attrib; void* data; SIZE_T size; };
    typedef BOOL(WINAPI * SetWca)(HWND, CompositionData*);
    static SetWca set = (SetWca)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!set) return false;
    AccentPolicy ap = {4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, 0, 0x01000000, 0};
    CompositionData cd = {19 /* WCA_ACCENT_POLICY */, &ap, sizeof(ap)};
    return set(h, &cd) != FALSE;
}

// ---------- superfície Direct2D com transparência ----------

struct Surface {
    HWND hwnd = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    bool Begin(ID2D1Factory* f) {
        if (!rt) {
            RECT rc; GetClientRect(hwnd, &rc);
            D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96); // trabalha em pixels
            if (FAILED(f->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &rt))) return false;
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType não funciona sobre transparência
        }
        rt->BeginDraw();
        return true;
    }
    bool End() { // false: recurso perdido, recriar bitmaps
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { rt->Release(); rt = nullptr; return false; }
        return true;
    }
    void Resize(UINT w, UINT h) { if (rt) rt->Resize(D2D1::SizeU(w, h)); }
    void Release() { if (rt) { rt->Release(); rt = nullptr; } }
};

// Caminho SVG (subconjunto: M L H V C A Z, absolutos e relativos) -> geometria Direct2D, escalado e deslocado.
inline ID2D1PathGeometry* SvgPath(ID2D1Factory* f, const char* d, float sc, float ox, float oy, bool filled = false) {
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* k = nullptr;
    if (FAILED(f->CreatePathGeometry(&g)) || FAILED(g->Open(&k))) { if (g) g->Release(); return nullptr; }
    const char* p = d;
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; // lcx/lcy: último ponto de controle (para 's')
    bool open = false; char lastCmd = 0;
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * sc, oy + y * sc); };
    auto skip = [&]() { while (*p == ' ' || *p == ',' || *p == '\n') ++p; };
    auto num = [&]() { skip(); char* e; float v = strtof(p, &e); p = e; return v; };
    auto hasNum = [&]() { skip(); return (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '+'; };
    auto begin = [&](float x, float y) { if (open) k->EndFigure(D2D1_FIGURE_END_OPEN); k->BeginFigure(P(x, y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW); open = true; sx = x; sy = y; };
    while (true) {
        skip();
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
        bool rel = cmd >= 'a';
        float bx = rel ? cx : 0, by = rel ? cy : 0;
        switch (cmd | 32) {
        case 'm': { float x = num() + bx, y = num() + by; begin(x, y); cx = x; cy = y; cmd = rel ? 'l' : 'L'; break; }
        case 'l': { float x = num() + bx, y = num() + by; k->AddLine(P(x, y)); cx = x; cy = y; break; }
        case 'h': { float x = num() + bx; k->AddLine(P(x, cy)); cx = x; break; }
        case 'v': { float y = num() + by; k->AddLine(P(cx, y)); cy = y; break; }
        case 'c': {
            float x1 = num() + bx, y1 = num() + by, x2 = num() + bx, y2 = num() + by, x = num() + bx, y = num() + by;
            k->AddBezier(D2D1::BezierSegment(P(x1, y1), P(x2, y2), P(x, y))); lcx = x2; lcy = y2; cx = x; cy = y; break;
        }
        case 's': {
            bool prevCubic = (lastCmd | 32) == 'c' || (lastCmd | 32) == 's';
            float x1 = prevCubic ? 2 * cx - lcx : cx, y1 = prevCubic ? 2 * cy - lcy : cy;
            float x2 = num() + bx, y2 = num() + by, x = num() + bx, y = num() + by;
            k->AddBezier(D2D1::BezierSegment(P(x1, y1), P(x2, y2), P(x, y))); lcx = x2; lcy = y2; cx = x; cy = y; break;
        }
        case 'a': {
            float rx = num(), ry = num(), rot = num(), large = num(), sweep = num(), x = num() + bx, y = num() + by;
            if (rx <= 0 || ry <= 0 || (x == cx && y == cy)) { k->AddLine(P(x, y)); cx = x; cy = y; break; }
            { // arco SVG -> curvas Bézier (o mesmo resultado em qualquer renderizador)
                const float PI = 3.14159265f;
                float ph = rot * PI / 180, co = cosf(ph), si = sinf(ph);
                float dx2 = (cx - x) / 2, dy2 = (cy - y) / 2, x1p = co * dx2 + si * dy2, y1p = -si * dx2 + co * dy2;
                rx = fabsf(rx); ry = fabsf(ry);
                float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
                if (lam > 1) { float q = sqrtf(lam); rx *= q; ry *= q; }
                float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p, den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
                float coef = (large != sweep ? 1.0f : -1.0f) * sqrtf(fmaxf(0.0f, den > 0 ? num / den : 0));
                float cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
                float ecx = co * cxp - si * cyp + (cx + x) / 2, ecy = si * cxp + co * cyp + (cy + y) / 2;
                auto ang = [](float ux, float uy, float vx, float vy) { return atan2f(ux * vy - uy * vx, ux * vx + uy * vy); };
                float t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry), dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
                if (sweep == 0 && dt > 0) dt -= 2 * PI; else if (sweep != 0 && dt < 0) dt += 2 * PI;
                int n = (int)ceilf(fabsf(dt) / (PI / 2) - 0.001f); if (n < 1) n = 1;
                float d = dt / n, tk = 4.0f / 3 * tanf(d / 4);
                auto E = [&](float t, float* px, float* py) { *px = ecx + rx * co * cosf(t) - ry * si * sinf(t); *py = ecy + rx * si * cosf(t) + ry * co * sinf(t); };
                auto D = [&](float t, float* px, float* py) { *px = -rx * co * sinf(t) - ry * si * cosf(t); *py = -rx * si * sinf(t) + ry * co * cosf(t); };
                for (int i = 0; i < n; ++i) {
                    float a0 = t1 + i * d, a1 = a0 + d, ax, ay, bx2, by2, dax, day, dbx, dby;
                    E(a0, &ax, &ay); E(a1, &bx2, &by2); D(a0, &dax, &day); D(a1, &dbx, &dby);
                    if (i == n - 1) { bx2 = x; by2 = y; }
                    k->AddBezier(D2D1::BezierSegment(P(ax + tk * dax, ay + tk * day), P(bx2 - tk * dbx, by2 - tk * dby), P(bx2, by2)));
                }
            }
            cx = x; cy = y; break;
        }
        case 'z': if (open) { k->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; } cx = sx; cy = sy; break;
        default: ++p; continue;
        }
        lastCmd = cmd;
        if ((cmd | 32) == 'z') continue;
        if (!hasNum() && *p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) ++p;
    }
    if (open) k->EndFigure(D2D1_FIGURE_END_OPEN);
    k->Close(); k->Release();
    return g;
}

// ---------- pintor ----------

// Shared official Vizu wordmark used by settings and installer.
inline void Brand(ID2D1RenderTarget* rt, const Theme& th, float x, float y, float height) {
    static const char* kWord[] = {
            "M-121.57-2069.57H102.46l256.87,572.16,256.88-572.16h224.03l-349.54,744.23H227.96l-349.54-744.23Z",
            "M887.14-1989.34c0-57.01,44.57-88.67,123.16-88.67s121.99,31.67,121.99,88.67-42.23,88.67-121.99,88.67-123.16-31.67-123.16-88.67ZM902.39-1871.11h215.82v545.77h-215.82v-545.77Z",
            "M1179.19-1455.18l364.79-289.25h-360.09v-125.62h609.93v125.62l-367.13,294.53h373v124.57h-620.49v-129.84Z",
            "M2577.32-1325.34h-215.82v-205.85c-10.56,80.23-72.72,210.07-235.76,216.41-168.9,6.33-279.16-100.29-279.16-235.41v-319.86h215.82v292.42c0,90.79,41.05,137.23,98.53,137.23,111.43,0,200.57-164.68,200.57-307.19v-122.46h215.82v544.71Z",
            "M2667.76-1485.87c0-102.03,71.8-158.72,198.4-158.72s196.51,56.68,196.51,158.72-68.02,158.72-196.51,158.72-198.4-56.69-198.4-158.72Z"};
    float sc = height / 830.0f, ox = x + 160 * sc, oy = y + 2120 * sc;
    ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
    for (int i = 0; i < 5; ++i) {
        ID2D1PathGeometry* g = SvgPath(fac, kWord[i], sc, ox, oy, true);
        ID2D1SolidColorBrush* b = nullptr;
        if (g && SUCCEEDED(rt->CreateSolidColorBrush(i == 4 ? Rgba(0xFF6633) : th.text, &b))) { rt->FillGeometry(g, b); b->Release(); }
        if (g) g->Release();
    }
    fac->Release();
}

class Painter {
public:
    ID2D1RenderTarget* rt;
    IDWriteFactory* dw;
    const Theme& th;
    float s; // escala de DPI
    Painter(ID2D1RenderTarget* r, IDWriteFactory* d, const Theme& t, float scale) : rt(r), dw(d), th(t), s(scale) {}

    static const wchar_t* IconFont() {
        static const wchar_t* name = nullptr;
        if (!name) {
            name = L"Segoe MDL2 Assets";
            HDC dc = GetDC(nullptr);
            LOGFONTW lf = {}; lf.lfCharSet = DEFAULT_CHARSET; wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
            EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *(const wchar_t**)p = L"Segoe Fluent Icons"; return 0; }, (LPARAM)&name, 0);
            ReleaseDC(nullptr, dc);
        }
        return name;
    }

    void Fill(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b); b->Release(); }
    }
    void Stroke(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float w = 1) {
        ID2D1SolidColorBrush* b = nullptr;
        D2D1_RECT_F in = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
        if (SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), b, w); b->Release(); }
    }
    // Cartão de vidro: preenchimento translúcido + borda de luz (no Crispy, borda laranja com leve brilho).
    void Glass(const D2D1_RECT_F& r, float radius, bool hot = false) {
        Fill(r, radius, hot ? th.panelHot : th.panel);
        if (th.neon && hot) Glow(r, radius);
        Stroke(r, radius, th.stroke);
    }
    // Brilho neon discreto em volta de um retângulo arredondado.
    void Glow(const D2D1_RECT_F& r, float radius) {
        for (int i = 3; i >= 1; --i) {
            float g = i * 1.6f * s;
            Stroke(D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g), radius + g, Rgba(NeonColor(), 0.07f * (4 - i)), 1.4f * s);
        }
    }
    // Sombra suave barata (camadas translúcidas).
    void Shadow(const D2D1_RECT_F& r, float radius) {
        for (int i = 4; i >= 1; --i) {
            float g = i * 3 * s;
            Fill(D2D1::RectF(r.left - g, r.top - g + 2 * s, r.right + g, r.bottom + g + 4 * s), radius + g, Rgba(0x000000, th.dark ? 0.07f : 0.035f));
        }
    }

    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, const wchar_t* family = nullptr, bool wrap = false) {
        IDWriteTextFormat* f = nullptr;
        if (!family) family = th.font;
        if (FAILED(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * s, L"pt-br", &f))) return nullptr;
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!wrap) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject* e = nullptr;
            if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f, &e))) { f->SetTrimming(&trim, e); e->Release(); }
        }
        return f;
    }
    void Text(const std::wstring& t, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        IDWriteTextFormat* f = Format(size, w, align);
        ID2D1SolidColorBrush* b = nullptr;
        if (f && SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawText(t.c_str(), (UINT32)t.size(), f, r, b); b->Release(); }
        if (f) f->Release();
    }
    // Texto com espaçamento extra entre as letras (em fração do tamanho da fonte), centralizado no retângulo.
    void TextSpaced(const std::wstring& t, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w, float em) {
        IDWriteTextFormat* f = Format(size, w, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!f) return;
        IDWriteTextLayout* l = nullptr;
        if (SUCCEEDED(dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, r.right - r.left, r.bottom - r.top, &l))) {
            IDWriteTextLayout1* l1 = nullptr;
            float sp = em * size * s;
            if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
                l1->SetCharacterSpacing(sp / 2, sp / 2, 0, DWRITE_TEXT_RANGE{0, (UINT32)t.size()});
                l1->Release();
            }
            ID2D1SolidColorBrush* b = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), l, b); b->Release(); }
            l->Release();
        }
        f->Release();
    }
    // Texto com quebra de linha; retorna a altura usada.
    float Paragraph(const std::wstring& t, float x, float y, float width, float size, D2D1_COLOR_F c, bool draw = true) {
        IDWriteTextFormat* f = Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, nullptr, true);
        IDWriteTextLayout* lay = nullptr;
        float h = 0;
        if (f && SUCCEEDED(dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, width, 10000, &lay))) {
            lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * s * 1.45f, size * s * 1.1f);
            DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); h = m.height;
            ID2D1SolidColorBrush* b = nullptr;
            if (draw && SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawTextLayout(D2D1::Point2F(x, y), lay, b); b->Release(); }
            lay->Release();
        }
        if (f) f->Release();
        return h;
    }
    float Measure(const std::wstring& t, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL) {
        IDWriteTextFormat* f = Format(size, w, DWRITE_TEXT_ALIGNMENT_LEADING);
        IDWriteTextLayout* lay = nullptr;
        float width = 0;
        if (f && SUCCEEDED(dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, 10000, 100, &lay))) {
            DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); width = m.widthIncludingTrailingWhitespace;
            lay->Release();
        }
        if (f) f->Release();
        return width;
    }
    // Ícone Feather pelo nome (traço com pontas redondas, como no feathericons.com).
    void Feather(const char* d, const D2D1_RECT_F& r, float box, D2D1_COLOR_F c, float stroke = 2) {
        if (!d) return;
        ID2D1Factory* f = nullptr; rt->GetFactory(&f);
        float sc = box / 24, ox = (r.left + r.right) / 2 - box / 2, oy = (r.top + r.bottom) / 2 - box / 2;
        ID2D1PathGeometry* g = SvgPath(f, d, sc, ox, oy);
        ID2D1StrokeStyle* st = nullptr;
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &st);
        ID2D1SolidColorBrush* b = nullptr;
        if (g && SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawGeometry(g, b, (stroke * sc) > 1.1f ? stroke * sc : 1.1f, st); b->Release(); }
        if (st) st->Release();
        if (g) g->Release();
        f->Release();
    }
    void FeatherIcon(const char* name, const D2D1_RECT_F& r, float box, D2D1_COLOR_F c) { Feather(feather::Find(name), r, box, c); }
    void Icon(const wchar_t* glyph, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c) {
        if (glyph && glyph[0] && !glyph[1]) // glifo conhecido: desenha o equivalente Feather
            if (const char* d = feather::ForGlyph(glyph[0])) { Feather(d, r, size * 1.3f * s, c); return; }
        IDWriteTextFormat* f = Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, IconFont());
        ID2D1SolidColorBrush* b = nullptr;
        if (f && SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawText(glyph, (UINT32)wcslen(glyph), f, r, b); b->Release(); }
        if (f) f->Release();
    }
    // Botão redondo de vidro com ícone.
    void CircleButton(const D2D1_RECT_F& r, const wchar_t* glyph, bool hot) {
        float rad = (r.bottom - r.top) / 2;
        if (hot) Fill(r, rad, th.panelHot);
        if (hot && th.neon) Glow(r, rad);
        Stroke(r, rad, hot ? (th.neon ? th.accent : th.stroke) : th.stroke);
        Icon(glyph, r, 13, hot ? (th.neon ? th.accent : th.icon) : th.text2);
    }
    // Interruptor estilo iOS.
    void Toggle(const D2D1_RECT_F& r, bool on, bool hot) {
        float h = r.bottom - r.top;
        if (on && th.neon) Glow(r, h / 2);
        Fill(r, h / 2, on ? th.accent : th.track);
        if (hot) Fill(r, h / 2, Rgba(0xFFFFFF, 0.10f));
        float k = h - 4 * s, x = on ? r.right - 2 * s - k : r.left + 2 * s;
        D2D1_RECT_F kr = D2D1::RectF(x, r.top + 2 * s, x + k, r.top + 2 * s + k);
        Fill(D2D1::RectF(kr.left, kr.top + 1 * s, kr.right, kr.bottom + 1 * s), k / 2, Rgba(0x000000, 0.18f));
        Fill(kr, k / 2, on ? th.knobOn : th.knob);
    }
    // Controle segmentado; retorna nada, quem chama guarda os retângulos de cada parte.
    void Segmented(const D2D1_RECT_F& r, const std::wstring* labels, int n, int sel, int hot, D2D1_RECT_F* parts) {
        float h = r.bottom - r.top;
        Fill(r, h / 2, th.track);
        float w = (r.right - r.left - 4 * s) / n;
        for (int i = 0; i < n; ++i) {
            D2D1_RECT_F p = D2D1::RectF(r.left + 2 * s + i * w, r.top + 2 * s, r.left + 2 * s + (i + 1) * w, r.bottom - 2 * s);
            parts[i] = p;
            if (i == sel) {
                Fill(p, (h - 4 * s) / 2, th.segFill);
                if (th.neon || !th.classic) Stroke(p, (h - 4 * s) / 2, th.neon ? th.accent : th.stroke);
            }
            else if (i == hot) Fill(p, (h - 4 * s) / 2, Rgba(th.dark ? 0xFFFFFF : 0x000000, 0.06f));
            Text(labels[i], p, 12.5f, i == sel ? th.segText : th.text2, i == sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    // [ − valor + ]; minus/plus recebem os retângulos clicáveis.
    void Stepper(const D2D1_RECT_F& r, const std::wstring& value, int hot, D2D1_RECT_F* minus, D2D1_RECT_F* plus) {
        float h = r.bottom - r.top;
        Fill(r, h / 2, th.track);
        *minus = D2D1::RectF(r.left + 2 * s, r.top + 2 * s, r.left + h - 2 * s, r.bottom - 2 * s);
        *plus = D2D1::RectF(r.right - h + 2 * s, r.top + 2 * s, r.right - 2 * s, r.bottom - 2 * s);
        if (hot == 0) Fill(*minus, (h - 4 * s) / 2, th.panelHot);
        if (hot == 1) Fill(*plus, (h - 4 * s) / 2, th.panelHot);
        Icon(L"", *minus, 11, th.text);
        Icon(L"", *plus, 11, th.text);
        Text(value, D2D1::RectF(r.left + h, r.top, r.right - h, r.bottom), 12.5f, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Botão "pílula" com texto.
    void Pill(const D2D1_RECT_F& r, const std::wstring& label, bool hot, bool primary = false) {
        float h = r.bottom - r.top;
        if (primary) { if (th.neon) Glow(r, h / 2); Fill(r, h / 2, th.accent); if (hot) Fill(r, h / 2, Rgba(0xFFFFFF, 0.14f)); }
        else {
            if (hot) Fill(r, h / 2, th.panelHot);
            if (hot && th.neon) Glow(r, h / 2);
            Stroke(r, h / 2, hot && th.neon ? th.accent : th.stroke);
        }
        Text(label, r, 12.5f, primary ? th.onAccent : (hot && th.neon ? th.accent : th.text), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
};

inline bool Hit(const D2D1_RECT_F& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

} // namespace ui
