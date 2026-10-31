// DesignThumbs - thumbnail provider leve para o Explorer do Windows.
// Suporta: .psd .psb (composite 8/16 bits RGB/Gray/CMYK), .ai .indd .indt .eps (miniatura XMP), .svg (Direct2D)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000003 // Windows 10 1703 (SVG no Direct2D)
#endif
#include <windows.h>
#include <shlobj.h>
#include <olectl.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include <algorithm>
#include <new>
using std::min; using std::max;
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d2d1.lib")

// {9E85547E-13C6-4C5E-95CC-990C74D9BB3F}
static const CLSID CLSID_DesignThumbs = {0x9e85547e, 0x13c6, 0x4c5e, {0x95, 0xcc, 0x99, 0x0c, 0x74, 0xd9, 0xbb, 0x3f}};
static const wchar_t* kClsidStr = L"{9E85547E-13C6-4C5E-95CC-990C74D9BB3F}";
static const wchar_t* kExts[] = {L".psd", L".psb", L".ai", L".indd", L".indt", L".eps", L".svg"};

static HMODULE g_module;
static LONG g_refs;

// ---------- utilidades ----------

static HBITMAP CreateDib(UINT w, UINT h, BYTE** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)bits, nullptr, 0);
}

static void FitSize(UINT w, UINT h, UINT cx, UINT* ow, UINT* oh) {
    if (w <= cx && h <= cx) { *ow = w; *oh = h; return; }
    if (w >= h) { *ow = cx; *oh = max(1u, (UINT)((UINT64)h * cx / w)); }
    else { *oh = cx; *ow = max(1u, (UINT)((UINT64)w * cx / h)); }
}

// Decodifica qualquer imagem suportada pelo WIC (JPEG, PNG...) para um HBITMAP de até cx px.
static HRESULT DecodeWithWic(const BYTE* data, size_t size, UINT cx, HBITMAP* out) {
    IWICImagingFactory* f = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) return hr;
    IStream* s = SHCreateMemStream(data, (UINT)size);
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* sc = nullptr; IWICFormatConverter* conv = nullptr;
    hr = s ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    UINT w = 0, h = 0, ow, oh;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (SUCCEEDED(hr)) { FitSize(w, h, cx, &ow, &oh); hr = f->CreateBitmapScaler(&sc); }
    if (SUCCEEDED(hr)) hr = sc->Initialize(frame, ow, oh, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        BYTE* bits; HBITMAP bmp = CreateDib(ow, oh, &bits);
        hr = bmp ? conv->CopyPixels(nullptr, ow * 4, ow * oh * 4, bits) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) *out = bmp; else if (bmp) DeleteObject(bmp);
    }
    if (conv) conv->Release(); if (sc) sc->Release(); if (frame) frame->Release();
    if (dec) dec->Release(); if (s) s->Release(); f->Release();
    return hr;
}

// ---------- AI / INDD / EPS: miniatura JPEG em base64 dentro do XMP ----------

static int B64(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62; if (c == '/') return 63;
    return -1;
}

static HRESULT XmpThumbnail(const std::vector<BYTE>& buf, UINT cx, HBITMAP* out) {
    const char* p = (const char*)buf.data();
    const char* end = p + buf.size();
    static const char tag[] = "xmpGImg:image";
    HRESULT hr = E_FAIL;
    // O arquivo pode ter várias miniaturas (páginas/pranchetas); usa a maior.
    std::vector<BYTE> best;
    bool bestComplete = false;
    for (const char* it = p; ; ) {
        it = (const char*)memchr(it, 'x', end - it);
        if (!it || end - it < (ptrdiff_t)sizeof(tag)) break;
        if (memcmp(it, tag, sizeof(tag) - 1) != 0) { ++it; continue; }
        it += sizeof(tag) - 1;
        if (*it == '>') ++it;                       // <xmpGImg:image>...</xmpGImg:image>
        else if (*it == '=' && it + 1 < end) it += 2; // xmpGImg:image="..."
        else continue;
        std::vector<BYTE> jpg;
        unsigned acc = 0; int bits = 0;
        for (; it < end && *it != '<' && *it != '"'; ++it) {
            if (*it == '&') { while (it < end && *it != ';') ++it; continue; } // &#xA;
            int v = B64(*it);
            if (v < 0) continue;
            acc = (acc << 6) | v; bits += 6;
            if (bits >= 8) { bits -= 8; jpg.push_back((BYTE)(acc >> bits)); }
        }
        // Arquivos salvos várias vezes guardam cópias antigas/truncadas: prefere JPEG completo.
        while (!jpg.empty() && jpg.back() == 0) jpg.pop_back();
        bool complete = jpg.size() > 4 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[jpg.size() - 2] == 0xFF && jpg.back() == 0xD9;
        if (complete > bestComplete || (complete == bestComplete && jpg.size() > best.size())) { best.swap(jpg); bestComplete = complete; }
    }
    if (!best.empty()) hr = DecodeWithWic(best.data(), best.size(), cx, out);
    return hr;
}

// ---------- PSD / PSB ----------
// Lido direto do stream com Seek: só baixa/lê os trechos necessários, mesmo em arquivos
// de centenas de MB ou em unidades na nuvem (Google Drive, OneDrive).

static bool ReadAt(IStream* s, UINT64 off, void* dst, size_t n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (FAILED(s->Seek(li, STREAM_SEEK_SET, nullptr))) return false;
    BYTE* p = (BYTE*)dst;
    while (n) {
        ULONG got = 0;
        HRESULT hr = s->Read(p, (ULONG)min(n, (size_t)1 << 24), &got);
        if (FAILED(hr) || got == 0) return false;
        p += got; n -= got;
    }
    return true;
}

static UINT64 BE(const BYTE* p, int bytes) { UINT64 v = 0; for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i]; return v; }

static bool ReadBE(IStream* s, UINT64 off, int bytes, UINT64* v) {
    BYTE b[8];
    if (!ReadAt(s, off, b, bytes)) return false;
    *v = BE(b, bytes);
    return true;
}

// Usa a imagem composta (alta qualidade). Lê apenas as linhas necessárias.
static HRESULT PsdComposite(IStream* s, UINT cx, HBITMAP* out) {
    BYTE hd[26];
    if (!ReadAt(s, 0, hd, 26) || memcmp(hd, "8BPS", 4) != 0) return E_FAIL;
    bool psb = BE(hd + 4, 2) == 2;
    UINT channels = (UINT)BE(hd + 12, 2), h = (UINT)BE(hd + 14, 4), w = (UINT)BE(hd + 18, 4);
    UINT depth = (UINT)BE(hd + 22, 2), mode = (UINT)BE(hd + 24, 2);
    if ((depth != 8 && depth != 16) || w == 0 || h == 0) return E_FAIL;
    if (mode != 1 && mode != 3 && mode != 4) return E_FAIL; // Gray, RGB, CMYK
    UINT64 pos = 26, len;
    for (int section = 0; section < 3; ++section) {          // color mode, resources, layers
        int lenBytes = (section == 2 && psb) ? 8 : 4;
        if (!ReadBE(s, pos, lenBytes, &len)) return E_FAIL;
        pos += lenBytes + len;
    }
    UINT64 comp;
    if (!ReadBE(s, pos, 2, &comp) || comp > 1) return E_FAIL;
    pos += 2;

    UINT need = mode == 3 ? 3 : mode == 4 ? 4 : 1;
    if (channels < need) return E_FAIL;
    bool alpha = channels > need;
    UINT use = alpha ? need + 1 : need;
    UINT bpc = depth / 8;
    size_t rowBytes = (size_t)w * bpc;
    size_t usedRows = (size_t)use * h;

    // offset de cada linha de cada canal
    std::vector<UINT64> rowOff(usedRows);
    std::vector<UINT> rowLen(usedRows);
    if (comp == 0) {
        UINT64 dataStart = pos;
        for (size_t i = 0; i < usedRows; ++i) { rowOff[i] = dataStart + (UINT64)i * rowBytes; rowLen[i] = (UINT)rowBytes; }
    } else {
        int cb = psb ? 4 : 2;
        UINT64 totalRows = (UINT64)channels * h;
        std::vector<BYTE> counts((size_t)(usedRows * cb));
        if (!ReadAt(s, pos, counts.data(), counts.size())) return E_FAIL;
        UINT64 data = pos + totalRows * cb;
        for (size_t i = 0; i < usedRows; ++i) {
            rowOff[i] = data;
            rowLen[i] = (UINT)BE(&counts[i * cb], cb);
            data += rowLen[i];
        }
    }

    UINT ow, oh;
    FitSize(w, h, cx, &ow, &oh);
    BYTE* bits; HBITMAP bmp = CreateDib(ow, oh, &bits);
    if (!bmp) return E_OUTOFMEMORY;
    std::vector<BYTE> row(rowBytes), src;
    std::vector<BYTE> plane((size_t)use * ow);

    for (UINT y = 0; y < oh; ++y) {
        UINT sy = (UINT)((UINT64)y * h / oh);
        for (UINT c = 0; c < use; ++c) {
            size_t idx = (size_t)c * h + sy;
            size_t n = rowLen[idx];
            src.resize(n);
            if (n && !ReadAt(s, rowOff[idx], src.data(), n)) { DeleteObject(bmp); return E_FAIL; }
            const BYTE* rowData = src.data();
            if (comp == 1) { // PackBits
                size_t i = 0, o = 0;
                memset(row.data(), 0, rowBytes);
                while (i < n && o < rowBytes) {
                    signed char k = (signed char)src[i++];
                    if (k >= 0) { size_t cnt = min((size_t)k + 1, min(rowBytes - o, n - i)); memcpy(&row[o], &src[i], cnt); i += k + 1; o += cnt; }
                    else if (k != -128) { if (i >= n) break; size_t cnt = min((size_t)(1 - k), rowBytes - o); memset(&row[o], src[i++], cnt); o += cnt; }
                }
                rowData = row.data();
            } else if (n < rowBytes) { DeleteObject(bmp); return E_FAIL; }
            for (UINT x = 0; x < ow; ++x) {
                size_t sx = (size_t)((UINT64)x * w / ow);
                plane[(size_t)c * ow + x] = rowData[sx * bpc]; // byte alto basta para miniatura
            }
        }
        BYTE* d = bits + (size_t)y * ow * 4;
        for (UINT x = 0; x < ow; ++x, d += 4) {
            BYTE R, G, B;
            if (mode == 3) { R = plane[x]; G = plane[ow + x]; B = plane[2 * ow + x]; }
            else if (mode == 1) { R = G = B = plane[x]; }
            else { // CMYK do PSD é armazenado invertido (255 = sem tinta)
                UINT C = plane[x], M = plane[ow + x], Y = plane[2 * ow + x], K = plane[3 * ow + x];
                R = (BYTE)(C * K / 255); G = (BYTE)(M * K / 255); B = (BYTE)(Y * K / 255);
            }
            BYTE A = alpha ? plane[need * ow + x] : 255;
            // compõe sobre branco
            d[0] = (BYTE)((B * A + 255 * (255 - A)) / 255);
            d[1] = (BYTE)((G * A + 255 * (255 - A)) / 255);
            d[2] = (BYTE)((R * A + 255 * (255 - A)) / 255);
            d[3] = 255;
        }
    }
    *out = bmp;
    return S_OK;
}

// Fallback: miniatura JPEG embutida (resource 1036/1033).
static HRESULT PsdEmbeddedThumb(IStream* s, UINT cx, HBITMAP* out) {
    UINT64 cm, rl;
    if (!ReadBE(s, 26, 4, &cm) || !ReadBE(s, 30 + cm, 4, &rl) || rl > (64u << 20)) return E_FAIL;
    std::vector<BYTE> res((size_t)rl);
    if (!ReadAt(s, 34 + cm, res.data(), res.size())) return E_FAIL;
    size_t pos = 0, end = res.size();
    while (pos + 12 <= end) {
        if (memcmp(&res[pos], "8BIM", 4) != 0) return E_FAIL;
        UINT id = (UINT)BE(&res[pos + 4], 2);
        pos += 6;
        pos += (res[pos] + 2) & ~1u;
        if (pos + 4 > end) return E_FAIL;
        size_t len = (size_t)BE(&res[pos], 4);
        pos += 4;
        if (pos + len > end) return E_FAIL;
        if ((id == 1036 || id == 1033) && len > 28)
            return DecodeWithWic(&res[pos + 28], len - 28, cx, out);
        pos += (len + 1) & ~(size_t)1;
    }
    return E_FAIL;
}

// ---------- SVG via Direct2D (Windows 10 1703+) ----------

static HRESULT SvgRender(const std::vector<BYTE>& buf, UINT cx, HBITMAP* out) {
    IWICImagingFactory* wic = nullptr; ID2D1Factory* d2d = nullptr; IWICBitmap* wbmp = nullptr;
    ID2D1RenderTarget* rt = nullptr; ID2D1DeviceContext5* dc = nullptr;
    ID2D1SvgDocument* doc = nullptr; ID2D1SvgElement* root = nullptr; IStream* s = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    if (SUCCEEDED(hr)) hr = wic->CreateBitmap(cx, cx, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wbmp);
    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = d2d->CreateWicBitmapRenderTarget(wbmp, p, &rt);
    }
    if (SUCCEEDED(hr)) hr = rt->QueryInterface(IID_PPV_ARGS(&dc));
    if (SUCCEEDED(hr)) { s = SHCreateMemStream(buf.data(), (UINT)buf.size()); hr = s ? S_OK : E_OUTOFMEMORY; }
    if (SUCCEEDED(hr)) hr = dc->CreateSvgDocument(s, D2D1::SizeF((float)cx, (float)cx), &doc);
    if (SUCCEEDED(hr)) {
        doc->GetRoot(&root);
        // Sem viewBox, cria um a partir de width/height para escalar ao tamanho da miniatura.
        if (root && !root->IsAttributeSpecified(L"viewBox")) {
            float w = 0, h = 0;
            root->GetAttributeValue(L"width", &w); root->GetAttributeValue(L"height", &h);
            if (w > 0 && h > 0) {
                D2D1_SVG_VIEWBOX vb = {0, 0, w, h};
                root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb));
            }
        }
        if (root) { root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, L"100%"); root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, L"100%"); }
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawSvgDocument(doc);
        hr = dc->EndDraw();
    }
    if (SUCCEEDED(hr)) {
        BYTE* bits; HBITMAP bmp = CreateDib(cx, cx, &bits);
        hr = bmp ? wbmp->CopyPixels(nullptr, cx * 4, cx * cx * 4, bits) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) *out = bmp; else if (bmp) DeleteObject(bmp);
    }
    if (root) root->Release(); if (doc) doc->Release(); if (s) s->Release(); if (dc) dc->Release();
    if (rt) rt->Release(); if (wbmp) wbmp->Release(); if (d2d) d2d->Release(); if (wic) wic->Release();
    return hr;
}

// ---------- COM: provider ----------

class Provider : public IThumbnailProvider, public IInitializeWithStream {
    LONG ref_ = 1;
    IStream* stream_ = nullptr;
public:
    Provider() { InterlockedIncrement(&g_refs); }
    ~Provider() { if (stream_) stream_->Release(); InterlockedDecrement(&g_refs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(Provider, IThumbnailProvider), QITABENT(Provider, IInitializeWithStream), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }

    IFACEMETHODIMP Initialize(IStream* s, DWORD) {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = s; s->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) {
        if (!stream_) return E_UNEXPECTED;
        BYTE magic[4] = {};
        *alpha = WTSAT_RGB;
        HRESULT hr = E_FAIL;
        if (ReadAt(stream_, 0, magic, 4) && memcmp(magic, "8BPS", 4) == 0) {
            hr = PsdComposite(stream_, cx, phbmp);
            if (FAILED(hr)) hr = PsdEmbeddedThumb(stream_, cx, phbmp);
            return hr;
        }

        // AI / INDD / EPS / SVG: lê o arquivo inteiro (limite de 512 MB)
        STATSTG st = {};
        if (FAILED(stream_->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart < 8 || st.cbSize.QuadPart > (512ull << 20))
            return E_FAIL;
        std::vector<BYTE> buf((size_t)st.cbSize.QuadPart);
        if (!ReadAt(stream_, 0, buf.data(), buf.size())) return E_FAIL;
        hr = XmpThumbnail(buf, cx, phbmp);
        if (FAILED(hr)) { // provavelmente SVG
            hr = SvgRender(buf, cx, phbmp);
            if (SUCCEEDED(hr)) *alpha = WTSAT_ARGB;
        }
        return hr;
    }
};

class Factory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(Factory, IClassFactory), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return 2; }
    IFACEMETHODIMP_(ULONG) Release() { return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Provider* p = new (std::nothrow) Provider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) { lock ? InterlockedIncrement(&g_refs) : InterlockedDecrement(&g_refs); return S_OK; }
};
static Factory g_factory;

// ---------- exports ----------

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_module = m; DisableThreadLibraryCalls(m); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_DesignThumbs) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

static LSTATUS SetKey(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (s == ERROR_SUCCESS) {
        s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    return s;
}

static const wchar_t* kThumbKey = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring clsid = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
    if (SetKey(clsid, nullptr, L"DesignThumbs Thumbnail Provider") ||
        SetKey(clsid + L"\\InprocServer32", nullptr, path) ||
        SetKey(clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment"))
        return SELFREG_E_CLASS;
    for (auto ext : kExts)
        if (SetKey(std::wstring(L"Software\\Classes\\") + ext + kThumbKey, nullptr, kClsidStr)) return SELFREG_E_CLASS;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    for (auto ext : kExts) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext + kThumbKey;
        wchar_t val[64] = {}; DWORD sz = sizeof(val);
        // só remove se o handler registrado for o nosso
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS &&
            _wcsicmp(val, kClsidStr) == 0)
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, key.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
