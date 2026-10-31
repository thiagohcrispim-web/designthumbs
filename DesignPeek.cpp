// DesignPeek - "Quick Look" para o Explorer: selecione um arquivo e aperte Espaço.
//   Espaço/Esc: fecha         Setas/clique em outro arquivo: troca o preview
//   Roda do mouse/+/-: zoom   Arrastar: mover        Z: 100%   0: ajustar   Duplo clique: alterna
//   PgUp/PgDn: páginas        Enter: abre no programa padrão  Ctrl+C: copia a imagem   B: fundo claro/escuro
// Segurar Shift com arquivos selecionados mostra botões para converter em PNG, JPG ou PDF.
// Sem hook de teclado: o Espaço só vira atalho (RegisterHotKey) enquanto o foco está na lista de
// arquivos, detectado por eventos de foco de acessibilidade (WinEvents).
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
#include <dwmapi.h>
#include "Convert.h" // antes do dwrite.h (ordem dos headers do MinGW)
#include <dwrite.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

using std::min; using std::max;
using conv::SafeRelease; using conv::Lower;

enum {
    WM_LOADED = WM_APP + 1, WM_CONV_PROGRESS, WM_CONV_DONE, WM_OPEN_SETTINGS,
};

// ---------- configurações (HKCU\Software\DesignThumbs) ----------

struct Config { bool space = true, shift = true, lightBg = false; DWORD shiftDelay = 500, quality = 92; };
static Config g_cfg;
static const wchar_t* kCfgKey = L"Software\\DesignThumbs";

static DWORD ReadDword(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kCfgKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
static void WriteDword(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kCfgKey, name, REG_DWORD, &v, sizeof(v)); }
static void LoadConfig() {
    g_cfg.space = ReadDword(L"SpacePreview", 1) != 0;
    g_cfg.shift = ReadDword(L"ShiftButtons", 1) != 0;
    g_cfg.lightBg = ReadDword(L"LightBackground", 0) != 0;
    g_cfg.shiftDelay = max(150ul, min(3000ul, (unsigned long)ReadDword(L"ShiftDelay", 500)));
    g_cfg.quality = max(50ul, min(100ul, (unsigned long)ReadDword(L"JpgQuality", 92)));
}
static void SaveConfig() {
    WriteDword(L"SpacePreview", g_cfg.space); WriteDword(L"ShiftButtons", g_cfg.shift); WriteDword(L"LightBackground", g_cfg.lightBg);
    WriteDword(L"ShiftDelay", g_cfg.shiftDelay); WriteDword(L"JpgQuality", g_cfg.quality);
}

// ---------- estado ----------

static HINSTANCE g_inst;
static HWND g_wnd, g_shiftWnd, g_toastWnd, g_settingsWnd;
static HWINEVENTHOOK g_focusHook, g_fgHook;
static bool g_open, g_spaceKey, g_openKeys;
static HWND g_defView;           // lista de arquivos de onde veio o Espaço
static UINT g_screen = 2048;     // lado maior da área de trabalho (tamanho do preview)

// arquivo atual
static std::wstring g_path, g_name, g_flash;
static fmt::Info g_info;
static bool g_hasInfo;
static UINT64 g_fileSize;
static int g_page, g_pages = 1;

// imagem exibida (DIB 32bpp pré-multiplicado) e sua cópia na GPU
static HBITMAP g_dib;
static bool g_dibOwned;          // miniatura temporária (não está no cache)
static UINT g_bw, g_bh, g_dibCx;
static bool g_alpha, g_full, g_loading, g_firstFull;

// visualização: escala em pixels de tela por pixel "nativo" (g_nw x g_nh)
static float g_nw = 1, g_nh = 1, g_scale = 1, g_ox, g_oy, g_ui = 1;
static bool g_fitMode = true, g_nativeKnown, g_dragging, g_gearHot;
static POINT g_dragFrom;

// Direct2D / DirectWrite
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1Bitmap* g_gpuBmp;
static ID2D1BitmapBrush* g_checker;
static IDWriteTextFormat *g_titleFmt, *g_infoFmt, *g_iconFmt;
static float g_fmtScale;

enum { kHotSpace = 1, kHotEsc, kHotEnter, kHotCopy, kHotZ, kHot0, kHotNum0, kHotPlus, kHotMinus, kHotAdd, kHotSub, kHotPgUp, kHotPgDn, kHotB };
static const struct { int id; UINT mod, vk; } kOpenKeys[] = {
    {kHotEsc, 0, VK_ESCAPE}, {kHotEnter, 0, VK_RETURN}, {kHotCopy, MOD_CONTROL, 'C'}, {kHotZ, 0, 'Z'},
    {kHot0, 0, '0'}, {kHotNum0, 0, VK_NUMPAD0}, {kHotPlus, 0, VK_OEM_PLUS}, {kHotMinus, 0, VK_OEM_MINUS},
    {kHotAdd, 0, VK_ADD}, {kHotSub, 0, VK_SUBTRACT}, {kHotPgUp, 0, VK_PRIOR}, {kHotPgDn, 0, VK_NEXT}, {kHotB, 0, 'B'},
};
enum { kRefreshTimer = 1, kFlashTimer, kShiftTimer, kToastTimer };
static const float kCaption = 40, kInfo = 52, kPad = 12;

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

static IShellView* ViewFromBrowser(IDispatch* disp, HWND defView) {
    IServiceProvider* sp = nullptr; IShellBrowser* sb = nullptr; IShellView* sv = nullptr;
    if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) &&
        SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
        SUCCEEDED(sb->QueryActiveShellView(&sv))) {
        HWND h = nullptr;
        sv->GetWindow(&h);
        if (h != defView) SafeRelease(sv);
    }
    SafeRelease(sb); SafeRelease(sp);
    return sv;
}

// IShellView da lista `defView` (janelas/abas do Explorer e área de trabalho).
static IShellView* ShellViewFor(HWND defView) {
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) return nullptr;
    IShellView* view = nullptr;
    long count = 0;
    sw->get_Count(&count);
    for (long i = 0; i < count && !view; ++i) {
        VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = i;
        IDispatch* d = nullptr;
        if (SUCCEEDED(sw->Item(v, &d)) && d) { view = ViewFromBrowser(d, defView); d->Release(); }
    }
    if (!view) {
        VARIANT empty; VariantInit(&empty);
        long hwnd; IDispatch* d = nullptr;
        if (sw->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &d) == S_OK && d) { view = ViewFromBrowser(d, defView); d->Release(); }
    }
    sw->Release();
    return view;
}

static std::wstring ItemPath(IShellItem* item) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { s = p; CoTaskMemFree(p); }
    return s;
}

static bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Arquivos selecionados, na ordem em que foram selecionados (ordem padrão do Explorer, sem SVGIO_FLAG_VIEWORDER).
static std::vector<std::wstring> SelectedFiles(HWND defView) {
    std::vector<std::wstring> files;
    IShellView* sv = ShellViewFor(defView);
    IShellItemArray* arr = nullptr;
    if (sv && SUCCEEDED(sv->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&arr)))) {
        DWORD n = 0;
        arr->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(arr->GetItemAt(i, &it))) {
                std::wstring p = ItemPath(it);
                if (IsFile(p)) files.push_back(p);
                it->Release();
            }
        }
    }
    SafeRelease(arr); SafeRelease(sv);
    return files;
}

// Arquivo com foco e vizinhos (para pré-carregar).
static std::wstring FocusedFile(HWND defView, std::vector<std::wstring>* neighbors = nullptr) {
    std::wstring path;
    IShellView* sv = ShellViewFor(defView);
    IFolderView2* fv = nullptr;
    if (sv && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) {
        int idx = -1, count = 0;
        if (SUCCEEDED(fv->GetSelectedItem(-1, &idx)) && idx >= 0) {
            int focused = -1;
            if (SUCCEEDED(fv->GetFocusedItem(&focused)) && focused >= 0) {
                // com vários selecionados, o item com foco é o que acabou de ser escolhido
                IShellItemArray* sel = nullptr;
                BOOL isSel = FALSE;
                if (SUCCEEDED(fv->GetSelection(FALSE, &sel))) {
                    IShellItem* fi = nullptr;
                    if (SUCCEEDED(fv->GetItem(focused, IID_PPV_ARGS(&fi)))) {
                        DWORD n = 0; sel->GetCount(&n);
                        for (DWORD i = 0; i < n && !isSel; ++i) {
                            IShellItem* si = nullptr; int order = 1;
                            if (SUCCEEDED(sel->GetItemAt(i, &si))) { si->Compare(fi, SICHINT_CANONICAL, &order); si->Release(); }
                            isSel = order == 0;
                        }
                        fi->Release();
                    }
                    sel->Release();
                }
                if (isSel) idx = focused;
            }
            IShellItem* it = nullptr;
            if (SUCCEEDED(fv->GetItem(idx, IID_PPV_ARGS(&it)))) { path = ItemPath(it); it->Release(); }
            fv->ItemCount(SVGIO_ALLVIEW, &count);
            if (neighbors)
                for (int d : {1, -1, 2}) {
                    int j = idx + d;
                    if (j < 0 || j >= count) continue;
                    IShellItem* ni = nullptr;
                    if (SUCCEEDED(fv->GetItem(j, IID_PPV_ARGS(&ni)))) {
                        std::wstring np = ItemPath(ni);
                        if (IsFile(np)) neighbors->push_back(np);
                        ni->Release();
                    }
                }
        }
    }
    SafeRelease(fv); SafeRelease(sv);
    return path;
}

// ---------- carregamento em segundo plano + cache ----------

struct Job { std::wstring path; UINT cx; int page; bool info; UINT screen; };
struct Loaded { std::wstring path; UINT cx = 0; int page = 0; HBITMAP bmp = nullptr; bool alpha = false; int pages = 1; bool hasInfo = false; fmt::Info info; };

static std::mutex g_qm;
static std::condition_variable g_qcv;
static std::deque<Job> g_queue;
static std::set<std::wstring> g_pending;

static std::wstring JobKey(const std::wstring& path, int page, UINT cx) { return path + L"|" + std::to_wstring(page) + L"|" + std::to_wstring(cx); }

static void WorkerLoop() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_qm);
            g_qcv.wait(lk, [] { return !g_queue.empty(); });
            j = g_queue.front();
            g_queue.pop_front();
        }
        auto* L = new Loaded;
        L->path = j.path; L->page = j.page;
        if (j.info) { conv::FileInfo(j.path, &L->info); L->hasInfo = true; }
        UINT cx = j.cx;
        if (!cx) { // automático: tamanho da tela, sem passar do nativo
            fmt::Info in = L->info;
            cx = j.screen;
            if (L->hasInfo && in.w && in.h && in.note.empty()) cx = min(cx, max(in.w, in.h));
        }
        L->cx = j.cx ? j.cx : 0;
        if (!conv::RenderFile(j.path, cx, j.page, &L->bmp, &L->alpha, &L->pages)) { L->bmp = conv::IconImage(j.path); L->alpha = true; }
        if (!PostMessage(g_wnd, WM_LOADED, 0, (LPARAM)L)) { if (L->bmp) DeleteObject(L->bmp); delete L; }
    }
}

static void Enqueue(const std::wstring& path, UINT cx, int page, bool info, bool urgent) {
    std::wstring key = JobKey(path, page, cx);
    if (!g_pending.insert(key).second) return;
    std::lock_guard<std::mutex> lk(g_qm);
    Job j{path, cx, page, info, g_screen};
    if (urgent) g_queue.push_front(j); else g_queue.push_back(j);
    g_qcv.notify_one();
}

struct CacheEntry { std::wstring path; int page; UINT cx; HBITMAP bmp; bool alpha; int pages; DWORD used; };
static std::vector<CacheEntry> g_cache;
static std::vector<std::pair<std::wstring, fmt::Info>> g_infoCache;

static const fmt::Info* CachedInfo(const std::wstring& path) {
    for (auto& e : g_infoCache) if (e.first == path) return &e.second;
    return nullptr;
}

static CacheEntry* Cached(const std::wstring& path, int page) {
    CacheEntry* best = nullptr;
    for (auto& e : g_cache) if (e.path == path && e.page == page && (!best || e.cx > best->cx)) best = &e;
    if (best) best->used = GetTickCount();
    return best;
}

static void CachePut(Loaded* L) {
    g_cache.push_back({L->path, L->page, L->cx, L->bmp, L->alpha, L->pages, GetTickCount()});
    while (g_cache.size() > 6) { // remove o menos usado (nunca o exibido)
        auto victim = g_cache.end();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->bmp != g_dib && (victim == g_cache.end() || it->used < victim->used)) victim = it;
        if (victim == g_cache.end()) break;
        DeleteObject(victim->bmp);
        g_cache.erase(victim);
    }
    if (L->hasInfo) {
        for (auto it = g_infoCache.begin(); it != g_infoCache.end(); ++it) if (it->first == L->path) { g_infoCache.erase(it); break; }
        g_infoCache.push_back({L->path, L->info});
        if (g_infoCache.size() > 32) g_infoCache.erase(g_infoCache.begin());
    }
}

// ---------- fontes instaladas ----------

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

// Pede uma versão maior quando o zoom passa da resolução atual.
static void EnsureResolution() {
    if (g_full || g_nw * g_scale <= g_bw * 1.1f) return;
    UINT maxTex = g_rt ? min(8192u, g_rt->GetMaximumBitmapSize()) : 8192u;
    g_full = true;
    Enqueue(g_path, min(conv::ExportSize(g_info), maxTex), g_page, false, true);
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
    SetZoom(one, (a.left + a.right) / 2, (a.top + a.bottom) / 2);
}

// ---------- exibição ----------

static void UpdateHotkeys();

static void SetDisplay(HBITMAP bmp, bool owned, bool alpha, UINT cx) {
    if (g_dib && g_dibOwned && g_dib != bmp) DeleteObject(g_dib);
    g_dib = bmp; g_dibOwned = owned; g_alpha = alpha; g_dibCx = cx;
    BITMAP bm = {};
    if (bmp) GetObject(bmp, sizeof(bm), &bm);
    g_bw = max(1L, bm.bmWidth); g_bh = max(1L, (LONG)std::abs(bm.bmHeight));
    SafeRelease(g_gpuBmp);
}

// Tamanho "nativo" usado pela escala (pixels reais quando conhecidos).
static void UpdateNativeSize() {
    g_nativeKnown = g_hasInfo && g_info.w && g_info.h && g_info.note.empty();
    if (g_hasInfo && g_info.w && g_info.h) { g_nw = (float)g_info.w; g_nh = (float)g_info.h; }
    else { g_nw = (float)g_bw; g_nh = (float)g_bh; }
    g_full = g_nativeKnown ? max(g_bw, g_bh) >= conv::ExportSize(g_info) : g_dibCx >= 4096;
}

static void ResizeToContent() {
    HMONITOR mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    float maxW = (wa.right - wa.left) * 0.85f - 2 * kPad * g_ui, maxH = (wa.bottom - wa.top) * 0.85f - (kCaption + kInfo) * g_ui;
    float s = min(maxW / g_nw, maxH / g_nh);
    s = min(s, g_nativeKnown ? 1.0f : 2.0f);
    int w = max((int)(g_nw * s + 2 * kPad * g_ui), (int)(640 * g_ui));
    int h = max((int)(g_nh * s + (kCaption + kInfo) * g_ui), (int)(320 * g_ui));
    SetWindowPos(g_wnd, HWND_TOPMOST, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                 w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_rt) g_rt->Resize(D2D1::SizeU(w, h));
}

static void ClosePreview() {
    g_open = false;
    g_dragging = false;
    ReleaseCapture();
    KillTimer(g_wnd, kRefreshTimer);
    ShowWindow(g_wnd, SW_HIDE);
    SetDisplay(nullptr, false, false, 0);
    g_path.clear();
    UpdateHotkeys();
}

static void Prefetch(const std::vector<std::wstring>& files) {
    for (auto& f : files)
        if (!Cached(f, 0)) Enqueue(f, 0, 0, CachedInfo(f) == nullptr, false);
}

// Abre (ou troca para) o arquivo com foco: mostra na hora o que houver em cache e carrega o resto em segundo plano.
static bool OpenSelection() {
    std::vector<std::wstring> neighbors;
    std::wstring p = FocusedFile(g_defView, &neighbors);
    if (p.empty() || !IsFile(p)) return g_open;
    if (g_open && p == g_path) return true;

    g_path = p;
    g_name = PathFindFileNameW(p.c_str());
    g_page = 0; g_pages = 1;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    g_fileSize = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) ? ((UINT64)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) : 0;
    const fmt::Info* ci = CachedInfo(p);
    g_hasInfo = ci != nullptr;
    g_info = ci ? *ci : fmt::Info();

    g_ui = GetDpiForWindow(g_defView) / 96.0f;
    HMONITOR mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_screen = (UINT)max(mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top);

    CacheEntry* e = Cached(p, 0);
    g_loading = !e;
    g_firstFull = !e;
    if (e) {
        SetDisplay(e->bmp, false, e->alpha, e->cx);
        g_pages = e->pages;
        if (!g_hasInfo) Enqueue(p, 1, 0, true, true); // só faltam as informações
    }
    else {
        HBITMAP t = conv::CachedThumb(p); // instantâneo; a versão nítida chega depois
        SetDisplay(t, true, t && conv::PrepareAlpha(t), 0);
        Enqueue(p, 0, 0, !g_hasInfo, true);
    }
    UpdateNativeSize();
    g_open = true;
    ResizeToContent();
    Fit();
    UpdateHotkeys();
    Prefetch(neighbors);
    return true;
}

static void OnLoaded(Loaded* L) {
    g_pending.erase(JobKey(L->path, L->page, L->cx));
    if (L->cx == 1) { // pedido só das informações
        if (L->bmp) DeleteObject(L->bmp);
        L->bmp = nullptr;
        if (L->hasInfo && g_open && L->path == g_path) { g_info = L->info; g_hasInfo = true; UpdateNativeSize(); Fit(); }
        if (L->hasInfo) g_infoCache.push_back({L->path, L->info});
        delete L;
        return;
    }
    if (!L->bmp) { delete L; return; }
    CachePut(L);
    if (g_open && L->path == g_path) {
        if (L->hasInfo) { g_info = L->info; g_hasInfo = true; }
        g_pages = L->pages;
        if (L->page == g_page && (g_loading || g_dibOwned || L->cx >= g_dibCx)) {
            SetDisplay(L->bmp, false, L->alpha, L->cx ? L->cx : g_screen);
            g_loading = false;
            float oldW = g_nw;
            UpdateNativeSize();
            if (g_firstFull) { // primeira versão nítida: ajusta a janela ao tamanho real
                g_firstFull = false;
                ResizeToContent();
                Fit();
            } else if (g_fitMode || oldW != g_nw) Fit();
            if (L->cx) g_full = true;
        }
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
    delete L;
}

static void ChangePage(int delta) {
    if (g_pages <= 1) return;
    int p = max(0, min(g_pages - 1, g_page + delta));
    if (p == g_page) return;
    g_page = p;
    if (CacheEntry* e = Cached(g_path, p)) SetDisplay(e->bmp, false, e->alpha, e->cx);
    else { g_loading = true; Enqueue(g_path, 0, p, false, true); }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void Flash(const wchar_t* text) {
    g_flash = text;
    SetTimer(g_wnd, kFlashTimer, 1500, nullptr);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// Ctrl+C: copia como PNG (mantém transparência) e como bitmap (sobre branco), na maior resolução.
static void CopyImage() {
    HBITMAP bmp = nullptr; bool alpha; int pages;
    bool own = false;
    if (!g_full && conv::RenderFile(g_path, conv::ExportSize(g_info), g_page, &bmp, &alpha, &pages)) own = true;
    else bmp = g_dib;
    BYTE* px; UINT w, h;
    if (!conv::PixelsOf(bmp, &px, &w, &h)) return;

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
    IStream* st = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, FALSE, &st)) && conv::Encode(bmp, GUID_ContainerFormatPng, 1, st))
        GetHGlobalFromStream(st, &pngMem);
    SafeRelease(st);
    if (own) DeleteObject(bmp);

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

// ---------- conversão (botões do Shift) ----------

static std::vector<std::wstring> g_convFiles;
static bool g_converting;
static std::wstring g_toastTitle, g_toastText;
static std::vector<std::wstring> g_toastOutputs;
static int g_shiftHot = -1;
static ULONGLONG g_shiftDownAt;
static bool g_shiftUsed;

static void ShowToast(const std::wstring& title, const std::wstring& text, DWORD hideAfterMs) {
    g_toastTitle = title; g_toastText = text;
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (g_defView) mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    float ui = GetDpiForWindow(g_defView ? g_defView : GetDesktopWindow()) / 96.0f;
    int w = (int)(380 * ui), h = (int)(72 * ui), m = (int)(16 * ui);
    SetWindowPos(g_toastWnd, HWND_TOPMOST, mi.rcWork.right - w - m, mi.rcWork.bottom - h - m, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toastWnd, nullptr, TRUE);
    KillTimer(g_toastWnd, kToastTimer);
    if (hideAfterMs) SetTimer(g_toastWnd, kToastTimer, hideAfterMs, nullptr);
}

static void StartConversion(conv::Kind kind) {
    if (g_converting) { ShowToast(L"Aguarde", L"Já há uma conversão em andamento.", 3000); return; }
    std::vector<std::wstring> files = g_convFiles;
    if (files.empty()) return;
    g_converting = true;
    g_toastOutputs.clear();
    static const wchar_t* names[] = {L"PNG", L"JPG", L"PDF"};
    ShowToast(std::wstring(L"Convertendo para ") + names[kind], L"Preparando…", 0);
    float q = g_cfg.quality / 100.0f;
    HWND wnd = g_wnd;
    std::thread([files, kind, q, wnd] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* r = new conv::Result(conv::Convert(files, kind, q, [wnd](size_t d, size_t t) { PostMessage(wnd, WM_CONV_PROGRESS, d, t); }));
        if (!PostMessage(wnd, WM_CONV_DONE, kind, (LPARAM)r)) delete r;
        CoUninitialize();
    }).detach();
}

static void HideShiftPopup() { if (g_shiftWnd && IsWindowVisible(g_shiftWnd)) ShowWindow(g_shiftWnd, SW_HIDE); g_shiftHot = -1; }

static void ShowShiftPopup() {
    g_convFiles = SelectedFiles(g_defView);
    if (g_convFiles.empty()) return;
    POINT pt; GetCursorPos(&pt);
    float ui = GetDpiForWindow(g_defView) / 96.0f;
    int w = (int)(300 * ui), h = (int)(92 * ui);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = max((int)mi.rcWork.left, min((int)mi.rcWork.right - w, (int)pt.x - w / 2));
    int y = pt.y - h - (int)(14 * ui);
    if (y < mi.rcWork.top) y = pt.y + (int)(20 * ui); // sem espaço acima: mostra abaixo do cursor
    SetWindowPos(g_shiftWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_shiftWnd, nullptr, TRUE);
}

static bool CursorOver(HWND h) {
    if (!h || !IsWindowVisible(h)) return false;
    POINT pt; GetCursorPos(&pt);
    RECT rc; GetWindowRect(h, &rc);
    return PtInRect(&rc, pt) != 0;
}

// Shift segurado sozinho (sem outra tecla ou clique) por g_cfg.shiftDelay ms com a lista de arquivos em foco.
static void ShiftTick() {
    bool shift = GetAsyncKeyState(VK_SHIFT) < 0;
    if (!shift) {
        g_shiftDownAt = 0;
        if (IsWindowVisible(g_shiftWnd) && !CursorOver(g_shiftWnd)) HideShiftPopup();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!g_shiftDownAt) { g_shiftDownAt = now; g_shiftUsed = false; }
    for (int vk = 1; vk < 255 && !g_shiftUsed; ++vk) // Shift+clique / Shift+setas = seleção, não mostrar
        if (vk != VK_SHIFT && vk != VK_LSHIFT && vk != VK_RSHIFT && GetAsyncKeyState(vk) < 0) g_shiftUsed = true;
    if (!g_shiftUsed && !g_open && !IsWindowVisible(g_shiftWnd) && now - g_shiftDownAt >= g_cfg.shiftDelay) {
        g_shiftUsed = true;
        if (HWND dv = FocusedFileList()) { g_defView = dv; ShowShiftPopup(); }
    }
}

// ---------- atalhos ----------

// Registra os atalhos só quando fazem sentido, para não atrapalhar a digitação.
static void UpdateHotkeys() {
    HWND list = FocusedFileList();
    bool space = g_open || (g_cfg.space && list);
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
    // Shift só é observado com a lista de arquivos em foco (ou com os botões visíveis)
    static bool watching;
    bool watch = g_cfg.shift && (list || IsWindowVisible(g_shiftWnd));
    if (watch != watching) {
        if (watch) SetTimer(g_wnd, kShiftTimer, 40, nullptr); else { KillTimer(g_wnd, kShiftTimer); g_shiftDownAt = 0; }
        watching = watch;
    }
}

static void CALLBACK FocusEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (g_open && event == EVENT_OBJECT_FOCUS && hwnd && GetParent(hwnd) == g_defView)
        SetTimer(g_wnd, kRefreshTimer, 90, nullptr); // seleção mudou (setas/clique): atualiza o preview
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd != g_wnd && hwnd != g_shiftWnd && hwnd != g_toastWnd) {
        if (g_open && GetAncestor(hwnd, GA_ROOT) != GetAncestor(g_defView, GA_ROOT)) ClosePreview(); // foi para outro programa
        if (!CursorOver(g_shiftWnd)) HideShiftPopup();
    }
    UpdateHotkeys();
}

static void OpenSettings();

static void OnHotkey(int id) {
    switch (id) {
    case kHotSpace:
        if (g_open) ClosePreview();
        else if (HWND dv = FocusedFileList()) { HideShiftPopup(); g_defView = dv; OpenSelection(); }
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
    case kHotB: g_cfg.lightBg = !g_cfg.lightBg; SaveConfig(); SafeRelease(g_checker); InvalidateRect(g_wnd, nullptr, FALSE); break;
    }
    UpdateHotkeys();
}

// ---------- desenho do preview ----------

static std::wstring FormatSize(UINT64 b) {
    wchar_t t[32];
    if (b >= (1ull << 30)) swprintf(t, 32, L"%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) swprintf(t, 32, L"%.1f MB", b / 1048576.0);
    else swprintf(t, 32, L"%.0f KB", max(1.0, b / 1024.0));
    for (wchar_t* c = t; *c; ++c) if (*c == L'.') *c = L',';
    return t;
}

static void CreateDeviceResources() {
    if (!g_rt) {
        RECT rc; GetClientRect(g_wnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
        rp.dpiX = rp.dpiY = 96; // trabalha em pixels
        if (FAILED(g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt))) return;
    }
    if (!g_checker) { // xadrez para transparência
        UINT32 a = g_cfg.lightBg ? 0xFFFFFFFF : 0xFF3A3A3A, b = g_cfg.lightBg ? 0xFFE2E2E2 : 0xFF2C2C2C;
        UINT32 cells[16 * 16];
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) cells[y * 16 + x] = ((x < 8) ^ (y < 8)) ? a : b;
        ID2D1Bitmap* cb = nullptr;
        if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(16, 16), cells, 64, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &cb))) {
            g_rt->CreateBitmapBrush(cb, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), &g_checker);
            cb->Release();
        }
    }
}

static void DiscardDeviceResources() { SafeRelease(g_gpuBmp); SafeRelease(g_checker); SafeRelease(g_rt); }

static void EnsureTextFormats() {
    if (g_titleFmt && g_fmtScale == g_ui) return;
    SafeRelease(g_titleFmt); SafeRelease(g_infoFmt); SafeRelease(g_iconFmt);
    g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14 * g_ui, L"pt-br", &g_titleFmt);
    g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12 * g_ui, L"pt-br", &g_infoFmt);
    const wchar_t* icons = FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    g_dw->CreateTextFormat(icons, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16 * g_ui, L"pt-br", &g_iconFmt);
    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    for (IDWriteTextFormat* f : {g_titleFmt, g_infoFmt, g_iconFmt}) {
        if (!f) continue;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(f, &ellipsis))) { f->SetTrimming(&trim, ellipsis); ellipsis->Release(); }
    }
    if (g_iconFmt) g_iconFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtScale = g_ui;
}

static RECT GearRect() { int s = (int)(kCaption * g_ui); return {(int)(4 * g_ui), 0, (int)(4 * g_ui) + s, s}; }

static std::wstring InfoLine() {
    std::vector<std::wstring> parts;
    wchar_t t[128];
    const fmt::Info& in = g_info;
    if (!g_hasInfo) return L"Carregando informações…";
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
    bool light = g_cfg.lightBg;
    D2D1_SIZE_F size = g_rt->GetSize();
    ID2D1SolidColorBrush *text = nullptr, *dim = nullptr, *red = nullptr, *accent = nullptr, *hot = nullptr;
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(light ? 0xF3F3F3 : 0x1E1E1E));
    g_rt->CreateSolidColorBrush(D2D1::ColorF(light ? 0x1E1E1E : 0xE6E6E6), &text);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(light ? 0x6A6A6A : 0x9A9A9A), &dim);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(light ? 0xC42B1C : 0xFF6B6B), &red);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(light ? 0x005FB8 : 0x4CC2FF), &accent);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(light ? 0x000000 : 0xFFFFFF, 0.08f), &hot);

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

    if (g_titleFmt && g_infoFmt) {
        float side = 160 * g_ui;
        // engrenagem (configurações)
        RECT gr = GearRect();
        D2D1_RECT_F gf = D2D1::RectF((float)gr.left, (float)gr.top + 4 * g_ui, (float)gr.right, (float)gr.bottom - 4 * g_ui);
        if (g_gearHot) g_rt->FillRoundedRectangle(D2D1::RoundedRect(gf, 4 * g_ui, 4 * g_ui), hot);
        if (g_iconFmt) g_rt->DrawText(L"", 1, g_iconFmt, gf, g_gearHot ? text : dim);
        if (!g_flash.empty() || g_loading) {
            const std::wstring& f = g_flash.empty() ? std::wstring(L"Carregando…") : g_flash;
            g_infoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_rt->DrawText(f.c_str(), (UINT32)f.size(), g_infoFmt, D2D1::RectF((float)gr.right + 4 * g_ui, 0, side, kCaption * g_ui), accent);
        }
        // título, página e zoom
        std::wstring right;
        wchar_t t[64];
        if (g_pages > 1) { swprintf(t, 64, L"Página %d de %d   ", g_page + 1, g_pages); right += t; }
        swprintf(t, 64, L"%.0f%%", g_scale * 100 * (g_nativeKnown ? 1.0f : g_nw / g_bw));
        right += t;
        g_titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_rt->DrawText(g_name.c_str(), (UINT32)g_name.size(), g_titleFmt, D2D1::RectF(side, 0, size.width - side, kCaption * g_ui), text);
        g_infoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_rt->DrawText(right.c_str(), (UINT32)right.size(), g_infoFmt, D2D1::RectF(size.width - side - kPad * g_ui, 0, size.width - kPad * g_ui, kCaption * g_ui), dim);

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
        } else l2 = L"Roda: zoom  ·  Arrastar: mover  ·  Z: 100%  ·  0: ajustar  ·  PgUp/PgDn: páginas  ·  Enter: abrir  ·  Ctrl+C: copiar  ·  B: fundo";
        IDWriteTextLayout* lay = nullptr;
        float lw = size.width - 2 * kPad * g_ui;
        if (SUCCEEDED(g_dw->CreateTextLayout(l2.c_str(), (UINT32)l2.size(), g_infoFmt, lw, lineH, &lay))) {
            for (auto& r : missing) lay->SetDrawingEffect(red, r);
            g_rt->DrawTextLayout(D2D1::Point2F(kPad * g_ui, top + 4 * g_ui + lineH), lay, g_info.fonts.empty() ? dim : text);
            lay->Release();
        }
    }
    SafeRelease(text); SafeRelease(dim); SafeRelease(red); SafeRelease(accent); SafeRelease(hot);
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

// ---------- janela do preview ----------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY: OnHotkey((int)wp); return 0;
    case WM_LOADED: OnLoaded((Loaded*)lp); return 0;
    case WM_CONV_PROGRESS: {
        wchar_t t[96];
        swprintf(t, 96, L"%u de %u arquivo%ls…", (unsigned)wp, (unsigned)lp, lp > 1 ? L"s" : L"");
        g_toastText = t;
        InvalidateRect(g_toastWnd, nullptr, TRUE);
        return 0;
    }
    case WM_CONV_DONE: {
        auto* r = (conv::Result*)lp;
        g_converting = false;
        g_toastOutputs = r->outputs;
        static const wchar_t* names[] = {L"PNG", L"JPG", L"PDF"};
        wchar_t t[160];
        if (r->outputs.empty()) ShowToast(L"Não foi possível converter", L"Nenhum arquivo gerou imagem.", 6000);
        else {
            if (wp == conv::kPdf) swprintf(t, 160, L"%ls", PathFindFileNameW(r->outputs[0].c_str()));
            else swprintf(t, 160, L"%u arquivo%ls %ls", (unsigned)r->outputs.size(), r->outputs.size() > 1 ? L"s" : L"", names[wp]);
            std::wstring text = t;
            if (r->failed) text += L"  ·  " + std::to_wstring(r->failed) + L" com erro";
            ShowToast(L"✓ Pronto — clique para mostrar na pasta", text, 7000);
        }
        delete r;
        return 0;
    }
    case WM_OPEN_SETTINGS: OpenSettings(); return 0;
    case WM_TIMER:
        if (wp == kShiftTimer) { ShiftTick(); return 0; }
        KillTimer(h, wp);
        if (wp == kRefreshTimer && g_open) OpenSelection();
        if (wp == kFlashTimer) { g_flash.clear(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // o foco continua no Explorer (setas funcionam)
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        RECT gr = GearRect();
        if (PtInRect(&gr, pt)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        SetZoom(g_scale * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.2f : 1 / 1.2f), (float)pt.x, (float)pt.y);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT gr = GearRect();
        if (PtInRect(&gr, pt)) { ClosePreview(); OpenSettings(); return 0; }
        g_dragging = true;
        g_dragFrom = pt;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT gr = GearRect();
        bool hotNow = PtInRect(&gr, p) != 0;
        if (hotNow != g_gearHot) {
            g_gearHot = hotNow;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (g_dragging) {
            g_ox += p.x - g_dragFrom.x; g_oy += p.y - g_dragFrom.y;
            g_dragFrom = p;
            g_fitMode = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: if (g_gearHot) { g_gearHot = false; InvalidateRect(h, nullptr, FALSE); } return 0;
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

// ---------- janelinhas desenhadas com GDI (botões do Shift e aviso) ----------

static HFONT MakeFont(float ui, int size, int weight) {
    return CreateFontW(-(int)(size * ui), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void FillRound(HDC dc, RECT r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static RECT ShiftButton(HWND h, int i) {
    RECT rc; GetClientRect(h, &rc);
    float ui = GetDpiForWindow(h) / 96.0f;
    int pad = (int)(10 * ui), gap = (int)(8 * ui), top = (int)(36 * ui);
    int bw = (rc.right - 2 * pad - 2 * gap) / 3;
    return {pad + i * (bw + gap), top, pad + i * (bw + gap) + bw, rc.bottom - pad};
}

static LRESULT CALLBACK ShiftProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hotNow = -1;
        for (int i = 0; i < 3; ++i) { RECT r = ShiftButton(h, i); if (PtInRect(&r, p)) hotNow = i; }
        if (hotNow != g_shiftHot) { g_shiftHot = hotNow; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_shiftHot = -1;
        if (GetAsyncKeyState(VK_SHIFT) >= 0) HideShiftPopup(); // soltou o Shift e saiu de cima: some
        else InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (int i = 0; i < 3; ++i) {
            RECT r = ShiftButton(h, i);
            if (PtInRect(&r, p)) { HideShiftPopup(); StartConversion((conv::Kind)i); break; }
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        float ui = GetDpiForWindow(h) / 96.0f;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, buf);
        HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);
        HFONT small = MakeFont(ui, 12, FW_NORMAL), big = MakeFont(ui, 14, FW_SEMIBOLD), tiny = MakeFont(ui, 10, FW_NORMAL);
        size_t n = g_convFiles.size();
        std::wstring title = n == 1 ? L"Converter “" + std::wstring(PathFindFileNameW(g_convFiles[0].c_str())) + L"”"
                                    : L"Converter " + std::to_wstring(n) + L" arquivos";
        SelectObject(mem, small);
        SetTextColor(mem, RGB(200, 200, 200));
        RECT tr = {(int)(12 * ui), 0, rc.right - (int)(12 * ui), (int)(34 * ui)};
        DrawTextW(mem, title.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        static const wchar_t* labels[] = {L"PNG", L"JPG", L"PDF"};
        for (int i = 0; i < 3; ++i) {
            RECT r = ShiftButton(h, i);
            FillRound(mem, r, g_shiftHot == i ? RGB(0, 103, 192) : RGB(58, 58, 58), (int)(8 * ui));
            SetTextColor(mem, RGB(255, 255, 255));
            SelectObject(mem, big);
            RECT lr = r;
            bool sub = i == 2 && n > 1;
            if (sub) lr.bottom -= (int)(10 * ui);
            DrawTextW(mem, labels[i], -1, &lr, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            if (sub) { // PDF com várias páginas
                SelectObject(mem, tiny);
                SetTextColor(mem, RGB(210, 210, 210));
                RECT sr = r; sr.top = r.bottom - (int)(20 * ui);
                std::wstring s = std::to_wstring(n) + L" páginas";
                DrawTextW(mem, s.c_str(), -1, &sr, DT_SINGLELINE | DT_CENTER | DT_TOP);
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(buf); DeleteDC(mem);
        DeleteObject(small); DeleteObject(big); DeleteObject(tiny);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Mostra as saídas no Explorer (todas da mesma pasta selecionadas).
static void RevealOutputs() {
    if (g_toastOutputs.empty()) return;
    std::wstring dir = conv::DirOf(g_toastOutputs[0]);
    PIDLIST_ABSOLUTE folder = nullptr;
    if (FAILED(SHParseDisplayName(dir.c_str(), nullptr, &folder, 0, nullptr))) return;
    std::vector<PIDLIST_ABSOLUTE> items;
    for (auto& o : g_toastOutputs) {
        PIDLIST_ABSOLUTE p = nullptr;
        if (conv::DirOf(o) == dir && SUCCEEDED(SHParseDisplayName(o.c_str(), nullptr, &p, 0, nullptr))) items.push_back(p);
    }
    std::vector<PCUITEMID_CHILD> children;
    for (auto p : items) children.push_back(ILFindLastID(p));
    SHOpenFolderAndSelectItems(folder, (UINT)children.size(), children.data(), 0);
    for (auto p : items) CoTaskMemFree(p);
    CoTaskMemFree(folder);
}

static LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER: KillTimer(h, wp); ShowWindow(h, SW_HIDE); return 0;
    case WM_LBUTTONUP: if (!g_converting) { ShowWindow(h, SW_HIDE); RevealOutputs(); } return 0;
    case WM_RBUTTONUP: if (!g_converting) ShowWindow(h, SW_HIDE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        float ui = GetDpiForWindow(h) / 96.0f;
        HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT big = MakeFont(ui, 14, FW_SEMIBOLD), small = MakeFont(ui, 12, FW_NORMAL);
        int pad = (int)(16 * ui);
        RECT r1 = {pad, (int)(10 * ui), rc.right - pad, rc.bottom / 2 + (int)(2 * ui)};
        RECT r2 = {pad, rc.bottom / 2 + (int)(2 * ui), rc.right - pad, rc.bottom - (int)(10 * ui)};
        SelectObject(dc, big); SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, g_toastTitle.c_str(), -1, &r1, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, small); SetTextColor(dc, RGB(190, 190, 190));
        DrawTextW(dc, g_toastText.c_str(), -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        DeleteObject(big); DeleteObject(small);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- configurações ----------

static void SetAutostart(bool on);
static bool AutostartEnabled();

enum { kIdSpace = 101, kIdShift, kIdDelay, kIdQuality, kIdAutostart, kIdFolder, kIdQuit, kIdClose, kIdAbout };

static const wchar_t* kAbout =
    L"O DesignThumbs deixa o Explorer do Windows entender arquivos de design e fotografia:\r\n\r\n"
    L"• Miniaturas e painel de visualização (Alt+P) para Photoshop (PSD/PSB), Illustrator (AI), InDesign (INDD), "
    L"EPS, SVG/SVGZ, CorelDRAW (CDR), Affinity, Adobe XD, Sketch, Krita, Procreate e RAW de câmeras "
    L"(CR3, CR2, NEF, ARW, DNG, RAF, ORF, RW2...).\r\n\r\n"
    L"• Preview com Espaço: selecione um arquivo e aperte Espaço.\r\n"
    L"    Espaço/Esc fecha · setas trocam de arquivo · roda do mouse dá zoom · arrastar move\r\n"
    L"    Z = 100% · 0 = ajustar · duplo clique alterna · PgUp/PgDn = páginas do InDesign\r\n"
    L"    Enter abre no programa · Ctrl+C copia a imagem · B alterna fundo claro/escuro\r\n"
    L"    A barra de baixo mostra tamanho, DPI, cor, câmera e as fontes usadas (faltando = vermelho ⚠).\r\n\r\n"
    L"• Converter com Shift: selecione um ou vários arquivos e segure Shift. Aparecem botões para PNG, JPG ou PDF. "
    L"Os arquivos são salvos ao lado dos originais. Com vários arquivos, o PDF reúne todos em páginas, "
    L"na ordem em que foram selecionados.\r\n\r\n"
    L"Leve: não fica escutando o teclado e só trabalha quando você pede.";

static HFONT g_settingsFont;

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        float ui = GetDpiForWindow(h) / 96.0f;
        auto S = [ui](int v) { return (int)(v * ui); };
        g_settingsFont = MakeFont(ui, 12, FW_NORMAL);
        HFONT title = MakeFont(ui, 18, FW_SEMIBOLD);
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hh, int id, HFONT f = nullptr) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(hh), h, (HMENU)(INT_PTR)id, g_inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_settingsFont), TRUE);
            return c;
        };
        make(L"STATIC", L"DesignPeek — Configurações", 0, 20, 14, 460, 30, 0, title);
        make(L"BUTTON", L"Preview com a barra de Espaço", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 56, 460, 24, kIdSpace);
        make(L"BUTTON", L"Botões de conversão ao segurar Shift", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 84, 300, 24, kIdShift);
        make(L"STATIC", L"Tempo segurando (ms):", SS_CENTERIMAGE, 40, 112, 170, 24, 0);
        make(L"EDIT", std::to_wstring(g_cfg.shiftDelay).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, 210, 112, 70, 24, kIdDelay);
        make(L"STATIC", L"Qualidade do JPG/PDF (50–100):", SS_CENTERIMAGE, 20, 144, 190, 24, 0);
        make(L"EDIT", std::to_wstring(g_cfg.quality).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, 210, 144, 70, 24, kIdQuality);
        make(L"BUTTON", L"Iniciar com o Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 176, 460, 24, kIdAutostart);
        make(L"EDIT", kAbout, WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, 212, 520, 250, kIdAbout);
        make(L"BUTTON", L"Abrir pasta do programa", WS_TABSTOP, 20, 476, 170, 30, kIdFolder);
        make(L"BUTTON", L"Encerrar o DesignPeek", WS_TABSTOP, 200, 476, 170, 30, kIdQuit);
        make(L"BUTTON", L"Fechar", BS_DEFPUSHBUTTON | WS_TABSTOP, 440, 476, 100, 30, kIdClose);
        CheckDlgButton(h, kIdSpace, g_cfg.space ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, kIdShift, g_cfg.shift ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, kIdAutostart, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        return 0;
    }
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == kIdSpace) g_cfg.space = IsDlgButtonChecked(h, kIdSpace) == BST_CHECKED;
        else if (id == kIdShift) g_cfg.shift = IsDlgButtonChecked(h, kIdShift) == BST_CHECKED;
        else if (id == kIdAutostart) SetAutostart(IsDlgButtonChecked(h, kIdAutostart) == BST_CHECKED);
        else if ((id == kIdDelay || id == kIdQuality) && HIWORD(wp) == EN_KILLFOCUS) {
            BOOL ok;
            UINT v = GetDlgItemInt(h, id, &ok, FALSE);
            if (id == kIdDelay) g_cfg.shiftDelay = ok ? max(150u, min(3000u, v)) : g_cfg.shiftDelay;
            else g_cfg.quality = ok ? max(50u, min(100u, v)) : g_cfg.quality;
            SetDlgItemInt(h, id, id == kIdDelay ? g_cfg.shiftDelay : g_cfg.quality, FALSE);
        } else if (id == kIdFolder) {
            wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            PIDLIST_ABSOLUTE p = nullptr;
            if (SUCCEEDED(SHParseDisplayName(exe, nullptr, &p, 0, nullptr))) { SHOpenFolderAndSelectItems(p, 0, nullptr, 0); CoTaskMemFree(p); }
        } else if (id == kIdQuit) { DestroyWindow(h); PostMessage(g_wnd, WM_CLOSE, 0, 0); return 0; }
        else if (id == kIdClose || id == IDCANCEL) { SetFocus(h); DestroyWindow(h); return 0; }
        else return 0;
        SaveConfig();
        UpdateHotkeys();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: {
        // grava os campos numéricos mesmo sem perder o foco
        BOOL ok;
        UINT v = GetDlgItemInt(h, kIdDelay, &ok, FALSE); if (ok) g_cfg.shiftDelay = max(150u, min(3000u, v));
        v = GetDlgItemInt(h, kIdQuality, &ok, FALSE); if (ok) g_cfg.quality = max(50u, min(100u, v));
        SaveConfig();
        g_settingsWnd = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenSettings() {
    if (g_settingsWnd) { ShowWindow(g_settingsWnd, SW_RESTORE); SetForegroundWindow(g_settingsWnd); return; }
    POINT pt; GetCursorPos(&pt);
    UINT dpi = GetDpiForWindow(GetDesktopWindow());
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    float ui = dpi / 96.0f;
    RECT r = {0, 0, (int)(560 * ui), (int)(520 * ui)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    g_settingsWnd = CreateWindowExW(0, L"DesignPeekSettings", L"DesignPeek", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2,
                                    w, h, nullptr, nullptr, g_inst, nullptr);
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
}

// Atalho na pasta Inicializar (em vez de chave Run no registro).
static std::wstring StartupLink() {
    PWSTR startup = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startup))) return L"";
    std::wstring lnk = std::wstring(startup) + L"\\DesignPeek.lnk";
    CoTaskMemFree(startup);
    return lnk;
}
static bool AutostartEnabled() { std::wstring l = StartupLink(); return !l.empty() && GetFileAttributesW(l.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void CreateLink(const std::wstring& lnk, const wchar_t* args, const wchar_t* desc) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    IShellLinkW* sl = nullptr; IPersistFile* pf = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(exe);
        sl->SetArguments(args);
        sl->SetDescription(desc);
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
        sl->Release();
    }
}

static void SetAutostart(bool on) {
    std::wstring lnk = StartupLink();
    if (lnk.empty()) return;
    if (on) CreateLink(lnk, L"", L"DesignPeek - preview com Espaço");
    else DeleteFileW(lnk.c_str());
}

// Atalho no menu Iniciar para abrir as configurações.
static void SetStartMenu(bool on) {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return;
    std::wstring lnk = std::wstring(programs) + L"\\DesignPeek.lnk";
    CoTaskMemFree(programs);
    if (on) CreateLink(lnk, L"/settings", L"DesignPeek - configurações"); else DeleteFileW(lnk.c_str());
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    g_inst = inst;
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"/autostart") != std::wstring::npos || args.find(L"/noautostart") != std::wstring::npos) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool on = args.find(L"/noautostart") == std::wstring::npos;
        SetAutostart(on);
        SetStartMenu(on);
        CoUninitialize();
        return 0;
    }
    if (args.find(L"/quit") != std::wstring::npos) { // encerra a instância em execução
        if (HWND w = FindWindowW(L"DesignPeekWnd", nullptr)) PostMessage(w, WM_CLOSE, 0, 0);
        return 0;
    }
    CreateMutexW(nullptr, TRUE, L"DesignPeek.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { // já rodando: abrir de novo = abrir as configurações
        if (HWND w = FindWindowW(L"DesignPeekWnd", nullptr)) { AllowSetForegroundWindow(ASFW_ANY); PostMessage(w, WM_OPEN_SETTINGS, 0, 0); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadConfig();
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
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = ShiftProc; wc.lpszClassName = L"DesignPeekShift"; RegisterClassW(&wc);
    wc.lpfnWndProc = ToastProc; wc.lpszClassName = L"DesignPeekToast"; RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = SettingsProc; wc.lpszClassName = L"DesignPeekSettings";
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    g_wnd = CreateWindowExW(exStyle, L"DesignPeekWnd", L"DesignPeek", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_shiftWnd = CreateWindowExW(exStyle, L"DesignPeekShift", L"DesignPeek", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_toastWnd = CreateWindowExW(exStyle, L"DesignPeekToast", L"DesignPeek", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    DWORD corner = 2; // DWMWCP_ROUND (Windows 11)
    BOOL dark = TRUE;
    for (HWND w : {g_wnd, g_shiftWnd, g_toastWnd}) {
        DwmSetWindowAttribute(w, 33, &corner, sizeof(corner));
        DwmSetWindowAttribute(w, 20, &dark, sizeof(dark));
    }

    std::thread(WorkerLoop).detach();
    g_focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    UpdateHotkeys();
    if (args.find(L"/settings") != std::wstring::npos) OpenSettings();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnhookWinEvent(g_focusHook);
    UnhookWinEvent(g_fgHook);
    DiscardDeviceResources();
    SafeRelease(g_titleFmt); SafeRelease(g_infoFmt); SafeRelease(g_iconFmt); SafeRelease(g_dw); SafeRelease(g_d2d);
    ExitProcess(0); // a thread de carregamento é encerrada junto
}
