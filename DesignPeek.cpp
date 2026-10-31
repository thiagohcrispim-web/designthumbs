// DesignPeek - "Quick Look" para o Explorer: selecione um arquivo e aperte Espaço.
//   Espaço/Esc: fecha         Setas/clique em outro arquivo: troca o preview
//   Roda do mouse/+/-: zoom   Arrastar: mover        Z: 100%   0: ajustar   Duplo clique: alterna
//   PgUp/PgDn: páginas        Enter: abre no programa padrão  Ctrl+C: copia a imagem
// Sem hook de teclado: o Espaço só vira atalho (RegisterHotKey) enquanto o foco está
// na lista de arquivos, detectado por eventos de foco de acessibilidade (WinEvents).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000003 // Windows 10 1703 (SVG no Direct2D)
#endif
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <dwmapi.h>
#include "Formats.h" // antes do dwrite.h (ordem dos headers do MinGW)
#include <dwrite.h>
#include <set>

using std::min; using std::max;

static HWND g_wnd;
static HWINEVENTHOOK g_focusHook, g_fgHook;
static bool g_open, g_spaceKey, g_openKeys;
static HWND g_defView;           // lista de arquivos de onde veio o Espaço

// arquivo atual
static std::wstring g_path, g_name, g_ext, g_flash;
static fmt::Info g_info;
static UINT64 g_fileSize;
static int g_page, g_pages = 1;

// imagem atual (DIB 32bpp pré-multiplicado) e sua cópia na GPU
static HBITMAP g_dib;
static UINT g_bw, g_bh;
static bool g_alpha, g_full;

// visualização: escala em pixels de tela por pixel "nativo" (g_nw x g_nh)
static float g_nw = 1, g_nh = 1, g_scale = 1, g_ox, g_oy, g_ui = 1;
static bool g_fitMode = true, g_nativeKnown, g_dragging;
static POINT g_dragFrom;

// Direct2D / DirectWrite
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1Bitmap* g_gpuBmp;
static ID2D1BitmapBrush* g_checker;
static IDWriteTextFormat *g_titleFmt, *g_infoFmt;
static float g_fmtScale;

enum { kHotSpace = 1, kHotEsc, kHotEnter, kHotCopy, kHotZ, kHot0, kHotNum0, kHotPlus, kHotMinus, kHotAdd, kHotSub, kHotPgUp, kHotPgDn };
static const struct { int id; UINT mod, vk; } kOpenKeys[] = {
    {kHotEsc, 0, VK_ESCAPE}, {kHotEnter, 0, VK_RETURN}, {kHotCopy, MOD_CONTROL, 'C'}, {kHotZ, 0, 'Z'},
    {kHot0, 0, '0'}, {kHotNum0, 0, VK_NUMPAD0}, {kHotPlus, 0, VK_OEM_PLUS}, {kHotMinus, 0, VK_OEM_MINUS},
    {kHotAdd, 0, VK_ADD}, {kHotSub, 0, VK_SUBTRACT}, {kHotPgUp, 0, VK_PRIOR}, {kHotPgDn, 0, VK_NEXT},
};
enum { kRefreshTimer = 1, kFlashTimer };
static const float kCaption = 40, kInfo = 52, kPad = 12;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::wstring ClassOf(HWND h) {
    wchar_t c[64] = {};
    GetClassNameW(h, c, 64);
    return c;
}

// ---------- Explorer: foco e seleção ----------

// O foco está na lista de arquivos de uma janela do Explorer (ou da área de trabalho)?
static HWND FocusedFileList() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    GUITHREADINFO gti = {sizeof(gti)};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti)) return nullptr;
    if (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE) || gti.hwndCaret) return nullptr;
    HWND f = gti.hwndFocus;
    if (!f || ClassOf(f) != L"DirectUIHWND") return nullptr;
    HWND parent = GetParent(f);
    return parent && ClassOf(parent) == L"SHELLDLL_DefView" ? parent : nullptr;
}

static IShellItem* ItemFromBrowser(IDispatch* disp, HWND defView) {
    IShellItem* item = nullptr;
    IServiceProvider* sp = nullptr; IShellBrowser* sb = nullptr; IShellView* sv = nullptr; IFolderView2* fv = nullptr;
    if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) &&
        SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
        SUCCEEDED(sb->QueryActiveShellView(&sv))) {
        HWND h = nullptr;
        sv->GetWindow(&h);
        int idx = -1;
        if (h == defView && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) &&
            SUCCEEDED(fv->GetSelectedItem(-1, &idx)) && idx >= 0)
            fv->GetItem(idx, IID_PPV_ARGS(&item));
    }
    SafeRelease(fv); SafeRelease(sv); SafeRelease(sb); SafeRelease(sp);
    return item;
}

// Item selecionado na lista `defView` (janelas/abas do Explorer e área de trabalho).
static IShellItem* SelectedItem(HWND defView) {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) return nullptr;
    IShellItem* item = nullptr;
    long count = 0;
    sw->get_Count(&count);
    for (long i = 0; i < count && !item; ++i) {
        VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = i;
        IDispatch* d = nullptr;
        if (SUCCEEDED(sw->Item(v, &d)) && d) { item = ItemFromBrowser(d, defView); d->Release(); }
    }
    if (!item) {
        VARIANT empty; VariantInit(&empty);
        long hwnd; IDispatch* d = nullptr;
        if (sw->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &d) == S_OK && d) {
            item = ItemFromBrowser(d, defView); d->Release();
        }
    }
    sw->Release();
    return item;
}

// ---------- obtenção da imagem ----------

// Alguns bitmaps 32bpp vêm com alpha zerado; outros com alpha não pré-multiplicado.
static bool PrepareAlpha(HBITMAP bmp) {
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
static HBITMAP ToDib(HBITMAP src) {
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
static HBITMAP PdfHandlerImage(UINT cx) {
    static const wchar_t* kThumbShellEx = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
    wchar_t clsidStr[64] = {}, prog[256] = {};
    DWORD sz = sizeof(clsidStr);
    std::wstring keys[3] = {std::wstring(L".pdf") + kThumbShellEx, L"", std::wstring(L"SystemFileAssociations\\.pdf") + kThumbShellEx};
    DWORD psz = sizeof(prog);
    if (RegGetValueW(HKEY_CLASSES_ROOT, L".pdf", nullptr, RRF_RT_REG_SZ, nullptr, prog, &psz) == ERROR_SUCCESS)
        keys[1] = std::wstring(prog) + kThumbShellEx;
    bool found = false;
    for (auto& k : keys) {
        sz = sizeof(clsidStr);
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
        SUCCEEDED(SHCreateStreamOnFileEx(g_path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)))
        init = SUCCEEDED(is->Initialize(stream, STGM_READ));
    if (!init && SUCCEEDED(tp->QueryInterface(IID_PPV_ARGS(&ii)))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            init = SUCCEEDED(ii->Initialize(item, STGM_READ));
            item->Release();
        }
    }
    if (!init && SUCCEEDED(tp->QueryInterface(IID_PPV_ARGS(&ifl)))) init = SUCCEEDED(ifl->Initialize(g_path.c_str(), STGM_READ));
    HBITMAP bmp = nullptr;
    WTS_ALPHATYPE at;
    if (init && FAILED(tp->GetThumbnail(cx, &bmp, &at))) bmp = nullptr;
    SafeRelease(stream); SafeRelease(is); SafeRelease(ii); SafeRelease(ifl); tp->Release();
    return bmp;
}

// Gera a imagem do arquivo atual com lado máximo cx (formatos próprios, PDF do .ai ou o Windows).
static bool LoadImage(UINT cx) {
    HBITMAP bmp = nullptr;
    bool alpha = false;
    if (_wcsicmp(g_ext.c_str(), L".ai") == 0 && g_pages <= 1 && (bmp = PdfHandlerImage(cx)) != nullptr) {
        bmp = ToDib(bmp);
        alpha = PrepareAlpha(bmp);
    }
    if (!bmp && fmt::Handles(g_ext)) {
        IStream* s = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(g_path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) {
            int pages = 1;
            if (FAILED(fmt::Render(s, g_ext, cx, g_page, &bmp, &alpha, &pages))) bmp = nullptr;
            g_pages = pages;
            s->Release();
        }
    }
    if (!bmp) {
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            if (FAILED(f->GetImage({(LONG)cx, (LONG)cx}, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bmp)) &&
                FAILED(f->GetImage({256, 256}, SIIGBF_ICONONLY, &bmp))) // sem preview: mostra o ícone
                bmp = nullptr;
            f->Release();
        }
        if (bmp) { bmp = ToDib(bmp); alpha = PrepareAlpha(bmp); }
    }
    if (!bmp) return false;
    if (g_dib) DeleteObject(g_dib);
    g_dib = bmp;
    g_alpha = alpha;
    BITMAP bm; GetObject(bmp, sizeof(bm), &bm);
    g_bw = bm.bmWidth; g_bh = std::abs(bm.bmHeight);
    SafeRelease(g_gpuBmp);
    return true;
}

// Info para arquivos que o Windows decodifica (JPG, PNG, TIFF, WebP, HEIC...).
static void ShellInfo() {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(g_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr))) {
        fr->GetSize(&g_info.w, &g_info.h);
        double dx = 0, dy = 0;
        if (SUCCEEDED(fr->GetResolution(&dx, &dy)) && dx > 1 && dx != 96) g_info.dpi = dx;
        WICPixelFormatGUID pf;
        IWICComponentInfo* ci = nullptr; IWICPixelFormatInfo2* pi = nullptr;
        if (SUCCEEDED(fr->GetPixelFormat(&pf)) && SUCCEEDED(f->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci->QueryInterface(IID_PPV_ARGS(&pi)))) {
            UINT bpp = 0, ch = 0;
            pi->GetBitsPerPixel(&bpp); pi->GetChannelCount(&ch);
            if (ch) g_info.bits = bpp / ch;
            WICPixelFormatNumericRepresentation nr;
            if (ch >= 4 && (IsEqualGUID(pf, GUID_WICPixelFormat32bppCMYK) || IsEqualGUID(pf, GUID_WICPixelFormat64bppCMYK))) g_info.color = L"CMYK";
            else if (ch == 1 && SUCCEEDED(pi->GetNumericRepresentation(&nr)) && nr != WICPixelFormatNumericRepresentationIndexed) g_info.color = L"Escala de cinza";
            else if (ch >= 3) g_info.color = ch == 4 ? L"RGBA" : L"RGB";
        }
        SafeRelease(pi); SafeRelease(ci);
    }
    SafeRelease(fr); SafeRelease(dec); f->Release();
}

// ---------- fontes instaladas ----------

static std::wstring Lower(std::wstring s) { CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }

static int CALLBACK FontEnum(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM p) {
    auto* set = (std::set<std::wstring>*)p;
    const wchar_t* face = lf->lfFaceName[0] == L'@' ? lf->lfFaceName + 1 : lf->lfFaceName;
    set->insert(Lower(face));
    set->insert(Lower(((const ENUMLOGFONTEXW*)lf)->elfFullName));
    return 1;
}

static bool FontInstalled(const std::wstring& family) {
    static std::set<std::wstring> fonts;
    static DWORD builtAt;
    if (fonts.empty() || GetTickCount() - builtAt > 60000) { // fontes ativadas há pouco (Adobe Fonts) também contam
        fonts.clear();
        HDC dc = GetDC(nullptr);
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, FontEnum, (LPARAM)&fonts, 0);
        ReleaseDC(nullptr, dc);
        builtAt = GetTickCount();
    }
    return fonts.count(Lower(family)) > 0;
}

// ---------- layout ----------

static D2D1_RECT_F ImageArea() {
    RECT rc; GetClientRect(g_wnd, &rc);
    return D2D1::RectF(kPad * g_ui, kCaption * g_ui, rc.right - kPad * g_ui, rc.bottom - kInfo * g_ui);
}

static float FitScale() {
    D2D1_RECT_F a = ImageArea();
    float s = min((a.right - a.left) / g_nw, (a.bottom - a.top) / g_nh);
    return min(s, g_nativeKnown ? 1.0f : 2.0f); // não amplia além do tamanho real
}

// Carrega uma versão maior quando o zoom passa da resolução atual.
static void EnsureResolution() {
    if (g_full || g_nw * g_scale <= g_bw * 1.1f) return;
    UINT maxTex = g_rt ? min(8192u, g_rt->GetMaximumBitmapSize()) : 8192u;
    UINT want = g_nativeKnown ? (UINT)max(g_info.w, g_info.h) : 4096u;
    g_full = true;
    LoadImage(min(want, maxTex));
}

static void SetZoom(float scale, float cx, float cy) {
    D2D1_RECT_F a = ImageArea();
    float fit = FitScale();
    scale = max(min(scale, 64.0f), fit * 0.25f);
    float acx = (a.left + a.right) / 2, acy = (a.top + a.bottom) / 2;
    // mantém fixo o ponto sob o cursor
    g_ox = cx - acx - (cx - acx - g_ox) * scale / g_scale;
    g_oy = cy - acy - (cy - acy - g_oy) * scale / g_scale;
    g_scale = scale;
    g_fitMode = std::fabs(scale - fit) < 1e-4f;
    if (g_fitMode) g_ox = g_oy = 0;
    EnsureResolution();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void ZoomAtCenter(float factor) {
    D2D1_RECT_F a = ImageArea();
    SetZoom(g_scale * factor, (a.left + a.right) / 2, (a.top + a.bottom) / 2);
}

static void Fit() { g_scale = FitScale(); g_ox = g_oy = 0; g_fitMode = true; InvalidateRect(g_wnd, nullptr, FALSE); }

static void ActualSize() {
    D2D1_RECT_F a = ImageArea();
    float one = g_nativeKnown ? 1.0f : g_bw / g_nw; // sem tamanho nativo: 1 pixel do preview
    g_scale = FitScale();
    SetZoom(one, (a.left + a.right) / 2, (a.top + a.bottom) / 2);
}

// ---------- abrir / fechar ----------

static void UpdateHotkeys();

static void ClosePreview() {
    g_open = false;
    g_dragging = false;
    ReleaseCapture();
    KillTimer(g_wnd, kRefreshTimer);
    ShowWindow(g_wnd, SW_HIDE);
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    SafeRelease(g_gpuBmp);
    g_path.clear();
    UpdateHotkeys();
}

static void ResizeToContent() {
    HMONITOR mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    float maxW = (wa.right - wa.left) * 0.85f - 2 * kPad * g_ui, maxH = (wa.bottom - wa.top) * 0.85f - (kCaption + kInfo) * g_ui;
    float s = min(maxW / g_nw, maxH / g_nh);
    s = min(s, g_nativeKnown ? 1.0f : 2.0f);
    int w = max((int)(g_nw * s + 2 * kPad * g_ui), (int)(620 * g_ui));
    int h = max((int)(g_nh * s + (kCaption + kInfo) * g_ui), (int)(320 * g_ui));
    SetWindowPos(g_wnd, HWND_TOPMOST, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                 w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_rt) g_rt->Resize(D2D1::SizeU(w, h));
}

static bool OpenSelection() {
    IShellItem* item = SelectedItem(g_defView);
    if (!item) return false;
    PWSTR path = nullptr, name = nullptr;
    item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
    item->Release();
    std::wstring p = path ? path : L"";
    std::wstring n = name ? name : L"";
    CoTaskMemFree(path); CoTaskMemFree(name);
    if (p.empty() || (g_open && p == g_path)) return g_open;
    if (GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY) return g_open;

    g_path = p; g_name = n; g_ext = Lower(PathFindExtensionW(p.c_str()));
    g_page = 0; g_pages = 1; g_full = false;
    g_info = fmt::Info();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    g_fileSize = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) ? ((UINT64)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) : 0;
    if (fmt::Handles(g_ext)) {
        IStream* s = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(p.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) {
            fmt::GetInfo(s, g_ext, &g_info);
            s->Release();
        }
    } else ShellInfo();

    g_ui = GetDpiForWindow(g_defView) / 96.0f;
    HMONITOR mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT screen = (UINT)max(mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top);

    g_nativeKnown = g_info.w > 0 && g_info.h > 0 && g_info.note.empty();
    UINT cx = screen;
    if (g_nativeKnown) cx = min(cx, max(g_info.w, g_info.h));
    if (!LoadImage(cx)) { if (g_open) ClosePreview(); return false; }
    if (g_info.w > 0 && g_info.h > 0) { g_nw = (float)g_info.w; g_nh = (float)g_info.h; }
    else { g_nw = (float)g_bw; g_nh = (float)g_bh; }
    g_full = g_nativeKnown && cx >= max(g_info.w, g_info.h);

    g_open = true;
    ResizeToContent();
    Fit();
    UpdateHotkeys();
    return true;
}

static void ChangePage(int delta) {
    if (g_pages <= 1) return;
    int p = max(0, min(g_pages - 1, g_page + delta));
    if (p == g_page) return;
    g_page = p;
    LoadImage(max(g_bw, g_bh));
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void Flash(const wchar_t* text) {
    g_flash = text;
    SetTimer(g_wnd, kFlashTimer, 1500, nullptr);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// Ctrl+C: copia como PNG (mantém transparência) e como bitmap (sobre branco).
static void CopyImage() {
    if (!g_full) { g_scale = max(g_scale, g_nw > 0 ? g_bw * 2.0f / g_nw : 1.0f); EnsureResolution(); Fit(); }
    if (!g_dib) return;
    DIBSECTION ds;
    if (GetObject(g_dib, sizeof(ds), &ds) != sizeof(ds)) return;
    const BYTE* px = (const BYTE*)ds.dsBm.bmBits;
    UINT w = g_bw, h = g_bh;

    size_t rowBytes = ((size_t)w * 3 + 3) & ~(size_t)3;
    HGLOBAL dibMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + rowBytes * h);
    if (dibMem) {
        auto* bih = (BITMAPINFOHEADER*)GlobalLock(dibMem);
        *bih = {};
        bih->biSize = sizeof(*bih); bih->biWidth = w; bih->biHeight = h; bih->biPlanes = 1; bih->biBitCount = 24;
        BYTE* dst = (BYTE*)(bih + 1);
        for (UINT y = 0; y < h; ++y) {
            const BYTE* s = px + (size_t)(h - 1 - y) * w * 4;
            BYTE* d = dst + y * rowBytes;
            for (UINT x = 0; x < w; ++x, s += 4, d += 3) // pré-multiplicado sobre branco
                for (int c = 0; c < 3; ++c) d[c] = (BYTE)(s[c] + (255 - s[3]));
        }
        GlobalUnlock(dibMem);
    }

    HGLOBAL pngMem = nullptr;
    IWICImagingFactory* f = nullptr; IWICBitmap* wb = nullptr; IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr; IStream* st = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4, (BYTE*)px, &wb)) &&
        SUCCEEDED(CreateStreamOnHGlobal(nullptr, FALSE, &st)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
        SUCCEEDED(fr->WriteSource(wb, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
        GetHGlobalFromStream(st, &pngMem);
    SafeRelease(fr); SafeRelease(enc); SafeRelease(st); SafeRelease(wb); SafeRelease(f);

    if (OpenClipboard(g_wnd)) {
        EmptyClipboard();
        if (pngMem && SetClipboardData(RegisterClipboardFormatW(L"PNG"), pngMem)) pngMem = nullptr;
        if (dibMem && SetClipboardData(CF_DIB, dibMem)) dibMem = nullptr;
        CloseClipboard();
        Flash(L"Imagem copiada");
    }
    if (pngMem) GlobalFree(pngMem);
    if (dibMem) GlobalFree(dibMem);
}

// ---------- atalhos ----------

// Registra o Espaço como atalho só quando faz sentido, para não atrapalhar a digitação.
static void UpdateHotkeys() {
    bool space = g_open || FocusedFileList();
    if (space != g_spaceKey) {
        if (space) g_spaceKey = RegisterHotKey(g_wnd, kHotSpace, MOD_NOREPEAT, VK_SPACE);
        else { UnregisterHotKey(g_wnd, kHotSpace); g_spaceKey = false; }
    }
    if (g_open != g_openKeys) {
        for (auto& k : kOpenKeys) {
            if (g_open) RegisterHotKey(g_wnd, k.id, k.mod, k.vk);
            else UnregisterHotKey(g_wnd, k.id);
        }
        g_openKeys = g_open;
    }
}

static void CALLBACK FocusEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (g_open && event == EVENT_OBJECT_FOCUS && hwnd && GetParent(hwnd) == g_defView)
        SetTimer(g_wnd, kRefreshTimer, 120, nullptr); // seleção mudou (setas/clique): atualiza o preview
    if (g_open && event == EVENT_SYSTEM_FOREGROUND && hwnd != g_wnd &&
        GetAncestor(hwnd, GA_ROOT) != GetAncestor(g_defView, GA_ROOT))
        ClosePreview(); // usuário foi para outro programa
    UpdateHotkeys();
}

static void OnHotkey(int id) {
    switch (id) {
    case kHotSpace:
        if (g_open) ClosePreview();
        else if (HWND dv = FocusedFileList()) { g_defView = dv; OpenSelection(); }
        break;
    case kHotEsc: ClosePreview(); break;
    case kHotEnter: {
        std::wstring p = g_path;
        ClosePreview();
        ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kHotCopy: CopyImage(); break;
    case kHotZ: ActualSize(); break;
    case kHot0: case kHotNum0: Fit(); break;
    case kHotPlus: case kHotAdd: ZoomAtCenter(1.25f); break;
    case kHotMinus: case kHotSub: ZoomAtCenter(0.8f); break;
    case kHotPgUp: ChangePage(-1); break;
    case kHotPgDn: ChangePage(1); break;
    }
    UpdateHotkeys();
}

// ---------- desenho ----------

static std::wstring FormatSize(UINT64 b) {
    wchar_t t[32];
    if (b >= (1ull << 30)) swprintf(t, 32, L"%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) swprintf(t, 32, L"%.1f MB", b / 1048576.0);
    else swprintf(t, 32, L"%.0f KB", max(1.0, b / 1024.0));
    for (wchar_t* c = t; *c; ++c) if (*c == L'.') *c = L',';
    return t;
}

static void CreateDeviceResources() {
    if (g_rt) return;
    RECT rc; GetClientRect(g_wnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
    rp.dpiX = rp.dpiY = 96; // trabalha em pixels
    if (FAILED(g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt))) return;
    // xadrez para transparência
    UINT32 cells[16 * 16];
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) cells[y * 16 + x] = ((x < 8) ^ (y < 8)) ? 0xFF3A3A3A : 0xFF2C2C2C;
    ID2D1Bitmap* cb = nullptr;
    if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(16, 16), cells, 64, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &cb))) {
        g_rt->CreateBitmapBrush(cb, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), &g_checker);
        cb->Release();
    }
}

static void DiscardDeviceResources() { SafeRelease(g_gpuBmp); SafeRelease(g_checker); SafeRelease(g_rt); }

static void EnsureTextFormats() {
    if (g_titleFmt && g_fmtScale == g_ui) return;
    SafeRelease(g_titleFmt); SafeRelease(g_infoFmt);
    g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14 * g_ui, L"pt-br", &g_titleFmt);
    g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12 * g_ui, L"pt-br", &g_infoFmt);
    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    for (IDWriteTextFormat* f : {g_titleFmt, g_infoFmt}) {
        if (!f) continue;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(f, &ellipsis))) { f->SetTrimming(&trim, ellipsis); ellipsis->Release(); }
    }
    g_fmtScale = g_ui;
}

static std::wstring InfoLine() {
    std::vector<std::wstring> parts;
    wchar_t t[128];
    const fmt::Info& in = g_info;
    if (in.w && in.h) {
        swprintf(t, 128, L"%u × %u px", in.w, in.h);
        parts.push_back(t);
        if (in.dpi >= 1 && in.note.empty()) {
            swprintf(t, 128, L"%.0f dpi (%.1f × %.1f cm)", in.dpi, in.w / in.dpi * 2.54, in.h / in.dpi * 2.54);
            for (wchar_t* c = t; *c; ++c) if (*c == L'.') *c = L',';
            parts.push_back(t);
        }
    }
    if (!in.pageSize.empty()) parts.push_back(in.pageSize);
    if (in.pages > 1) { swprintf(t, 128, L"%u páginas", in.pages); parts.push_back(t); }
    if (!in.color.empty()) {
        std::wstring c = in.color;
        if (in.bits) { swprintf(t, 128, L" %u bits", in.bits); c += t; }
        parts.push_back(c);
    }
    if (!in.camera.empty()) parts.push_back(in.camera);
    if (g_fileSize) parts.push_back(FormatSize(g_fileSize));
    if (!in.app.empty()) parts.push_back(in.app);
    if (!in.note.empty()) parts.push_back(in.note);
    std::wstring s;
    for (auto& p : parts) s += (s.empty() ? L"" : L"   ·   ") + p;
    return s;
}

static void Paint() {
    CreateDeviceResources();
    if (!g_rt) return;
    EnsureTextFormats();
    if (!g_gpuBmp && g_dib) {
        DIBSECTION ds;
        if (GetObject(g_dib, sizeof(ds), &ds) == sizeof(ds))
            g_rt->CreateBitmap(D2D1::SizeU(g_bw, g_bh), ds.dsBm.bmBits, g_bw * 4,
                               D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &g_gpuBmp);
    }
    D2D1_SIZE_F size = g_rt->GetSize();
    ID2D1SolidColorBrush *text = nullptr, *dim = nullptr, *red = nullptr, *accent = nullptr;
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0x1E1E1E));
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0xE6E6E6), &text);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x9A9A9A), &dim);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF6B6B), &red);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x4CC2FF), &accent);

    // imagem
    D2D1_RECT_F a = ImageArea();
    if (g_gpuBmp) {
        float w = g_nw * g_scale, h = g_nh * g_scale;
        float cx = (a.left + a.right) / 2 + g_ox, cy = (a.top + a.bottom) / 2 + g_oy;
        D2D1_RECT_F dst = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        g_rt->PushAxisAlignedClip(a, D2D1_ANTIALIAS_MODE_ALIASED);
        if (g_alpha && g_checker) g_rt->FillRectangle(dst, g_checker);
        ID2D1DeviceContext* dc = nullptr;
        bool pixelPeep = w / g_bw >= 3; // zoom alto: pixels nítidos
        if (SUCCEEDED(g_rt->QueryInterface(IID_PPV_ARGS(&dc)))) {
            dc->DrawBitmap(g_gpuBmp, &dst, 1.0f, pixelPeep ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            dc->Release();
        } else g_rt->DrawBitmap(g_gpuBmp, dst, 1.0f, pixelPeep ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g_rt->PopAxisAlignedClip();
    }

    // título, página e zoom
    if (g_titleFmt && g_infoFmt) {
        float side = 150 * g_ui;
        std::wstring right;
        wchar_t t[64];
        if (g_pages > 1) { swprintf(t, 64, L"Página %d de %d   ", g_page + 1, g_pages); right += t; }
        swprintf(t, 64, L"%.0f%%", g_scale * 100 * (g_nativeKnown ? 1.0f : g_nw / g_bw));
        right += t;
        g_titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_rt->DrawText(g_name.c_str(), (UINT32)g_name.size(), g_titleFmt, D2D1::RectF(side, 0, size.width - side, kCaption * g_ui), text);
        g_infoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_rt->DrawText(right.c_str(), (UINT32)right.size(), g_infoFmt, D2D1::RectF(size.width - side - kPad * g_ui, 0, size.width - kPad * g_ui, kCaption * g_ui), dim);
        if (!g_flash.empty()) {
            g_infoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_rt->DrawText(g_flash.c_str(), (UINT32)g_flash.size(), g_infoFmt, D2D1::RectF(kPad * g_ui, 0, side, kCaption * g_ui), accent);
        }

        // barra de informações
        g_infoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        float top = size.height - kInfo * g_ui, lineH = 20 * g_ui;
        std::wstring l1 = InfoLine();
        g_rt->DrawText(l1.c_str(), (UINT32)l1.size(), g_infoFmt, D2D1::RectF(kPad * g_ui, top + 4 * g_ui, size.width - kPad * g_ui, top + 4 * g_ui + lineH), text);

        std::wstring l2;
        std::vector<DWRITE_TEXT_RANGE> missing;
        if (!g_info.fonts.empty()) {
            l2 = L"Fontes: ";
            for (size_t i = 0; i < g_info.fonts.size(); ++i) {
                if (i) l2 += L", ";
                bool ok = FontInstalled(g_info.fontFamilies[i]);
                if (!ok) missing.push_back({(UINT32)l2.size(), (UINT32)g_info.fonts[i].size() + 2});
                l2 += g_info.fonts[i];
                if (!ok) l2 += L" ⚠";
            }
        } else l2 = L"Roda: zoom  ·  Arrastar: mover  ·  Z: 100%  ·  0: ajustar  ·  PgUp/PgDn: páginas  ·  Enter: abrir  ·  Ctrl+C: copiar";
        IDWriteTextLayout* lay = nullptr;
        float lw = size.width - 2 * kPad * g_ui;
        if (SUCCEEDED(g_dw->CreateTextLayout(l2.c_str(), (UINT32)l2.size(), g_infoFmt, lw, lineH, &lay))) {
            for (auto& r : missing) lay->SetDrawingEffect(red, r);
            g_rt->DrawTextLayout(D2D1::Point2F(kPad * g_ui, top + 4 * g_ui + lineH), lay, g_info.fonts.empty() ? dim : text);
            lay->Release();
        }
    }
    SafeRelease(text); SafeRelease(dim); SafeRelease(red); SafeRelease(accent);
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

// ---------- janela ----------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY: OnHotkey((int)wp); return 0;
    case WM_TIMER:
        KillTimer(h, wp);
        if (wp == kRefreshTimer && g_open) OpenSelection();
        if (wp == kFlashTimer) { g_flash.clear(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // o foco continua no Explorer (setas funcionam)
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        SetZoom(g_scale * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.2f : 1 / 1.2f), (float)pt.x, (float)pt.y);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_dragFrom = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            g_ox += p.x - g_dragFrom.x; g_oy += p.y - g_dragFrom.y;
            g_dragFrom = p;
            g_fitMode = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: g_dragging = false; ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK: {
        if (g_fitMode) {
            float one = g_nativeKnown ? 1.0f : g_bw / g_nw;
            SetZoom(max(one, FitScale() * 2), (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        } else Fit();
        return 0;
    }
    case WM_RBUTTONUP: ClosePreview(); return 0;
    case WM_SIZE: if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Atalho na pasta Inicializar (em vez de chave Run no registro).
static void SetAutostart(bool on) {
    PWSTR startup = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startup))) return;
    std::wstring lnk = std::wstring(startup) + L"\\DesignPeek.lnk";
    CoTaskMemFree(startup);
    if (!on) { DeleteFileW(lnk.c_str()); return; }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    IShellLinkW* sl = nullptr; IPersistFile* pf = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(exe);
        sl->SetDescription(L"DesignPeek - preview com Espaço");
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
        sl->Release();
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    if (cmd && (wcsstr(cmd, L"/autostart") || wcsstr(cmd, L"/noautostart"))) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetAutostart(!wcsstr(cmd, L"/noautostart"));
        CoUninitialize();
        return 0;
    }
    if (cmd && wcsstr(cmd, L"/quit")) { // encerra a instância em execução
        if (HWND w = FindWindowW(L"DesignPeekWnd", nullptr)) PostMessage(w, WM_CLOSE, 0, 0);
        return 0;
    }
    CreateMutexW(nullptr, TRUE, L"DesignPeek.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw)))
        return 1;

    WNDCLASSW wc = {};
    wc.style = CS_DROPSHADOW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DesignPeekWnd";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"DesignPeek", WS_POPUP,
                            0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    DWORD corner = 2; // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));

    g_focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    UpdateHotkeys();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    UnhookWinEvent(g_focusHook);
    UnhookWinEvent(g_fgHook);
    DiscardDeviceResources();
    SafeRelease(g_titleFmt); SafeRelease(g_infoFmt); SafeRelease(g_dw); SafeRelease(g_d2d);
    CoUninitialize();
    return 0;
}
