// Convert.h - geração de imagens a partir de arquivos e conversão para PNG/JPG/PDF (usado pelo Vizu).
// Sem estado global: as funções rodam tanto na thread da janela quanto nas threads de trabalho.
#pragma once
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <functional>
#include "Formats.h"

namespace conv {
using std::min; using std::max;

template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline std::wstring Lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
inline std::wstring ExtOf(const std::wstring& path) { return Lower(PathFindExtensionW(path.c_str())); }

// Alguns bitmaps 32bpp vêm com alpha zerado; outros com alpha não pré-multiplicado. Retorna se há transparência.
inline bool PrepareAlpha(HBITMAP bmp) {
    DIBSECTION ds;
    if (GetObject(bmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) return false;
    BYTE* p = (BYTE*)ds.dsBm.bmBits;
    size_t n = (size_t)ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight);
    bool anyAlpha = false, translucent = false, premul = true;
    for (size_t i = 0; i < n; ++i) {
        BYTE a = p[i * 4 + 3];
        if (a) anyAlpha = true;
        if (a != 255) translucent = true;
        if (p[i * 4] > a || p[i * 4 + 1] > a || p[i * 4 + 2] > a) premul = false;
    }
    for (size_t i = 0; i < n; ++i, p += 4) {
        if (!anyAlpha) p[3] = 255;
        else if (!premul) { p[0] = p[0] * p[3] / 255; p[1] = p[1] * p[3] / 255; p[2] = p[2] * p[3] / 255; }
    }
    return anyAlpha && translucent;
}

// Converte bitmaps não-DIB (alguns providers) para DIB 32bpp top-down.
inline HBITMAP ToDib(HBITMAP src) {
    DIBSECTION ds;
    if (GetObject(src, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 32 && ds.dsBmih.biHeight < 0) return src;
    BITMAP bm;
    if (!GetObject(src, sizeof(bm), &bm)) return src;
    BYTE* bits;
    HBITMAP dib = fmt::CreateDib(bm.bmWidth, std::abs(bm.bmHeight), &bits);
    if (!dib) return src;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -std::abs(bm.bmHeight);
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    GetDIBits(dc, src, 0, std::abs(bm.bmHeight), bits, &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    DeleteObject(src);
    return dib;
}

// Arquivos .ai modernos são PDF: usa o gerador de miniaturas de PDF do Windows (Edge/Acrobat) em alta resolução.
inline HBITMAP PdfHandlerImage(const std::wstring& path, UINT cx) {
    static const wchar_t* kThumbShellEx = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
    wchar_t clsidStr[64] = {}, prog[256] = {};
    std::wstring keys[3] = {std::wstring(L".pdf") + kThumbShellEx, L"", std::wstring(L"SystemFileAssociations\\.pdf") + kThumbShellEx};
    DWORD psz = sizeof(prog);
    if (RegGetValueW(HKEY_CLASSES_ROOT, L".pdf", nullptr, RRF_RT_REG_SZ, nullptr, prog, &psz) == ERROR_SUCCESS)
        keys[1] = std::wstring(prog) + kThumbShellEx;
    bool found = false;
    for (auto& k : keys) {
        DWORD sz = sizeof(clsidStr);
        if (!k.empty() && RegGetValueW(HKEY_CLASSES_ROOT, k.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, clsidStr, &sz) == ERROR_SUCCESS) { found = true; break; }
    }
    CLSID clsid;
    if (!found || FAILED(CLSIDFromString(clsidStr, &clsid))) return nullptr;
    IThumbnailProvider* tp = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&tp)))) return nullptr;
    bool init = false;
    IInitializeWithStream* is = nullptr; IInitializeWithItem* ii = nullptr; IInitializeWithFile* ifl = nullptr;
    IStream* stream = nullptr;
    if (SUCCEEDED(tp->QueryInterface(IID_PPV_ARGS(&is))) &&
        SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)))
        init = SUCCEEDED(is->Initialize(stream, STGM_READ));
    if (!init && SUCCEEDED(tp->QueryInterface(IID_PPV_ARGS(&ii)))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            init = SUCCEEDED(ii->Initialize(item, STGM_READ));
            item->Release();
        }
    }
    if (!init && SUCCEEDED(tp->QueryInterface(IID_PPV_ARGS(&ifl)))) init = SUCCEEDED(ifl->Initialize(path.c_str(), STGM_READ));
    HBITMAP bmp = nullptr;
    WTS_ALPHATYPE at;
    if (init && FAILED(tp->GetThumbnail(cx, &bmp, &at))) bmp = nullptr;
    SafeRelease(stream); SafeRelease(is); SafeRelease(ii); SafeRelease(ifl); tp->Release();
    return bmp;
}

// Imagem de um arquivo com lado máximo cx: formatos próprios, PDF do .ai ou o Windows (JPG, PNG, WebP, HEIC...).
inline bool RenderFile(const std::wstring& path, UINT cx, int page, HBITMAP* out, bool* alpha, int* pages) {
    std::wstring ext = ExtOf(path);
    HBITMAP bmp = nullptr;
    *alpha = false;
    int n = 1;
    if (fmt::Handles(ext)) {
        IStream* s = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) {
            if (FAILED(fmt::Render(s, ext, cx, page, &bmp, alpha, &n, true))) bmp = nullptr;
            s->Release();
        }
        // .ai sem imagem nenhuma: gerador de miniaturas de PDF do Windows (Edge/Acrobat)
        if (ext == L".ai" && !bmp)
            if (HBITMAP x = PdfHandlerImage(path, cx)) { bmp = ToDib(x); *alpha = PrepareAlpha(bmp); }
    }
    if (!bmp) {
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            if (FAILED(f->GetImage({(LONG)cx, (LONG)cx}, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bmp))) bmp = nullptr;
            f->Release();
        }
        if (bmp) { bmp = ToDib(bmp); *alpha = PrepareAlpha(bmp); }
    }
    if (pages) *pages = n;
    *out = bmp;
    return bmp != nullptr;
}

// Ícone grande, para arquivos sem preview.
inline HBITMAP IconImage(const std::wstring& path) {
    HBITMAP bmp = nullptr;
    IShellItemImageFactory* f = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
        if (FAILED(f->GetImage({256, 256}, SIIGBF_ICONONLY, &bmp))) bmp = nullptr;
        f->Release();
    }
    if (bmp) { bmp = ToDib(bmp); PrepareAlpha(bmp); }
    return bmp;
}

// Miniatura já em cache do Explorer (instantânea), para abrir o preview sem esperar.
inline HBITMAP CachedThumb(const std::wstring& path) {
    HBITMAP bmp = nullptr;
    IShellItemImageFactory* f = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
        if (FAILED(f->GetImage({256, 256}, SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY | SIIGBF_BIGGERSIZEOK, &bmp))) bmp = nullptr;
        f->Release();
    }
    if (bmp) { bmp = ToDib(bmp); PrepareAlpha(bmp); }
    return bmp;
}

// Informações de qualquer arquivo (formatos próprios ou via WIC).
inline void FileInfo(const std::wstring& path, fmt::Info* in) {
    std::wstring ext = ExtOf(path);
    if (fmt::Handles(ext)) {
        IStream* s = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) {
            fmt::GetInfo(s, ext, in);
            s->Release();
        }
        return;
    }
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr))) {
        fr->GetSize(&in->w, &in->h);
        double dx = 0, dy = 0;
        if (SUCCEEDED(fr->GetResolution(&dx, &dy)) && dx > 1 && dx != 96) in->dpi = dx;
        WICPixelFormatGUID pf;
        IWICComponentInfo* ci = nullptr; IWICPixelFormatInfo2* pi = nullptr;
        if (SUCCEEDED(fr->GetPixelFormat(&pf)) && SUCCEEDED(f->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci->QueryInterface(IID_PPV_ARGS(&pi)))) {
            UINT bpp = 0, ch = 0;
            pi->GetBitsPerPixel(&bpp); pi->GetChannelCount(&ch);
            if (ch) in->bits = bpp / ch;
            WICPixelFormatNumericRepresentation nr;
            if (ch >= 4 && (IsEqualGUID(pf, GUID_WICPixelFormat32bppCMYK) || IsEqualGUID(pf, GUID_WICPixelFormat64bppCMYK))) in->color = L"CMYK";
            else if (ch == 1 && SUCCEEDED(pi->GetNumericRepresentation(&nr)) && nr != WICPixelFormatNumericRepresentationIndexed) in->color = T(L"Escala de cinza", L"Grayscale", L"Escala de grises");
            else if (ch >= 3) in->color = ch == 4 ? L"RGBA" : L"RGB";
        }
        SafeRelease(pi); SafeRelease(ci);
    }
    SafeRelease(fr); SafeRelease(dec); f->Release();
}

// Maior tamanho útil para exportar: o nativo, quando conhecido.
inline UINT ExportSize(const fmt::Info& in) {
    if (in.w && in.h && in.note.empty()) return min(16384u, max(in.w, in.h));
    return 4096;
}

// ---------- gravação ----------

inline bool PixelsOf(HBITMAP bmp, BYTE** px, UINT* w, UINT* h) {
    DIBSECTION ds;
    if (!bmp || GetObject(bmp, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) return false;
    *px = (BYTE*)ds.dsBm.bmBits; *w = ds.dsBm.bmWidth; *h = std::abs(ds.dsBm.bmHeight);
    return true;
}

// Codifica com WIC. JPEG: compõe sobre branco (sem transparência).
inline bool Encode(HBITMAP bmp, const GUID& container, float quality, IStream* out, double dpi = 0) {
    BYTE* px; UINT w, h;
    if (!PixelsOf(bmp, &px, &w, &h)) return false;
    bool jpeg = IsEqualGUID(container, GUID_ContainerFormatJpeg);
    std::vector<BYTE> rgb;
    if (jpeg) {
        size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
        rgb.resize(stride * h);
        for (UINT y = 0; y < h; ++y) {
            const BYTE* s = px + (size_t)y * w * 4;
            BYTE* d = &rgb[y * stride];
            for (UINT x = 0; x < w; ++x, s += 4, d += 3)
                for (int c = 0; c < 3; ++c) d[c] = (BYTE)(s[c] + (255 - s[3]));
        }
    }
    IWICImagingFactory* f = nullptr; IWICBitmap* wb = nullptr; IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(jpeg ? f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat24bppBGR, (UINT)(rgb.size() / h), (UINT)rgb.size(), rgb.data(), &wb)
                             : f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4, px, &wb)) &&
              SUCCEEDED(f->CreateEncoder(container, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(out, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, &props));
    if (ok && jpeg && props) {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = quality;
        props->Write(1, &opt, &v);
    }
    ok = ok && SUCCEEDED(fr->Initialize(props));
    if (ok && dpi > 0) fr->SetResolution(dpi, dpi);
    ok = ok && SUCCEEDED(fr->WriteSource(wb, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    SafeRelease(props); SafeRelease(fr); SafeRelease(enc); SafeRelease(wb); SafeRelease(f);
    return ok;
}


// "pasta\nome.png" sem sobrescrever: "nome (2).png", "nome (3).png"...
inline std::wstring UniquePath(const std::wstring& dir, const std::wstring& base, const wchar_t* ext) {
    std::wstring p = dir + L"\\" + base + ext;
    for (int i = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        p = dir + L"\\" + base + L" (" + std::to_wstring(i) + L")" + ext;
    return p;
}

inline std::wstring DirOf(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
inline std::wstring BaseOf(const std::wstring& path) {
    std::wstring n = PathFindFileNameW(path.c_str());
    size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos ? n : n.substr(0, dot);
}

// ---------- PDF mínimo: uma imagem JPEG por página ----------

class PdfWriter {
    std::string out_;
    std::vector<size_t> offsets_;
    std::vector<int> pages_;
    int NewObj() { offsets_.push_back(out_.size()); int id = (int)offsets_.size(); out_ += std::to_string(id) + " 0 obj\n"; return id; }
public:
    PdfWriter() { out_ = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n"; offsets_ = {0, 0}; } // 1 = catálogo, 2 = páginas (escritos no fim)
    // Página com tamanho em pontos (1/72 pol). jpeg = false: pixels RGB sem compressão.
    void AddImagePage(const std::vector<BYTE>& jpg, UINT w, UINT h, double ptW, double ptH, bool jpeg = true) {
        char buf[256];
        int img = NewObj();
        snprintf(buf, sizeof(buf), "<< /Type /XObject /Subtype /Image /Width %u /Height %u /ColorSpace /DeviceRGB /BitsPerComponent 8 %s/Length %zu >>\nstream\n",
                 w, h, jpeg ? "/Filter /DCTDecode " : "", jpg.size());
        out_ += buf;
        out_.append((const char*)jpg.data(), jpg.size());
        out_ += "\nendstream\nendobj\n";
        snprintf(buf, sizeof(buf), "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q", ptW, ptH);
        std::string content = buf;
        int cnt = NewObj();
        out_ += "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream\nendobj\n";
        int page = NewObj();
        snprintf(buf, sizeof(buf), "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n", ptW, ptH, img, cnt);
        out_ += buf;
        pages_.push_back(page);
    }
    size_t PageCount() const { return pages_.size(); }
    bool Save(const std::wstring& path) {
        offsets_[0] = out_.size();
        out_ += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
        offsets_[1] = out_.size();
        out_ += "2 0 obj\n<< /Type /Pages /Count " + std::to_string(pages_.size()) + " /Kids [";
        for (int p : pages_) out_ += std::to_string(p) + " 0 R ";
        out_ += "] >>\nendobj\n";
        size_t xref = out_.size();
        out_ += "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
        char line[32];
        for (size_t o : offsets_) { snprintf(line, sizeof(line), "%010zu 00000 n \n", o); out_ += line; }
        out_ += "trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(f, out_.data(), (DWORD)out_.size(), &written, nullptr) && written == out_.size();
        CloseHandle(f);
        return ok;
    }
};

// Pixels RGB (linhas de cima para baixo) compostos sobre branco, para o PDF sem compressão.
inline bool RgbBytes(HBITMAP bmp, std::vector<BYTE>& out) {
    BYTE* px; UINT w, h;
    if (!PixelsOf(bmp, &px, &w, &h)) return false;
    out.resize((size_t)w * h * 3);
    BYTE* d = out.data();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i, px += 4, d += 3) {
        d[0] = (BYTE)(px[2] + (255 - px[3])); d[1] = (BYTE)(px[1] + (255 - px[3])); d[2] = (BYTE)(px[0] + (255 - px[3]));
    }
    return true;
}

inline bool JpegBytes(HBITMAP bmp, float quality, std::vector<BYTE>& out) {
    IStream* s = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &s))) return false;
    bool ok = Encode(bmp, GUID_ContainerFormatJpeg, quality, s);
    if (ok) {
        STATSTG st; s->Stat(&st, STATFLAG_NONAME);
        out.resize((size_t)st.cbSize.QuadPart);
        ok = fmt::ReadAt(s, 0, out.data(), out.size());
    }
    s->Release();
    return ok;
}

// Pixels a gerar: documentos com tamanho físico (INDD, AI, SVG, PDF) seguem o DPI escolhido;
// imagens (PSD, JPG, RAW...) usam a resolução nativa.
inline UINT TargetSize(const fmt::Info& in, UINT dpi) {
    if (in.pageWmm > 0 && in.pageHmm > 0) return max(256u, min(16384u, (UINT)(max(in.pageWmm, in.pageHmm) / 25.4 * dpi + 0.5)));
    return ExportSize(in);
}

} // namespace conv
