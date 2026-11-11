// PaletteFont.h - paletas de cor (.ase, .aco) e fontes (.ttf, .otf, .ttc): desenhadas em bitmap com Direct2D/DirectWrite.
// Incluído por Formats.h (depende de CreateDib, Bytes e BE de lá).
#pragma once
#include <dwrite_3.h>

namespace fmt {

struct Swatch { std::wstring name; BYTE r, g, b; };

inline BYTE Clamp255(double v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

inline void LabToRgb(double L, double a, double bb, BYTE* r, BYTE* g, BYTE* b) {
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - bb / 200;
    auto f = [](double t) { return t > 0.206893 ? t * t * t : (t - 16.0 / 116) / 7.787; };
    double X = 0.95047 * f(fx), Y = f(fy), Z = 1.08883 * f(fz);
    double R = 3.2406 * X - 1.5372 * Y - 0.4986 * Z, G = -0.9689 * X + 1.8758 * Y + 0.0415 * Z, B = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    auto gam = [](double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055; };
    *r = Clamp255(gam(R) * 255); *g = Clamp255(gam(G) * 255); *b = Clamp255(gam(B) * 255);
}

inline void HsbToRgb(double h, double s, double v, BYTE* r, BYTE* g, BYTE* b) {
    double c = v * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = v - c, R = 0, G = 0, B = 0;
    int i = (int)(h / 60) % 6;
    if (i == 0) { R = c; G = x; } else if (i == 1) { R = x; G = c; } else if (i == 2) { G = c; B = x; }
    else if (i == 3) { G = x; B = c; } else if (i == 4) { R = x; B = c; } else { R = c; B = x; }
    *r = Clamp255((R + m) * 255); *g = Clamp255((G + m) * 255); *b = Clamp255((B + m) * 255);
}

inline std::wstring Utf16Be(const BYTE* p, size_t chars) {
    std::wstring s;
    for (size_t i = 0; i < chars; ++i) { wchar_t c = (wchar_t)(p[i * 2] << 8 | p[i * 2 + 1]); if (!c) break; s += c; }
    return s;
}

// Photoshop .aco (versão 1 e 2, com nomes).
inline bool ParseAco(const Bytes& b, std::vector<Swatch>& out) {
    size_t p = 0;
    auto u16 = [&](size_t o) { return o + 2 <= b.size() ? (UINT)BE(&b[o], 2) : 0u; };
    for (int pass = 0; pass < 2 && p + 4 <= b.size(); ++pass) {
        UINT ver = u16(p), count = u16(p + 2);
        if (ver != 1 && ver != 2) break;
        p += 4;
        std::vector<Swatch> list;
        for (UINT i = 0; i < count && p + 10 <= b.size(); ++i) {
            UINT space = u16(p), w = u16(p + 2), x = u16(p + 4), y = u16(p + 6), z = u16(p + 8);
            p += 10;
            Swatch s{L"", 0, 0, 0};
            if (space == 0) { s.r = (BYTE)(w >> 8); s.g = (BYTE)(x >> 8); s.b = (BYTE)(y >> 8); }
            else if (space == 1) HsbToRgb(w / 65535.0 * 360, x / 65535.0, y / 65535.0, &s.r, &s.g, &s.b);
            else if (space == 2) { // 0 = 100% de tinta
                double C = 1 - w / 65535.0, M = 1 - x / 65535.0, Y = 1 - y / 65535.0, K = 1 - z / 65535.0;
                s.r = Clamp255(255 * (1 - C) * (1 - K)); s.g = Clamp255(255 * (1 - M) * (1 - K)); s.b = Clamp255(255 * (1 - Y) * (1 - K));
            } else if (space == 7) LabToRgb(w / 100.0, (short)x / 100.0, (short)y / 100.0, &s.r, &s.g, &s.b);
            else if (space == 8) s.r = s.g = s.b = Clamp255(255 - w / 10000.0 * 255);
            if (ver == 2 && p + 8 <= b.size()) {
                size_t len = (size_t)BE(&b[p + 4], 4);
                p += 8;
                if (p + len * 2 > b.size()) return !out.empty();
                s.name = Utf16Be(&b[p], len);
                p += len * 2;
            }
            list.push_back(s);
        }
        if (!list.empty()) out.swap(list); // a seção v2 (com nomes) substitui a v1
    }
    return !out.empty();
}

// Adobe Swatch Exchange (.ase).
inline bool ParseAse(const Bytes& b, std::vector<Swatch>& out) {
    if (b.size() < 12 || memcmp(b.data(), "ASEF", 4) != 0) return false;
    UINT blocks = (UINT)BE(&b[8], 4);
    size_t p = 12;
    auto f32 = [&](size_t o) { UINT32 u = (UINT32)BE(&b[o], 4); float f; memcpy(&f, &u, 4); return (double)f; };
    for (UINT i = 0; i < blocks && p + 6 <= b.size(); ++i) {
        UINT type = (UINT)BE(&b[p], 2);
        size_t len = (size_t)BE(&b[p + 2], 4), q = p + 6;
        p = q + len;
        if (p > b.size()) break;
        if (type != 1 || q + 2 > p) continue;
        size_t nlen = (size_t)BE(&b[q], 2);
        q += 2;
        if (q + nlen * 2 + 4 > p) continue;
        Swatch s{Utf16Be(&b[q], nlen), 0, 0, 0};
        q += nlen * 2;
        char model[5] = {}; memcpy(model, &b[q], 4);
        q += 4;
        if (!memcmp(model, "RGB ", 4) && q + 12 <= p) { s.r = Clamp255(f32(q) * 255); s.g = Clamp255(f32(q + 4) * 255); s.b = Clamp255(f32(q + 8) * 255); }
        else if (!memcmp(model, "CMYK", 4) && q + 16 <= p) {
            double C = f32(q), M = f32(q + 4), Y = f32(q + 8), K = f32(q + 12);
            s.r = Clamp255(255 * (1 - C) * (1 - K)); s.g = Clamp255(255 * (1 - M) * (1 - K)); s.b = Clamp255(255 * (1 - Y) * (1 - K));
        } else if (!memcmp(model, "LAB ", 4) && q + 12 <= p) LabToRgb(f32(q) * 100, f32(q + 4), f32(q + 8), &s.r, &s.g, &s.b);
        else if (!memcmp(model, "Gray", 4) && q + 4 <= p) s.r = s.g = s.b = Clamp255(f32(q) * 255);
        else continue;
        out.push_back(s);
    }
    return !out.empty();
}

inline std::wstring Hex(const Swatch& s) { wchar_t t[16]; swprintf(t, 16, L"#%02X%02X%02X", s.r, s.g, s.b); return t; }

// ---------- desenho em bitmap ----------

// Superfície Direct2D sobre um bitmap WIC; Finish() devolve o HBITMAP.
class Canvas {
    IWICImagingFactory* wic_ = nullptr; ID2D1Factory* d2d_ = nullptr; IWICBitmap* bmp_ = nullptr;
public:
    ID2D1RenderTarget* rt = nullptr;
    IDWriteFactory* dw = nullptr;
    UINT w, h;
    Canvas(UINT W, UINT H) : w(W), h(H) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)))) return;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return;
        if (FAILED(wic_->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp_))) return;
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(d2d_->CreateWicBitmapRenderTarget(bmp_, p, &rt))) return;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw);
        rt->BeginDraw();
    }
    ~Canvas() {
        if (dw) dw->Release(); if (rt) rt->Release(); if (bmp_) bmp_->Release(); if (d2d_) d2d_->Release(); if (wic_) wic_->Release();
    }
    bool Ok() const { return rt && dw; }
    void Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b); b->Release(); }
    }
    void Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) { rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, 1); b->Release(); }
    }
    void Text(const std::wstring& t, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT wgt = DWRITE_FONT_WEIGHT_NORMAL,
              const wchar_t* family = L"Segoe UI", IDWriteFontCollection* coll = nullptr, DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL,
              DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL, bool wrap = false) {
        IDWriteTextFormat* f = nullptr;
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(dw->CreateTextFormat(family, coll, wgt, style, stretch, size, L"pt-br", &f)) && SUCCEEDED(rt->CreateSolidColorBrush(c, &b))) {
            if (!wrap) {
                f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                IDWriteInlineObject* e = nullptr;
                if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f, &e))) { f->SetTrimming(&trim, e); e->Release(); }
            }
            rt->DrawText(t.c_str(), (UINT32)t.size(), f, r, b);
        }
        if (b) b->Release(); if (f) f->Release();
    }
    HRESULT Finish(HBITMAP* out) {
        if (!rt) return E_FAIL;
        HRESULT hr = rt->EndDraw();
        if (FAILED(hr)) return hr;
        BYTE* bits; HBITMAP dib = CreateDib(w, h, &bits);
        if (!dib) return E_OUTOFMEMORY;
        hr = bmp_->CopyPixels(nullptr, w * 4, w * h * 4, bits);
        if (SUCCEEDED(hr)) *out = dib; else DeleteObject(dib);
        return hr;
    }
};

// Grade de amostras. Com espaço, cada uma ganha nome e HEX (no preview); pequena (miniatura), só as cores.
inline HRESULT RenderPalette(const std::vector<Swatch>& sw, UINT cx, HBITMAP* out) {
    size_t n = sw.size();
    bool labels = cx >= 400;
    UINT cols = (UINT)max(1.0, ceil(sqrt(n * (labels ? 1.6 : 1.0))));
    cols = min(cols, labels ? 8u : 12u);
    UINT rows = (UINT)((n + cols - 1) / cols);
    float pad = cx * (labels ? 0.03f : 0.05f), gap = cx * (labels ? 0.02f : 0.025f);
    float cell = (cx - 2 * pad - (cols - 1) * gap) / cols, textH = labels ? max(28.0f, cell * 0.42f) : 0;
    UINT H = (UINT)(2 * pad + rows * (cell + textH) + (rows - 1) * gap);
    if (!labels) H = max(H, cx); // miniatura quadrada
    H = min(H, 16384u);
    Canvas c(cx, H);
    if (!c.Ok()) return E_FAIL;
    c.rt->Clear(D2D1::ColorF(0xFFFFFF));
    float top = labels ? pad : (H - (rows * cell + (rows - 1) * gap)) / 2;
    for (size_t i = 0; i < n; ++i) {
        float x = pad + (i % cols) * (cell + gap), y = top + (i / cols) * (cell + textH + gap);
        if (y > H) break;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + cell, y + cell);
        c.Fill(r, cell * 0.14f, D2D1::ColorF(RGB(sw[i].b, sw[i].g, sw[i].r)));
        c.Stroke(r, cell * 0.14f, D2D1::ColorF(0x000000, 0.08f));
        if (labels) {
            float fs = max(10.0f, min(15.0f, cell * 0.11f));
            c.Text(sw[i].name.empty() ? Hex(sw[i]) : sw[i].name, D2D1::RectF(x, y + cell + 4, x + cell, y + cell + 4 + fs * 1.4f), fs, D2D1::ColorF(0x1D1D1F), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            if (!sw[i].name.empty()) c.Text(Hex(sw[i]), D2D1::RectF(x, y + cell + 4 + fs * 1.35f, x + cell, y + cell + 4 + fs * 2.8f), fs * 0.9f, D2D1::ColorF(0x6E6E73));
        }
    }
    return c.Finish(out);
}

// ---------- fontes ----------

class FontFile {
    IDWriteFactory5* f5_ = nullptr;
    IDWriteInMemoryFontFileLoader* loader_ = nullptr;
public:
    IDWriteFontCollection1* coll = nullptr;
    std::wstring family, style;
    UINT glyphs = 0;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE fstyle = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    bool Open(const Bytes& data) { // Windows 10 1703+
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5), (IUnknown**)&f5_))) return false;
        if (FAILED(f5_->CreateInMemoryFontFileLoader(&loader_)) || FAILED(f5_->RegisterFontFileLoader(loader_))) return false;
        IDWriteFontFile* file = nullptr; IDWriteFontSetBuilder1* b = nullptr; IDWriteFontSet* set = nullptr;
        bool ok = SUCCEEDED(loader_->CreateInMemoryFontFileReference(f5_, data.data(), (UINT32)data.size(), nullptr, &file)) &&
                  SUCCEEDED(f5_->CreateFontSetBuilder(&b)) && SUCCEEDED(b->AddFontFile(file)) && SUCCEEDED(b->CreateFontSet(&set)) &&
                  SUCCEEDED(f5_->CreateFontCollectionFromFontSet(set, &coll));
        if (ok) {
            IDWriteFontFamily1* fam = nullptr; IDWriteLocalizedStrings* names = nullptr; IDWriteFont3* font = nullptr;
            if (SUCCEEDED(coll->GetFontFamily(0, &fam)) && SUCCEEDED(fam->GetFamilyNames(&names))) {
                UINT32 len = 0; names->GetStringLength(0, &len);
                family.resize(len); names->GetString(0, &family[0], len + 1);
                names->Release(); names = nullptr;
                if (SUCCEEDED(fam->GetFont(0, &font))) {
                    weight = font->GetWeight(); fstyle = font->GetStyle(); stretch = font->GetStretch();
                    if (SUCCEEDED(font->GetFaceNames(&names))) {
                        names->GetStringLength(0, &len);
                        style.resize(len); names->GetString(0, &style[0], len + 1);
                        names->Release();
                    }
                    IDWriteFontFace3* face = nullptr;
                    if (SUCCEEDED(font->CreateFontFace(&face))) { glyphs = face->GetGlyphCount(); face->Release(); }
                    font->Release();
                }
            }
            if (fam) fam->Release();
        }
        if (set) set->Release(); if (b) b->Release(); if (file) file->Release();
        return ok && !family.empty();
    }
    ~FontFile() {
        if (coll) coll->Release();
        if (loader_) { f5_->UnregisterFontFileLoader(loader_); loader_->Release(); }
        if (f5_) f5_->Release();
    }
};

// Folha de amostra: nome, alfabeto e frases em vários tamanhos.
inline HRESULT RenderFont(const Bytes& data, UINT cx, HBITMAP* out) {
    FontFile ff;
    if (!ff.Open(data)) return E_FAIL;
    bool compactPreview = cx < 400;
    UINT W = cx, H = compactPreview ? cx : (UINT)(cx * 1.3f);
    Canvas c(W, H);
    if (!c.Ok()) return E_FAIL;
    c.rt->Clear(D2D1::ColorF(0xFFFFFF));
    D2D1_COLOR_F ink = D2D1::ColorF(0x1D1D1F), soft = D2D1::ColorF(0x6E6E73);
    auto sample = [&](const std::wstring& t, float y, float size, bool wrap = false, float hh = 0) {
        c.Text(t, D2D1::RectF(W * 0.06f, y, W * 0.94f, y + (hh ? hh : size * 1.5f)), size, ink, ff.weight, ff.family.c_str(), ff.coll, ff.fstyle, ff.stretch, wrap);
    };
    if (compactPreview) { // miniatura: "Aa" grande
        sample(L"Aa", H * 0.18f, W * 0.42f);
        c.Text(ff.family, D2D1::RectF(W * 0.06f, H * 0.78f, W * 0.94f, H * 0.94f), W * 0.08f, soft);
        return c.Finish(out);
    }
    float u = W / 100.0f, y = 5 * u;
    c.Text(ff.family + (ff.style.empty() ? L"" : L" · " + ff.style), D2D1::RectF(6 * u, y, 94 * u, y + 5 * u), 3.2f * u, soft, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    y += 7 * u;
    sample(L"Aa Bb Cc", y, 13 * u); y += 19 * u;
    sample(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ", y, 5.2f * u); y += 8 * u;
    sample(L"abcdefghijklmnopqrstuvwxyz", y, 5.2f * u); y += 8 * u;
    sample(L"0123456789 !?&@#%()", y, 5.2f * u); y += 10 * u;
    for (float s : {6.5f, 4.5f, 3.2f, 2.4f}) {
        sample(T(L"A rápida raposa marrom pula sobre o cão preguiçoso.", L"The quick brown fox jumps over the lazy dog.", L"El veloz murciélago hindú comía feliz cardillo y kiwi."), y, s * u, true, s * u * 3);
        y += s * u * (s > 4 ? 3.0f : 2.6f);
    }
    return c.Finish(out);
}

inline bool IsFontMagic(const BYTE* m) {
    return (m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || !memcmp(m, "OTTO", 4) || !memcmp(m, "true", 4) || !memcmp(m, "ttcf", 4);
}

} // namespace fmt
