// DesignThumbs - miniaturas e painel de visualização (Alt+P) leves para o Explorer do Windows.
// Os formatos suportados e os decodificadores ficam em Formats.h (compartilhado com o DesignPeek).
// Registro por usuário (HKCU): instala sem administrador.
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
#include <new>
#include "Formats.h"
using std::min; using std::max;

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d2d1.lib")

// {9E85547E-13C6-4C5E-95CC-990C74D9BB3F}
static const CLSID CLSID_DesignThumbs = {0x9e85547e, 0x13c6, 0x4c5e, {0x95, 0xcc, 0x99, 0x0c, 0x74, 0xd9, 0xbb, 0x3f}};
static const wchar_t* kClsidStr = L"{9E85547E-13C6-4C5E-95CC-990C74D9BB3F}";
// {1188292A-70AB-412A-9C9D-E92625F45DCB}
static const CLSID CLSID_DesignPreview = {0x1188292a, 0x70ab, 0x412a, {0x9c, 0x9d, 0xe9, 0x26, 0x25, 0xf4, 0x5d, 0xcb}};
static const wchar_t* kPreviewClsidStr = L"{1188292A-70AB-412A-9C9D-E92625F45DCB}";

static HMODULE g_module;
static LONG g_refs;

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
        // A extensão ajuda a reconhecer RAW de câmera (TIFF por dentro)
        std::wstring ext;
        STATSTG st = {};
        if (SUCCEEDED(stream_->Stat(&st, STATFLAG_DEFAULT)) && st.pwcsName) {
            ext = PathFindExtensionW(st.pwcsName);
            CoTaskMemFree(st.pwcsName);
        }
        bool hasAlpha = false;
        HRESULT hr = fmt::Render(stream_, ext, cx, 0, phbmp, &hasAlpha);
        *alpha = hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        return hr;
    }
};

// ---------- COM: painel de visualização (Alt+P) ----------
// Roda dentro do prevhost.exe. Gera a imagem uma vez (composta sobre o fundo do painel)
// e só gera de novo se o painel crescer bastante.

static std::wstring StreamExt(IStream* s) {
    std::wstring ext;
    STATSTG st = {};
    if (SUCCEEDED(s->Stat(&st, STATFLAG_DEFAULT)) && st.pwcsName) {
        ext = PathFindExtensionW(st.pwcsName);
        CoTaskMemFree(st.pwcsName);
    }
    return ext;
}

class Preview : public IPreviewHandler, public IInitializeWithStream, public IObjectWithSite, public IOleWindow, public IPreviewHandlerVisuals {
    LONG ref_ = 1;
    IStream* stream_ = nullptr;
    IUnknown* site_ = nullptr;
    HWND parent_ = nullptr, wnd_ = nullptr;
    RECT rect_ = {};
    HBITMAP bmp_ = nullptr;   // opaco, já composto sobre bg_
    UINT bw_ = 0, bh_ = 0;
    COLORREF bg_ = RGB(255, 255, 255);
    std::wstring ext_;

    int page_ = 0, pages_ = 1, hot_ = 0;   // hot_: -1 seta esquerda, 1 seta direita
    RECT img_ = {};                        // onde a imagem foi desenhada (as setas ficam no canto dela)
    // Setas só em documentos de várias páginas: PDF e InDesign.
    bool Paged() const {
        return pages_ > 1 && (_wcsicmp(ext_.c_str(), L".pdf") == 0 || _wcsicmp(ext_.c_str(), L".indd") == 0 || _wcsicmp(ext_.c_str(), L".indt") == 0);
    }
    ID2D1Factory* d2d_ = nullptr;

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Preview*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: self->Paint(h); return 0;
        case WM_MOUSEMOVE: {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            int hot = self->ArrowAt(pt);
            if (hot != self->hot_) { self->hot_ = hot; InvalidateRect(h, nullptr, FALSE); }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE: if (self->hot_) { self->hot_ = 0; InvalidateRect(h, nullptr, FALSE); } return 0;
        case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, self->hot_ ? IDC_HAND : IDC_ARROW)); return TRUE;
        case WM_LBUTTONUP: {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            if (int d = self->ArrowAt(pt)) self->GoPage(self->page_ + d);
            return 0;
        }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Setas de página: dois círculos discretos no canto inferior direito da imagem.
    D2D1_ELLIPSE Arrow(int dir) {
        float r = 12, m = 8;
        float cy = img_.bottom - m - r, cxRight = img_.right - m - r;
        return D2D1::Ellipse(D2D1::Point2F(dir > 0 ? cxRight : cxRight - 2 * r - 6, cy), r, r);
    }
    int ArrowAt(POINT pt) {
        if (!Paged()) return 0;
        for (int d : {-1, 1}) {
            D2D1_ELLIPSE e = Arrow(d);
            float dx = pt.x - e.point.x, dy = pt.y - e.point.y;
            if (dx * dx + dy * dy <= (e.radiusX + 4) * (e.radiusX + 4) && ((d < 0 && page_ > 0) || (d > 0 && page_ < pages_ - 1))) return d;
        }
        return 0;
    }
    void GoPage(int p) {
        p = max(0, min(pages_ - 1, p));
        if (p == page_) return;
        page_ = p;
        if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
        Render();
        InvalidateRect(wnd_, nullptr, FALSE);
    }

    void Render() {
        UINT want = (UINT)max(256L, max(rect_.right - rect_.left, rect_.bottom - rect_.top));
        if (bmp_ && max(bw_, bh_) * 5 >= want * 4) return; // ainda nítido o bastante
        HBITMAP b = nullptr; bool alpha = false;
        int pages = 1;
        if (!stream_ || FAILED(fmt::Render(stream_, ext_, want, page_, &b, &alpha, &pages, true))) return;
        pages_ = pages;
        DIBSECTION ds;
        GetObject(b, sizeof(ds), &ds);
        BYTE* p = (BYTE*)ds.dsBm.bmBits;
        BYTE bgc[3] = {GetBValue(bg_), GetGValue(bg_), GetRValue(bg_)};
        for (size_t i = 0, n = (size_t)ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight); i < n; ++i, p += 4) // pré-multiplicado sobre o fundo
            for (int c = 0; c < 3; ++c) p[c] = (BYTE)(p[c] + bgc[c] * (255 - p[3]) / 255);
        if (bmp_) DeleteObject(bmp_);
        bmp_ = b; bw_ = ds.dsBm.bmWidth; bh_ = std::abs(ds.dsBm.bmHeight);
    }

    // Setas cinza translúcidas desenhadas com Direct2D (antisserrilhado), sobre a imagem.
    void PaintPager(HDC dc, const RECT& rc) {
        if (!Paged()) return;
        if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return;
        ID2D1DCRenderTarget* rt = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(d2d_->CreateDCRenderTarget(&p, &rt))) return;
        rt->BindDC(dc, &rc);
        rt->BeginDraw();
        ID2D1SolidColorBrush *fill = nullptr, *white = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0x6E6E73, 0.55f), &fill);
        rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 1), &white);
        ID2D1StrokeStyle* round = nullptr;
        d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
        for (int d : {-1, 1}) {
            bool enabled = d < 0 ? page_ > 0 : page_ < pages_ - 1;
            D2D1_ELLIPSE e = Arrow(d);
            fill->SetOpacity(!enabled ? 0.3f : hot_ == d ? 0.9f : 0.6f);
            rt->FillEllipse(e, fill);
            float cx = e.point.x + d * 1.0f, cy = e.point.y, k = 4.2f; // chevron < ou >
            white->SetOpacity(enabled ? 1.0f : 0.55f);
            rt->DrawLine(D2D1::Point2F(cx - d * k * 0.55f, cy - k), D2D1::Point2F(cx + d * k * 0.55f, cy), white, 1.8f, round);
            rt->DrawLine(D2D1::Point2F(cx + d * k * 0.55f, cy), D2D1::Point2F(cx - d * k * 0.55f, cy + k), white, 1.8f, round);
        }
        rt->EndDraw();
        if (round) round->Release();
        fill->Release(); white->Release();
        rt->Release();
    }

    void Paint(HWND h) {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc); // sem piscar
        HBITMAP buf = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ oldBuf = SelectObject(dc, buf);
        HBRUSH br = CreateSolidBrush(bg_);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        if (bmp_) {
            int pad = 8, aw = rc.right - 2 * pad, ah = rc.bottom - 2 * pad;
            double k = min(min((double)aw / bw_, (double)ah / bh_), 1.0);
            int w = max(1, (int)(bw_ * k)), hh = max(1, (int)(bh_ * k));
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, bmp_);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            img_ = {(rc.right - w) / 2, (rc.bottom - hh) / 2, (rc.right - w) / 2 + w, (rc.bottom - hh) / 2 + hh};
            StretchBlt(dc, img_.left, img_.top, w, hh, mem, 0, 0, bw_, bh_, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        PaintPager(dc, rc);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBuf);
        DeleteObject(buf);
        DeleteDC(dc);
        EndPaint(h, &ps);
    }

public:
    Preview() { InterlockedIncrement(&g_refs); }
    ~Preview() { Unload(); if (site_) site_->Release(); if (d2d_) d2d_->Release(); InterlockedDecrement(&g_refs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(Preview, IPreviewHandler), QITABENT(Preview, IInitializeWithStream), QITABENT(Preview, IObjectWithSite),
                                    QITABENT(Preview, IOleWindow), QITABENT(Preview, IPreviewHandlerVisuals), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* s, DWORD) {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = s; s->AddRef();
        ext_ = StreamExt(s);
        return S_OK;
    }
    // IPreviewHandler
    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) {
        parent_ = hwnd; rect_ = *prc;
        if (wnd_) { SetParent(wnd_, parent_); SetRect(prc); }
        return S_OK;
    }
    IFACEMETHODIMP SetRect(const RECT* prc) {
        rect_ = *prc;
        if (wnd_) {
            Render();
            SetWindowPos(wnd_, nullptr, rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(wnd_, nullptr, FALSE);
        }
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() {
        if (wnd_ || !parent_) return E_FAIL;
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_module;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DesignThumbsPreview";
        RegisterClassW(&wc);
        Render();
        wnd_ = CreateWindowExW(0, wc.lpszClassName, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, rect_.left, rect_.top,
                               rect_.right - rect_.left, rect_.bottom - rect_.top, parent_, nullptr, g_module, nullptr);
        if (!wnd_) return HRESULT_FROM_WIN32(GetLastError());
        SetWindowLongPtrW(wnd_, GWLP_USERDATA, (LONG_PTR)this);
        return bmp_ ? S_OK : E_FAIL;
    }
    IFACEMETHODIMP Unload() {
        if (wnd_) { DestroyWindow(wnd_); wnd_ = nullptr; }
        if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
        if (stream_) { stream_->Release(); stream_ = nullptr; }
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() { return S_FALSE; }
    IFACEMETHODIMP QueryFocus(HWND* phwnd) { *phwnd = GetFocus(); return S_OK; }
    IFACEMETHODIMP TranslateAccelerator(MSG*) { return S_FALSE; }
    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* site) { if (site_) site_->Release(); site_ = site; if (site_) site_->AddRef(); return S_OK; }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) { *ppv = nullptr; return site_ ? site_->QueryInterface(riid, ppv) : E_FAIL; }
    // IOleWindow
    IFACEMETHODIMP GetWindow(HWND* phwnd) { *phwnd = wnd_; return wnd_ ? S_OK : E_FAIL; }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) { return E_NOTIMPL; }
    // IPreviewHandlerVisuals: fundo segue o tema do Explorer
    IFACEMETHODIMP SetBackgroundColor(COLORREF c) { bg_ = c; if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; } if (wnd_) { Render(); InvalidateRect(wnd_, nullptr, FALSE); } return S_OK; }
    IFACEMETHODIMP SetFont(const LOGFONTW*) { return S_OK; }
    IFACEMETHODIMP SetTextColor(COLORREF) { return S_OK; }
};

template <class T> class Factory : public IClassFactory {
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
        T* p = new (std::nothrow) T();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) { lock ? InterlockedIncrement(&g_refs) : InterlockedDecrement(&g_refs); return S_OK; }
};
static Factory<Provider> g_thumbFactory;
static Factory<Preview> g_previewFactory;

// ---------- exports ----------

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_module = m; DisableThreadLibraryCalls(m); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid == CLSID_DesignThumbs) return g_thumbFactory.QueryInterface(riid, ppv);
    if (clsid == CLSID_DesignPreview) return g_previewFactory.QueryInterface(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

static LSTATUS SetKey(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (s == ERROR_SUCCESS) {
        s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    return s;
}

static const wchar_t* kThumbKey = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t* kPreviewKey = L"\\ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
static const wchar_t* kPreviewHandlers = L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
static const wchar_t* kPrevhostAppId = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}"; // prevhost.exe 64 bits

static bool RegisterClsid(const wchar_t* clsidStr, const wchar_t* name, const wchar_t* path) {
    std::wstring clsid = std::wstring(L"Software\\Classes\\CLSID\\") + clsidStr;
    return !SetKey(clsid, nullptr, name) && !SetKey(clsid + L"\\InprocServer32", nullptr, path) &&
           !SetKey(clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!RegisterClsid(kClsidStr, L"DesignThumbs Thumbnail Provider", path) ||
        !RegisterClsid(kPreviewClsidStr, L"DesignThumbs Preview Handler", path))
        return SELFREG_E_CLASS;
    std::wstring prev = std::wstring(L"Software\\Classes\\CLSID\\") + kPreviewClsidStr;
    SetKey(prev, L"AppID", kPrevhostAppId);
    SetKey(prev, L"DisplayName", L"DesignThumbs");
    SetKey(kPreviewHandlers, kPreviewClsidStr, L"DesignThumbs Preview Handler");
    for (auto ext : fmt::kExtensions) {
        if (SetKey(std::wstring(L"Software\\Classes\\") + ext + kThumbKey, nullptr, kClsidStr) ||
            SetKey(std::wstring(L"Software\\Classes\\") + ext + kPreviewKey, nullptr, kPreviewClsidStr))
            return SELFREG_E_CLASS;
    }
    for (auto ext : fmt::kPreviewOnly) // PDF: só o painel (com setas de página)
        SetKey(std::wstring(L"Software\\Classes\\") + ext + kPreviewKey, nullptr, kPreviewClsidStr);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

// só remove se o handler registrado for o nosso
static void DeleteIfOurs(const std::wstring& key, const wchar_t* clsidStr) {
    wchar_t val[64] = {}; DWORD sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS && _wcsicmp(val, clsidStr) == 0)
        RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kPreviewClsidStr).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kPreviewHandlers, kPreviewClsidStr);
    for (auto ext : fmt::kExtensions) {
        DeleteIfOurs(std::wstring(L"Software\\Classes\\") + ext + kThumbKey, kClsidStr);
        DeleteIfOurs(std::wstring(L"Software\\Classes\\") + ext + kPreviewKey, kPreviewClsidStr);
    }
    for (auto ext : fmt::kPreviewOnly) DeleteIfOurs(std::wstring(L"Software\\Classes\\") + ext + kPreviewKey, kPreviewClsidStr);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
