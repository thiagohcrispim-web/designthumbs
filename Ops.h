// Ops.h - ferramentas de imagem (recortar, girar, espelhar, redimensionar, tons de cinza, fundo) e gravação
// em PNG, JPG, WEBP, TIFF (várias páginas), BMP, GIF e ICO. Bitmaps: DIB 32bpp top-down, pré-multiplicado.
#pragma once
#include <wincodec.h>
#include <algorithm>
#include <vector>
#include <string>
#include "Formats.h"
#include "third_party/libwebp/src/webp/encode.h"

namespace ops {
using std::min; using std::max;

enum Format { kPng, kJpg, kPdf, kWebp, kTiff, kBmp, kGif, kIco, kFormatCount };
inline const wchar_t* Ext(Format f) {
    static const wchar_t* e[] = {L".png", L".jpg", L".pdf", L".webp", L".tif", L".bmp", L".gif", L".ico"};
    return e[f];
}
inline const wchar_t* Name(Format f) {
    static const wchar_t* n[] = {L"PNG", L"JPG", L"PDF", L"WEBP", L"TIFF", L"BMP", L"GIF", L"ICO"};
    return n[f];
}
inline bool HasAlpha(Format f) { return f == kPng || f == kWebp || f == kTiff || f == kIco || f == kGif; }

struct Edit {
    UINT maxW = 0, maxH = 0;   // cabe dentro (0 = sem limite)
    UINT scalePct = 100;       // escala (aplicada antes do limite)
    int rotate = 0;            // 0, 90, 180, 270 (horário)
    bool flipH = false, flipV = false, gray = false;
    COLORREF bg = CLR_INVALID; // fundo para transparência (CLR_INVALID = manter; branco se o formato não tem alpha)
    bool crop = false;
    float cx0 = 0, cy0 = 0, cx1 = 1, cy1 = 1; // recorte normalizado (0..1)
    bool Any() const { return maxW || maxH || scalePct != 100 || rotate || flipH || flipV || gray || bg != CLR_INVALID || crop; }
};

inline bool Pixels(HBITMAP b, BYTE** px, UINT* w, UINT* h) {
    DIBSECTION ds;
    if (!b || GetObject(b, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) return false;
    *px = (BYTE*)ds.dsBm.bmBits; *w = ds.dsBm.bmWidth; *h = std::abs(ds.dsBm.bmHeight);
    return true;
}

inline IWICImagingFactory* Wic() {
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

// Aplica as edições e devolve um bitmap novo (o original fica intacto).
inline HBITMAP Apply(HBITMAP src, const Edit& e, bool needOpaque) {
    BYTE* px; UINT w, h;
    if (!Pixels(src, &px, &w, &h)) return nullptr;
    IWICImagingFactory* f = Wic();
    if (!f) return nullptr;
    IWICBitmap* wb = nullptr; IWICBitmapClipper* clip = nullptr; IWICBitmapScaler* sc = nullptr;
    HBITMAP out = nullptr;
    if (SUCCEEDED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4, px, &wb))) {
        IWICBitmapSource* cur = wb;
        UINT cw = w, ch = h;
        if (e.crop) {
            WICRect r = {(INT)(min(e.cx0, e.cx1) * w), (INT)(min(e.cy0, e.cy1) * h), 0, 0};
            r.Width = max(1, (INT)(fabs(e.cx1 - e.cx0) * w)); r.Height = max(1, (INT)(fabs(e.cy1 - e.cy0) * h));
            r.Width = min(r.Width, (INT)w - r.X); r.Height = min(r.Height, (INT)h - r.Y);
            if (SUCCEEDED(f->CreateBitmapClipper(&clip)) && SUCCEEDED(clip->Initialize(cur, &r))) { cur = clip; cw = r.Width; ch = r.Height; }
        }
        bool swap = e.rotate == 90 || e.rotate == 270; // giro e espelho são feitos depois, à mão
        UINT tw = swap ? ch : cw, th = swap ? cw : ch;        // tamanho final antes da escala
        double k = e.scalePct / 100.0;
        if (e.maxW && tw * k > e.maxW) k = (double)e.maxW / tw;
        if (e.maxH && th * k > e.maxH) k = min(k, (double)e.maxH / th);
        UINT nw = max(1u, (UINT)(cw * k + 0.5)), nh = max(1u, (UINT)(ch * k + 0.5));
        if ((nw != cw || nh != ch) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(cur, nw, nh, (WICBitmapInterpolationMode)4 /* HighQualityCubic (Windows 10) */)))
            cur = sc;
        else { nw = cw; nh = ch; }
        BYTE* bits;
        out = fmt::CreateDib(nw, nh, &bits);
        if (out && FAILED(cur->CopyPixels(nullptr, nw * 4, nw * nh * 4, bits))) { DeleteObject(out); out = nullptr; }
        if (out && (e.rotate || e.flipH || e.flipV)) {
            UINT rw = swap ? nh : nw, rh = swap ? nw : nh;
            BYTE* rb; HBITMAP rot2 = fmt::CreateDib(rw, rh, &rb);
            const UINT32* sp = (const UINT32*)bits; UINT32* dp = (UINT32*)rb;
            for (UINT y = 0; y < rh; ++y)
                for (UINT x = 0; x < rw; ++x) {
                    UINT dx = e.flipH ? rw - 1 - x : x, dy = e.flipV ? rh - 1 - y : y; // espelho no resultado
                    UINT sx, sy;
                    if (e.rotate == 90) { sx = dy; sy = nh - 1 - dx; }
                    else if (e.rotate == 180) { sx = nw - 1 - dx; sy = nh - 1 - dy; }
                    else if (e.rotate == 270) { sx = nw - 1 - dy; sy = dx; }
                    else { sx = dx; sy = dy; }
                    dp[(size_t)y * rw + x] = sp[(size_t)sy * nw + sx];
                }
            DeleteObject(out);
            out = rot2; bits = rb; nw = rw; nh = rh;
        }
        if (out) {
            COLORREF bg = e.bg != CLR_INVALID ? e.bg : needOpaque ? RGB(255, 255, 255) : CLR_INVALID;
            BYTE br = GetRValue(bg), bgG = GetGValue(bg), bb = GetBValue(bg);
            for (size_t i = 0, n = (size_t)nw * nh; i < n; ++i, bits += 4) {
                if (bg != CLR_INVALID) { // compõe sobre a cor de fundo (pixels pré-multiplicados)
                    BYTE a = 255 - bits[3];
                    bits[0] = (BYTE)(bits[0] + bb * a / 255); bits[1] = (BYTE)(bits[1] + bgG * a / 255); bits[2] = (BYTE)(bits[2] + br * a / 255); bits[3] = 255;
                }
                if (e.gray) { // luminância (Rec. 709)
                    BYTE y = (BYTE)((bits[2] * 54 + bits[1] * 183 + bits[0] * 19) >> 8);
                    bits[0] = bits[1] = bits[2] = y;
                }
            }
        }
    }
    if (sc) sc->Release(); if (clip) clip->Release(); if (wb) wb->Release();
    f->Release();
    return out;
}

// Pixels sem pré-multiplicação (RGBA ou BGRA) para encoders que não aceitam pré-multiplicado.
inline std::vector<BYTE> Straight(const BYTE* px, size_t n, bool rgba) {
    std::vector<BYTE> v(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const BYTE* s = px + i * 4; BYTE* d = &v[i * 4];
        BYTE a = s[3];
        auto un = [a](BYTE c) { return a ? (BYTE)min(255, c * 255 / a) : (BYTE)0; };
        BYTE b = un(s[0]), g = un(s[1]), r = un(s[2]);
        if (rgba) { d[0] = r; d[1] = g; d[2] = b; } else { d[0] = b; d[1] = g; d[2] = r; }
        d[3] = a;
    }
    return v;
}

inline bool WriteFileBytes(const std::wstring& path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data, (DWORD)n, &w, nullptr) && w == n;
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// WEBP otimizado para o menor arquivo (como os compressores online): método 6 (mais lento, menor),
// filtro automático e YUV "sharp". Em gráficos (poucas cores: logos, ícones, prints) também tenta sem perdas
// e fica com o que for menor. qualidade >= 0,995 = sempre sem perdas.
inline bool EncodeWebp(const std::vector<BYTE>& rgba, UINT w, UINT h, bool lossless, float quality, WebPMemoryWriter* mw) {
    WebPConfig cfg;
    if (!WebPConfigInit(&cfg)) return false;
    if (lossless) { WebPConfigLosslessPreset(&cfg, w * h > 4000000 ? 4 : 6); }
    else {
        WebPConfigPreset(&cfg, WEBP_PRESET_PHOTO, quality * 100);
        cfg.method = 6;
        cfg.autofilter = 1;
        cfg.use_sharp_yuv = 1;
        cfg.alpha_quality = 85;
    }
    cfg.thread_level = 1;
    if (!WebPValidateConfig(&cfg)) return false;
    WebPPicture pic;
    if (!WebPPictureInit(&pic)) return false;
    pic.width = w; pic.height = h;
    pic.use_argb = lossless ? 1 : 0;
    bool ok = WebPPictureImportRGBA(&pic, rgba.data(), w * 4) != 0;
    WebPMemoryWriterInit(mw);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = mw;
    ok = ok && WebPEncode(&cfg, &pic);
    WebPPictureFree(&pic);
    return ok;
}

inline bool SaveWebp(HBITMAP b, const std::wstring& path, float quality) {
    BYTE* px; UINT w, h;
    if (!Pixels(b, &px, &w, &h)) return false;
    std::vector<BYTE> rgba = Straight(px, (size_t)w * h, true);
    bool forceLossless = quality >= 0.995f;
    // poucas cores = gráfico: sem perdas costuma ser menor e mais nítido
    bool graphic = false;
    if (!forceLossless && (size_t)w * h <= 16000000) {
        std::vector<UINT32> seen;
        seen.reserve(4097);
        graphic = true;
        for (size_t i = 0; i < (size_t)w * h && graphic; i += 1 + (size_t)w * h / 400000) {
            UINT32 c; memcpy(&c, &rgba[i * 4], 4);
            if (std::find(seen.begin(), seen.end(), c) == seen.end()) { seen.push_back(c); if (seen.size() > 1024) graphic = false; }
        }
    }
    WebPMemoryWriter best, alt;
    bool ok = EncodeWebp(rgba, w, h, forceLossless, quality, &best);
    if (ok && graphic && !forceLossless && EncodeWebp(rgba, w, h, true, quality, &alt)) {
        if (alt.size < best.size) std::swap(best, alt);
        WebPMemoryWriterClear(&alt);
    }
    ok = ok && best.size && WriteFileBytes(path, best.mem, best.size);
    WebPMemoryWriterClear(&best);
    return ok;
}

// Codifica uma ou mais imagens com WIC (TIFF aceita várias páginas; GIF vira 256 cores).
inline bool SaveWic(const std::vector<HBITMAP>& frames, const std::wstring& path, Format fmtOut, float quality, double dpi) {
    const GUID& container = fmtOut == kJpg ? GUID_ContainerFormatJpeg : fmtOut == kTiff ? GUID_ContainerFormatTiff
                          : fmtOut == kBmp ? GUID_ContainerFormatBmp : fmtOut == kGif ? GUID_ContainerFormatGif : GUID_ContainerFormatPng;
    IWICImagingFactory* f = Wic();
    if (!f) return false;
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(container, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache));
    for (size_t i = 0; ok && i < frames.size(); ++i) {
        BYTE* px; UINT w, h;
        if (!Pixels(frames[i], &px, &w, &h)) { ok = false; break; }
        bool opaque = fmtOut == kJpg || fmtOut == kBmp;
        std::vector<BYTE> buf;
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        UINT stride = w * 4;
        if (opaque) { // 24 bits sobre branco
            stride = (w * 3 + 3) & ~3u;
            buf.resize((size_t)stride * h);
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x) {
                    const BYTE* s = px + ((size_t)y * w + x) * 4; BYTE* d = &buf[(size_t)y * stride + x * 3];
                    for (int c = 0; c < 3; ++c) d[c] = (BYTE)(s[c] + (255 - s[3]));
                }
            pf = GUID_WICPixelFormat24bppBGR;
        } else buf = Straight(px, (size_t)w * h, false);
        IWICBitmap* wb = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
        IWICFormatConverter* conv = nullptr; IWICPalette* pal = nullptr;
        ok = SUCCEEDED(f->CreateBitmapFromMemory(w, h, pf, stride, (UINT)buf.size(), buf.data(), &wb)) &&
             SUCCEEDED(enc->CreateNewFrame(&fr, &props));
        if (ok && props && fmtOut == kJpg) {
            PROPBAG2 opt = {}; opt.pstrName = (LPOLESTR)L"ImageQuality";
            VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = quality;
            props->Write(1, &opt, &v);
        }
        if (ok && props && fmtOut == kTiff) {
            PROPBAG2 opt = {}; opt.pstrName = (LPOLESTR)L"TiffCompressionMethod";
            VARIANT v; VariantInit(&v); v.vt = VT_UI1; v.bVal = WICTiffCompressionZIP;
            props->Write(1, &opt, &v);
        }
        ok = ok && SUCCEEDED(fr->Initialize(props));
        if (ok && dpi > 0) fr->SetResolution(dpi, dpi);
        IWICBitmapSource* src = wb;
        if (ok && fmtOut == kGif && SUCCEEDED(f->CreatePalette(&pal)) && SUCCEEDED(pal->InitializeFromBitmap(wb, 256, TRUE)) &&
            SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion, pal, 0, WICBitmapPaletteTypeCustom))) {
            fr->SetPalette(pal);
            src = conv;
        }
        ok = ok && SUCCEEDED(fr->WriteSource(src, nullptr)) && SUCCEEDED(fr->Commit());
        if (conv) conv->Release(); if (pal) pal->Release(); if (props) props->Release(); if (fr) fr->Release(); if (wb) wb->Release();
    }
    ok = ok && SUCCEEDED(enc->Commit());
    if (enc) enc->Release(); if (st) st->Release();
    f->Release();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// PNG em memória (usado no ICO).
inline bool PngBytes(HBITMAP b, std::vector<BYTE>& out) {
    wchar_t tmp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"dti", 0, file);
    DeleteFileW(file);
    bool ok = SaveWic({b}, file, kPng, 1, 0);
    if (ok) {
        HANDLE h = CreateFileW(file, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD size = GetFileSize(h, nullptr), got = 0;
        out.resize(size);
        ok = h != INVALID_HANDLE_VALUE && ReadFile(h, out.data(), size, &got, nullptr) && got == size;
        CloseHandle(h);
    }
    DeleteFileW(file);
    return ok;
}

// ICO com vários tamanhos (16 a 256), imagem centralizada num quadrado transparente.
inline bool SaveIco(HBITMAP b, const std::wstring& path) {
    BYTE* px; UINT w, h;
    if (!Pixels(b, &px, &w, &h)) return false;
    static const UINT sizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<std::vector<BYTE>> pngs;
    for (UINT s : sizes) {
        Edit e; e.maxW = s; e.maxH = s; e.scalePct = 100;
        if (w < s && h < s) e.scalePct = (UINT)(100.0 * s / max(w, h)); // amplia ícones pequenos
        HBITMAP fit = Apply(b, e, false);
        BYTE* fp; UINT fw, fh;
        if (!fit || !Pixels(fit, &fp, &fw, &fh)) { if (fit) DeleteObject(fit); return false; }
        BYTE* sq; HBITMAP square = fmt::CreateDib(s, s, &sq);
        memset(sq, 0, (size_t)s * s * 4);
        UINT ox = (s - min(fw, s)) / 2, oy = (s - min(fh, s)) / 2;
        for (UINT y = 0; y < min(fh, s); ++y) memcpy(sq + ((size_t)(y + oy) * s + ox) * 4, fp + (size_t)y * fw * 4, min(fw, s) * 4);
        std::vector<BYTE> png;
        bool ok = PngBytes(square, png);
        DeleteObject(square); DeleteObject(fit);
        if (!ok) return false;
        pngs.push_back(png);
    }
    std::vector<BYTE> ico(6 + 16 * pngs.size());
    ico[2] = 1; ico[4] = (BYTE)pngs.size();
    UINT32 off = (UINT32)ico.size();
    for (size_t i = 0; i < pngs.size(); ++i) {
        BYTE* d = &ico[6 + i * 16];
        d[0] = sizes[i] >= 256 ? 0 : (BYTE)sizes[i]; d[1] = d[0];
        d[4] = 1; d[6] = 32;
        UINT32 n = (UINT32)pngs[i].size();
        memcpy(d + 8, &n, 4); memcpy(d + 12, &off, 4);
        off += n;
    }
    for (auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
    return WriteFileBytes(path, ico.data(), ico.size());
}

// Grava `frames` no formato pedido. Formatos de uma página gravam só a primeira.
inline bool Save(const std::vector<HBITMAP>& frames, const std::wstring& path, Format f, float quality, double dpi) {
    if (frames.empty()) return false;
    if (f == kWebp) return SaveWebp(frames[0], path, quality);
    if (f == kIco) return SaveIco(frames[0], path);
    if (f == kTiff) return SaveWic(frames, path, f, quality, dpi);
    return SaveWic({frames[0]}, path, f, quality, dpi);
}

} // namespace ops
