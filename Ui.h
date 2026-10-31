// Ui.h - visual "vidro" inspirado no visionOS: tema do Windows, desfoque acrylic e desenho em Direct2D.
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <string>

namespace ui {

// ---------- tema ----------

struct Theme {
    bool dark = true, glass = false;
    D2D1_COLOR_F tint;        // fundo da janela (sobre o desfoque)
    D2D1_COLOR_F panel;       // cartões / cápsulas
    D2D1_COLOR_F panelHot;    // cartão sob o mouse
    D2D1_COLOR_F stroke;      // borda de luz
    D2D1_COLOR_F text, text2; // texto principal / secundário
    D2D1_COLOR_F accent, onAccent, danger, track, knob;
};

inline D2D1_COLOR_F Rgba(UINT rgb, float a = 1) { return D2D1::ColorF(rgb, a); }

inline bool SystemDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

inline Theme MakeTheme(bool dark, bool glass) {
    Theme t;
    t.dark = dark; t.glass = glass;
    if (dark) {
        t.tint = glass ? Rgba(0x1C1C1E, 0.62f) : Rgba(0x1C1C1E);
        t.panel = Rgba(0xFFFFFF, 0.08f); t.panelHot = Rgba(0xFFFFFF, 0.14f); t.stroke = Rgba(0xFFFFFF, 0.16f);
        t.text = Rgba(0xFFFFFF, 0.96f); t.text2 = Rgba(0xFFFFFF, 0.58f);
        t.accent = Rgba(0x0A84FF); t.onAccent = Rgba(0xFFFFFF); t.danger = Rgba(0xFF6961);
        t.track = Rgba(0xFFFFFF, 0.18f); t.knob = Rgba(0xFFFFFF);
    } else {
        t.tint = glass ? Rgba(0xF5F5F7, 0.68f) : Rgba(0xF5F5F7);
        t.panel = Rgba(0xFFFFFF, 0.62f); t.panelHot = Rgba(0xFFFFFF, 0.85f); t.stroke = Rgba(0x000000, 0.08f);
        t.text = Rgba(0x1D1D1F); t.text2 = Rgba(0x000000, 0.52f);
        t.accent = Rgba(0x007AFF); t.onAccent = Rgba(0xFFFFFF); t.danger = Rgba(0xD70015);
        t.track = Rgba(0x000000, 0.12f); t.knob = Rgba(0xFFFFFF);
    }
    return t;
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

// ---------- pintor ----------

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
    // Cartão de vidro: preenchimento translúcido + borda de luz.
    void Glass(const D2D1_RECT_F& r, float radius, bool hot = false) {
        Fill(r, radius, hot ? th.panelHot : th.panel);
        Stroke(r, radius, th.stroke);
    }
    // Sombra suave barata (camadas translúcidas).
    void Shadow(const D2D1_RECT_F& r, float radius) {
        for (int i = 4; i >= 1; --i) {
            float g = i * 3 * s;
            Fill(D2D1::RectF(r.left - g, r.top - g + 2 * s, r.right + g, r.bottom + g + 4 * s), radius + g, Rgba(0x000000, th.dark ? 0.07f : 0.035f));
        }
    }

    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, const wchar_t* family = L"Segoe UI Variable Text", bool wrap = false) {
        IDWriteTextFormat* f = nullptr;
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
    // Texto com quebra de linha; retorna a altura usada.
    float Paragraph(const std::wstring& t, float x, float y, float width, float size, D2D1_COLOR_F c, bool draw = true) {
        IDWriteTextFormat* f = Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, L"Segoe UI Variable Text", true);
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
    void Icon(const wchar_t* glyph, const D2D1_RECT_F& r, float size, D2D1_COLOR_F c) {
        IDWriteTextFormat* f = Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, IconFont());
        ID2D1SolidColorBrush* b = nullptr;
        if (f && SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawText(glyph, (UINT32)wcslen(glyph), f, r, b); b->Release(); }
        if (f) f->Release();
    }
    // Botão redondo de vidro com ícone.
    void CircleButton(const D2D1_RECT_F& r, const wchar_t* glyph, bool hot) {
        float rad = (r.bottom - r.top) / 2;
        Fill(r, rad, hot ? th.panelHot : th.panel);
        Stroke(r, rad, th.stroke);
        Icon(glyph, r, 14, hot ? th.text : th.text2);
    }
    // Interruptor estilo iOS.
    void Toggle(const D2D1_RECT_F& r, bool on, bool hot) {
        float h = r.bottom - r.top;
        Fill(r, h / 2, on ? th.accent : th.track);
        if (hot) Fill(r, h / 2, Rgba(0xFFFFFF, 0.10f));
        float k = h - 4 * s, x = on ? r.right - 2 * s - k : r.left + 2 * s;
        D2D1_RECT_F kr = D2D1::RectF(x, r.top + 2 * s, x + k, r.top + 2 * s + k);
        Fill(D2D1::RectF(kr.left, kr.top + 1 * s, kr.right, kr.bottom + 1 * s), k / 2, Rgba(0x000000, 0.18f));
        Fill(kr, k / 2, th.knob);
    }
    // Controle segmentado; retorna nada, quem chama guarda os retângulos de cada parte.
    void Segmented(const D2D1_RECT_F& r, const std::wstring* labels, int n, int sel, int hot, D2D1_RECT_F* parts) {
        float h = r.bottom - r.top;
        Fill(r, h / 2, th.track);
        float w = (r.right - r.left - 4 * s) / n;
        for (int i = 0; i < n; ++i) {
            D2D1_RECT_F p = D2D1::RectF(r.left + 2 * s + i * w, r.top + 2 * s, r.left + 2 * s + (i + 1) * w, r.bottom - 2 * s);
            parts[i] = p;
            if (i == sel) { Fill(p, (h - 4 * s) / 2, th.dark ? Rgba(0x636366) : Rgba(0xFFFFFF)); Stroke(p, (h - 4 * s) / 2, th.stroke); }
            else if (i == hot) Fill(p, (h - 4 * s) / 2, Rgba(th.dark ? 0xFFFFFF : 0x000000, 0.06f));
            Text(labels[i], p, 12.5f, i == sel ? th.text : th.text2, i == sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
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
        Fill(r, h / 2, primary ? th.accent : hot ? th.panelHot : th.panel);
        if (!primary) Stroke(r, h / 2, th.stroke);
        if (primary && hot) Fill(r, h / 2, Rgba(0xFFFFFF, 0.12f));
        Text(label, r, 12.5f, primary ? th.onAccent : th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
};

inline bool Hit(const D2D1_RECT_F& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

} // namespace ui
