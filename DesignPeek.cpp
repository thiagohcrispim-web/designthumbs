// DesignPeek - "Quick Look" para o Explorer: selecione um arquivo e aperte Espaço.
//   Espaço/Esc: fecha         Setas/clique em outro arquivo: troca o preview
//   Roda do mouse/+/-: zoom   Arrastar: mover        Z: 100%   0: ajustar   Duplo clique: alterna
//   PgUp/PgDn: páginas        Enter: abre no programa padrão  Ctrl+C: copia a imagem
// Visual de vidro (acrylic) com cantos arredondados, seguindo o tema claro/escuro do Windows.
// Segurar Alt com arquivos selecionados mostra botões para converter em PNG, JPG ou PDF.
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
#include "Ui.h"
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

struct Config { bool space = true, alt = true, dpi300 = false, pdfRaw = false; DWORD altDelay = 500, quality = 92; };
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
    g_cfg.alt = ReadDword(L"AltButtons", 1) != 0;
    g_cfg.altDelay = max(150ul, min(3000ul, (unsigned long)ReadDword(L"AltDelay", 500)));
    g_cfg.quality = max(50ul, min(100ul, (unsigned long)ReadDword(L"JpgQuality", 92)));
    g_cfg.dpi300 = ReadDword(L"ConvertDpi", 72) == 300;
    g_cfg.pdfRaw = ReadDword(L"PdfUncompressed", 0) != 0;
}
static void SaveConfig() {
    WriteDword(L"SpacePreview", g_cfg.space); WriteDword(L"AltButtons", g_cfg.alt);
    WriteDword(L"AltDelay", g_cfg.altDelay); WriteDword(L"JpgQuality", g_cfg.quality);
    WriteDword(L"ConvertDpi", g_cfg.dpi300 ? 300 : 72); WriteDword(L"PdfUncompressed", g_cfg.pdfRaw);
}

// ---------- estado ----------

static HINSTANCE g_inst;
static HWND g_wnd, g_altWnd, g_toastWnd, g_settingsWnd;
static HWINEVENTHOOK g_focusHook, g_fgHook, g_selHook;
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
static int g_arrowHot;            // seta de página sob o mouse (-1/1)
static POINT g_dragFrom;

// Direct2D / DirectWrite
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1Bitmap* g_gpuBmp;
static ID2D1BitmapBrush* g_checker;

enum { kHotSpace = 1, kHotEsc, kHotEnter, kHotCopy, kHotZ, kHot0, kHotNum0, kHotPlus, kHotMinus, kHotAdd, kHotSub, kHotPgUp, kHotPgDn };
static const struct { int id; UINT mod, vk; } kOpenKeys[] = {
    {kHotEsc, 0, VK_ESCAPE}, {kHotEnter, 0, VK_RETURN}, {kHotCopy, MOD_CONTROL, 'C'}, {kHotZ, 0, 'Z'},
    {kHot0, 0, '0'}, {kHotNum0, 0, VK_NUMPAD0}, {kHotPlus, 0, VK_OEM_PLUS}, {kHotMinus, 0, VK_OEM_MINUS},
    {kHotAdd, 0, VK_ADD}, {kHotSub, 0, VK_SUBTRACT}, {kHotPgUp, 0, VK_PRIOR}, {kHotPgDn, 0, VK_NEXT},
};
enum { kRefreshTimer = 1, kFlashTimer, kAltTimer, kToastTimer, kSelTimer, kAnimTimer };
static const float kCaption = 56, kPad = 18;
static ui::Theme g_theme = ui::MakeTheme(true, false);
static DWORD g_animStart;

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

// Ordem em que os arquivos foram selecionados (cliques). O Explorer não guarda essa ordem, então ela é
// acompanhada aqui: a cada mudança de seleção, itens novos entram no fim e os desmarcados saem.
static HWND g_orderView;
static std::vector<std::wstring> g_selOrder;

static void SyncSelectionOrder(HWND defView) {
    std::vector<std::wstring> cur = SelectedFiles(defView);
    if (defView != g_orderView) { g_orderView = defView; g_selOrder = cur; return; }
    std::set<std::wstring> now(cur.begin(), cur.end());
    std::vector<std::wstring> kept;
    for (auto& f : g_selOrder) if (now.count(f)) kept.push_back(f);
    std::set<std::wstring> had(kept.begin(), kept.end());
    for (auto& f : cur) if (!had.count(f)) kept.push_back(f); // novos (Shift+clique/Ctrl+A entram na ordem da pasta)
    g_selOrder.swap(kept);
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

// Altura reservada embaixo: cápsula de informações e, se houver, a de fontes.
static float BottomH() { return 66 + (g_hasInfo && !g_info.fonts.empty() ? 38 : 0); }

static D2D1_RECT_F ImageArea() {
    RECT rc; GetClientRect(g_wnd, &rc);
    return D2D1::RectF(kPad * g_ui, kCaption * g_ui, rc.right - kPad * g_ui, rc.bottom - BottomH() * g_ui);
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
    float maxW = (wa.right - wa.left) * 0.85f - 2 * kPad * g_ui, maxH = (wa.bottom - wa.top) * 0.85f - (kCaption + BottomH()) * g_ui;
    float s = min(maxW / g_nw, maxH / g_nh);
    s = min(s, g_nativeKnown ? 1.0f : 2.0f);
    int w = max((int)(g_nw * s + 2 * kPad * g_ui), (int)(640 * g_ui));
    int h = max((int)(g_nh * s + (kCaption + BottomH()) * g_ui), (int)(320 * g_ui));
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
static bool OpenPath(const std::wstring& p, const std::vector<std::wstring>& neighbors);

static bool OpenSelection() {
    std::vector<std::wstring> neighbors;
    std::wstring p = FocusedFile(g_defView, &neighbors);
    return OpenPath(p, neighbors);
}

// Abre o preview de um arquivo (seleção do Explorer ou "DesignPeek.exe /peek arquivo").
static bool OpenPath(const std::wstring& p, const std::vector<std::wstring>& neighbors) {
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

    g_ui = (g_defView ? GetDpiForWindow(g_defView) : GetDpiForSystem()) / 96.0f;
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
    if (!g_open) { g_animStart = GetTickCount(); SetTimer(g_wnd, kAnimTimer, 15, nullptr); } // leve fade + escala ao abrir
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

// ---------- conversão (botões do Alt) ----------

static std::vector<std::wstring> g_convFiles;
static bool g_converting;
static std::wstring g_toastTitle, g_toastText;
static int g_toastState;           // 0 convertendo, 1 pronto, 2 erro
static UINT g_toastDone, g_toastTotal;
static std::vector<std::wstring> g_toastOutputs;
static int g_altHot = -1;
static ULONGLONG g_altDownAt;
static bool g_altFired;          // botões já mostrados neste toque

static void ShowToast(const std::wstring& title, const std::wstring& text, DWORD hideAfterMs, int state) {
    g_toastTitle = title; g_toastText = text; g_toastState = state;
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (g_defView) mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    float ui = GetDpiForWindow(g_defView ? g_defView : GetDesktopWindow()) / 96.0f;
    int w = (int)(380 * ui), h = (int)(80 * ui), m = (int)(18 * ui);
    SetWindowPos(g_toastWnd, HWND_TOPMOST, mi.rcWork.right - w - m, mi.rcWork.bottom - h - m, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toastWnd, nullptr, TRUE);
    KillTimer(g_toastWnd, kToastTimer);
    if (hideAfterMs) SetTimer(g_toastWnd, kToastTimer, hideAfterMs, nullptr);
}

static void StartConversion(conv::Kind kind) {
    if (g_converting) { ShowToast(L"Aguarde", L"Já há uma conversão em andamento.", 3000, 0); return; }
    std::vector<std::wstring> files = g_convFiles;
    if (files.empty()) return;
    g_converting = true;
    g_toastOutputs.clear();
    static const wchar_t* names[] = {L"PNG", L"JPG", L"PDF"};
    g_toastDone = 0; g_toastTotal = (UINT)files.size();
    ShowToast(std::wstring(L"Convertendo para ") + names[kind], L"Preparando…", 0, 0);
    conv::Options opt;
    opt.quality = g_cfg.quality / 100.0f;
    opt.dpi = g_cfg.dpi300 ? 300 : 72;
    opt.pdfRaw = g_cfg.pdfRaw;
    HWND wnd = g_wnd;
    std::thread([files, kind, opt, wnd] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* r = new conv::Result(conv::Convert(files, kind, opt, [wnd](size_t d, size_t t) { PostMessage(wnd, WM_CONV_PROGRESS, d, t); }));
        if (!PostMessage(wnd, WM_CONV_DONE, kind, (LPARAM)r)) delete r;
        CoUninitialize();
    }).detach();
}

static void HideAltPopup() { if (g_altWnd && IsWindowVisible(g_altWnd)) ShowWindow(g_altWnd, SW_HIDE); g_altHot = -1; }

static void ShowAltPopup() {
    SyncSelectionOrder(g_defView);
    g_convFiles = g_selOrder;
    if (g_convFiles.empty()) return;
    POINT pt; GetCursorPos(&pt);
    float ui = GetDpiForWindow(g_defView) / 96.0f;
    int w = (int)(330 * ui), h = (int)(112 * ui);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = max((int)mi.rcWork.left, min((int)mi.rcWork.right - w, (int)pt.x - w / 2));
    int y = pt.y - h - (int)(14 * ui);
    if (y < mi.rcWork.top) y = pt.y + (int)(20 * ui); // sem espaço acima: mostra abaixo do cursor
    SetWindowPos(g_altWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_altWnd, nullptr, TRUE);
}

static bool CursorOver(HWND h) {
    if (!h || !IsWindowVisible(h)) return false;
    POINT pt; GetCursorPos(&pt);
    RECT rc; GetWindowRect(h, &rc);
    return PtInRect(&rc, pt) != 0;
}

// Alt segurado parado por g_cfg.altDelay ms com a lista de arquivos em foco mostra os botões.
// Qualquer clique ou outra tecla com o Alt apertado reinicia a contagem (Alt+Tab, Alt+Enter...).
static void AltTick() {
    bool alt = GetAsyncKeyState(VK_MENU) < 0;
    if (!alt) {
        g_altDownAt = 0;
        g_altFired = false;
        if (IsWindowVisible(g_altWnd) && !CursorOver(g_altWnd)) HideAltPopup();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!g_altDownAt) g_altDownAt = now;
    for (int vk = 1; vk < 255; ++vk)
        if (vk != VK_MENU && vk != VK_LMENU && vk != VK_RMENU && GetAsyncKeyState(vk) < 0) { g_altDownAt = now; g_altFired = false; break; }
    if (!g_altFired && !g_open && !IsWindowVisible(g_altWnd) && now - g_altDownAt >= g_cfg.altDelay) {
        g_altFired = true;
        if (HWND dv = FocusedFileList()) { g_defView = dv; ShowAltPopup(); }
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
    // Alt só é observado com a lista de arquivos em foco (ou com os botões visíveis)
    static bool watching;
    bool watch = g_cfg.alt && (list || IsWindowVisible(g_altWnd));
    if (watch != watching) {
        if (watch) SetTimer(g_wnd, kAltTimer, 40, nullptr); else { KillTimer(g_wnd, kAltTimer); g_altDownAt = 0; }
        watching = watch;
    }
}

static void CALLBACK FocusEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (g_cfg.alt && event != EVENT_SYSTEM_FOREGROUND && hwnd) {
        HWND list = FocusedFileList();
        if (list && (GetParent(hwnd) == list || hwnd == list)) SetTimer(g_wnd, kSelTimer, 60, nullptr); // seleção pode ter mudado
    }
    if (g_open && event == EVENT_OBJECT_FOCUS && hwnd && GetParent(hwnd) == g_defView)
        SetTimer(g_wnd, kRefreshTimer, 90, nullptr); // seleção mudou (setas/clique): atualiza o preview
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd != g_wnd && hwnd != g_altWnd && hwnd != g_toastWnd) {
        if (g_open && GetAncestor(hwnd, GA_ROOT) != GetAncestor(g_defView, GA_ROOT)) ClosePreview(); // foi para outro programa
        if (!CursorOver(g_altWnd)) HideAltPopup();
    }
    UpdateHotkeys();
}

static void OpenSettings();

static void OnHotkey(int id) {
    switch (id) {
    case kHotSpace:
        if (g_open) ClosePreview();
        else if (HWND dv = FocusedFileList()) { HideAltPopup(); g_defView = dv; OpenSelection(); }
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
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96); // pixels; transparência para o vidro
        if (FAILED(g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt))) return;
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!g_checker) { // xadrez para transparência
        UINT32 a = g_theme.dark ? 0xFF3A3A3C : 0xFFFFFFFF, b = g_theme.dark ? 0xFF2C2C2E : 0xFFE5E5EA;
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

static D2D1_RECT_F GearRectF() { return D2D1::RectF(14 * g_ui, 12 * g_ui, 46 * g_ui, 44 * g_ui); }
// Setas de página nas laterais da imagem (InDesign e outros com várias páginas).
static D2D1_RECT_F PageArrow(int dir) {
    D2D1_RECT_F a = ImageArea();
    float d = 40 * g_ui, cy = (a.top + a.bottom) / 2, x = dir < 0 ? a.left + 10 * g_ui : a.right - 10 * g_ui - d;
    return D2D1::RectF(x, cy - d / 2, x + d, cy + d / 2);
}
static int PageArrowAt(POINT pt) {
    if (g_pages <= 1) return 0;
    if (g_page > 0 && ui::Hit(PageArrow(-1), pt)) return -1;
    if (g_page < g_pages - 1 && ui::Hit(PageArrow(1), pt)) return 1;
    return 0;
}

static RECT GearRect() { D2D1_RECT_F r = GearRectF(); return {(LONG)r.left, (LONG)r.top, (LONG)r.right, (LONG)r.bottom}; }

// Informações em "chips" na cápsula flutuante de baixo.
static std::vector<std::wstring> InfoChips() {
    std::vector<std::wstring> parts;
    wchar_t t[128];
    const fmt::Info& in = g_info;
    if (!g_hasInfo) { parts.push_back(L"Carregando informações…"); return parts; }
    if (in.w && in.h) {
        swprintf(t, 128, L"%u × %u px", in.w, in.h);
        parts.push_back(t);
        if (in.dpi >= 1 && in.note.empty()) {
            swprintf(t, 128, L"%.0f dpi · %.1f × %.1f cm", in.dpi, in.w / in.dpi * 2.54, in.h / in.dpi * 2.54);
            for (wchar_t* c = t; *c; ++c) if (*c == L'.' && c[1] != L' ') *c = L',';
            parts.push_back(t);
        }
    }
    if (!in.pageSize.empty()) parts.push_back(in.pageSize);
    if (in.pages > 1) { swprintf(t, 128, L"%u páginas", in.pages); parts.push_back(t); }
    if (!in.color.empty()) {
        std::wstring c = in.color;
        if (in.bits) { swprintf(t, 128, L" · %u bits", in.bits); c += t; }
        parts.push_back(c);
    }
    if (!in.camera.empty()) parts.push_back(in.camera);
    if (g_fileSize) parts.push_back(FormatSize(g_fileSize));
    if (!in.app.empty()) parts.push_back(in.app);
    if (!in.note.empty()) parts.push_back(in.note);
    return parts;
}

static float Ease(float t) { t = max(0.0f, min(1.0f, t)); return 1 - (1 - t) * (1 - t) * (1 - t); }

static void Paint() {
    CreateDeviceResources();
    if (!g_rt) return;
    if (!g_gpuBmp && g_dib) {
        DIBSECTION ds;
        if (GetObject(g_dib, sizeof(ds), &ds) == sizeof(ds))
            g_rt->CreateBitmap(D2D1::SizeU(g_bw, g_bh), ds.dsBm.bmBits, g_bw * 4,
                               D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &g_gpuBmp);
    }
    const ui::Theme& th = g_theme;
    D2D1_SIZE_F size = g_rt->GetSize();
    float s = g_ui, anim = Ease((GetTickCount() - g_animStart) / 160.0f);
    g_rt->BeginDraw();
    g_rt->Clear(th.tint);
    ui::Painter p(g_rt, g_dw, th, s);

    // imagem flutuando, com cantos arredondados
    D2D1_RECT_F a = ImageArea();
    if (g_gpuBmp) {
        float k = 0.97f + 0.03f * anim;
        float w = g_nw * g_scale * k, h = g_nh * g_scale * k;
        float cx = (a.left + a.right) / 2 + g_ox, cy = (a.top + a.bottom) / 2 + g_oy;
        D2D1_RECT_F dst = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        float radius = 14 * s;
        if (g_fitMode) p.Shadow(dst, radius);
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        D2D1_RECT_F clipRect = g_fitMode ? dst : a;
        g_d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(clipRect, radius, radius), &clip);
        ID2D1Layer* layer = nullptr;
        g_rt->CreateLayer(&layer);
        if (clip && layer) g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), anim), layer);
        if (g_alpha && g_checker) g_rt->FillRectangle(dst, g_checker);
        ID2D1DeviceContext* dc = nullptr;
        bool pixelPeep = w / g_bw >= 3; // zoom alto: pixels nítidos
        if (SUCCEEDED(g_rt->QueryInterface(IID_PPV_ARGS(&dc)))) {
            dc->DrawBitmap(g_gpuBmp, &dst, 1.0f, pixelPeep ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            dc->Release();
        } else g_rt->DrawBitmap(g_gpuBmp, dst, 1.0f, pixelPeep ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (clip && layer) g_rt->PopLayer();
        SafeRelease(layer); SafeRelease(clip);
        if (g_fitMode) p.Stroke(dst, radius, th.stroke);
    } else p.Text(L"Carregando…", a, 13, th.text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

    // setas de página
    if (g_pages > 1)
        for (int d : {-1, 1}) {
            D2D1_RECT_F r = PageArrow(d);
            bool enabled = d < 0 ? g_page > 0 : g_page < g_pages - 1;
            float rad = (r.bottom - r.top) / 2;
            p.Fill(r, rad, th.dark ? ui::Rgba(0x1C1C1E, g_arrowHot == d ? 0.85f : 0.6f) : ui::Rgba(0xFFFFFF, g_arrowHot == d ? 0.95f : 0.75f));
            p.Stroke(r, rad, th.stroke);
            p.Icon(d < 0 ? L"\uE76B" : L"\uE76C", r, 14, enabled ? th.text : th.text2);
        }

    // barra de cima: engrenagem, nome, zoom/página
    p.CircleButton(GearRectF(), L"", g_gearHot);
    float side = 170 * s;
    p.Text(g_name, D2D1::RectF(side, 12 * s, size.width - side, 44 * s), 14, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    wchar_t t[64];
    swprintf(t, 64, L"%.0f%%", g_scale * 100 * (g_nativeKnown ? 1.0f : g_nw / g_bw));
    std::wstring zoom = t;
    float zw = p.Measure(zoom, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 24 * s, x = size.width - 14 * s;
    D2D1_RECT_F zr = D2D1::RectF(x - zw, 14 * s, x, 42 * s);
    p.Glass(zr, 14 * s);
    p.Text(zoom, zr, 12, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    x = zr.left - 8 * s;
    if (g_pages > 1) {
        swprintf(t, 64, L"%d / %d", g_page + 1, g_pages);
        std::wstring pg = t;
        float pw = p.Measure(pg, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 24 * s;
        D2D1_RECT_F pr = D2D1::RectF(x - pw, 14 * s, x, 42 * s);
        p.Glass(pr, 14 * s);
        p.Text(pg, pr, 12, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (!g_flash.empty() || g_loading) {
        std::wstring f = g_flash.empty() ? std::wstring(L"Carregando…") : g_flash;
        p.Text(f, D2D1::RectF(54 * s, 12 * s, side, 44 * s), 12, g_flash.empty() ? th.text2 : th.accent);
    }

    // cápsula flutuante (ornament) com as informações
    std::vector<std::wstring> chips = InfoChips();
    float gap = 22 * s, padX = 18 * s, maxW = size.width - 40 * s;
    std::vector<float> widths;
    float total = padX * 2;
    for (auto& c : chips) {
        float w = p.Measure(c, widths.empty() ? 12 : 12, widths.empty() ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL) + 4 * s;
        if (total + w + (widths.empty() ? 0 : gap) > maxW) break; // o que não cabe fica de fora
        total += w + (widths.empty() ? 0 : gap);
        widths.push_back(w);
    }
    float ob = size.height - 14 * s, oh = 36 * s;
    D2D1_RECT_F orn = D2D1::RectF((size.width - total) / 2, ob - oh, (size.width + total) / 2, ob);
    p.Glass(orn, oh / 2);
    float cxp = orn.left + padX;
    for (size_t i = 0; i < widths.size(); ++i) {
        if (i) { // separador fino
            D2D1_RECT_F sep = D2D1::RectF(cxp + gap / 2 - 0.5f * s, orn.top + 11 * s, cxp + gap / 2 + 0.5f * s, orn.bottom - 11 * s);
            p.Fill(sep, 0, th.stroke);
            cxp += gap;
        }
        p.Text(chips[i], D2D1::RectF(cxp, orn.top, cxp + widths[i] + 2, orn.bottom), 12, i == 0 ? th.text : th.text2, i == 0 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        cxp += widths[i];
    }

    // fontes do documento (faltando = vermelho)
    if (!g_info.fonts.empty()) {
        std::wstring l2 = L"Fontes   ";
        std::vector<DWRITE_TEXT_RANGE> missing;
        for (size_t i = 0; i < g_info.fonts.size(); ++i) {
            if (i) l2 += L"  ·  ";
            bool ok = FontInstalled(g_info.fontFamilies[i]);
            if (!ok) missing.push_back({(UINT32)l2.size(), (UINT32)g_info.fonts[i].size() + 2});
            l2 += g_info.fonts[i];
            if (!ok) l2 += L" ⚠";
        }
        float fw = min(p.Measure(l2, 11.5f) * 1.15f + 2 * padX, maxW), fh = 30 * s; // folga para os nomes em negrito
        D2D1_RECT_F fr = D2D1::RectF((size.width - fw) / 2, orn.top - 8 * s - fh, (size.width + fw) / 2, orn.top - 8 * s);
        p.Glass(fr, fh / 2);
        IDWriteTextFormat* f = p.Format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        IDWriteTextLayout* lay = nullptr;
        ID2D1SolidColorBrush *txt = nullptr, *red = nullptr, *lbl = nullptr;
        g_rt->CreateSolidColorBrush(th.text, &txt); g_rt->CreateSolidColorBrush(th.danger, &red); g_rt->CreateSolidColorBrush(th.text2, &lbl);
        if (f && txt && red && lbl && SUCCEEDED(g_dw->CreateTextLayout(l2.c_str(), (UINT32)l2.size(), f, fr.right - fr.left - 2 * padX, fh, &lay))) {
            lay->SetDrawingEffect(lbl, {0, 6});
            for (auto& r : missing) { lay->SetDrawingEffect(red, r); lay->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r); }
            g_rt->DrawTextLayout(D2D1::Point2F(fr.left + padX, fr.top), lay, txt);
            lay->Release();
        }
        SafeRelease(txt); SafeRelease(red); SafeRelease(lbl);
        if (f) f->Release();
    }
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

// Tema do Windows mudou (claro/escuro): reaplica o vidro e redesenha tudo.
static void ApplyTheme() {
    bool dark = ui::SystemDark();
    bool glass = true;
    for (HWND w : {g_wnd, g_altWnd, g_toastWnd, g_settingsWnd})
        if (w) glass &= ui::EnableGlass(w, dark);
    g_theme = ui::MakeTheme(dark, glass);
    SafeRelease(g_checker);
    for (HWND w : {g_wnd, g_altWnd, g_toastWnd, g_settingsWnd}) if (w) InvalidateRect(w, nullptr, FALSE);
}

// ---------- janela do preview ----------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY: OnHotkey((int)wp); return 0;
    case WM_LOADED: OnLoaded((Loaded*)lp); return 0;
    case WM_CONV_PROGRESS: {
        wchar_t t[96];
        g_toastDone = (UINT)wp; g_toastTotal = (UINT)lp;
        swprintf(t, 96, L"%u de %u arquivo%ls", (unsigned)wp, (unsigned)lp, lp > 1 ? L"s" : L"");
        g_toastText = t;
        InvalidateRect(g_toastWnd, nullptr, FALSE);
        return 0;
    }
    case WM_CONV_DONE: {
        auto* r = (conv::Result*)lp;
        g_converting = false;
        g_toastOutputs = r->outputs;
        static const wchar_t* names[] = {L"PNG", L"JPG", L"PDF"};
        wchar_t t[160];
        if (r->outputs.empty()) ShowToast(L"Não foi possível converter", L"Nenhum arquivo gerou imagem.", 6000, 2);
        else {
            if (wp == conv::kPdf) swprintf(t, 160, L"%ls", PathFindFileNameW(r->outputs[0].c_str()));
            else swprintf(t, 160, L"%u arquivo%ls %ls", (unsigned)r->outputs.size(), r->outputs.size() > 1 ? L"s" : L"", names[wp]);
            std::wstring text = t;
            if (r->failed) text += L"  ·  " + std::to_wstring(r->failed) + L" com erro";
            ShowToast(L"Pronto · clique para ver na pasta", text, 7000, 1);
        }
        delete r;
        return 0;
    }
    case WM_OPEN_SETTINGS: OpenSettings(); return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) ApplyTheme();
        return 0;
    case WM_TIMER:
        if (wp == kAltTimer) { AltTick(); return 0; }
        if (wp == kAnimTimer) { InvalidateRect(h, nullptr, FALSE); if (GetTickCount() - g_animStart > 180) KillTimer(h, wp); return 0; }
        if (wp == kSelTimer) { KillTimer(h, wp); if (HWND dv = FocusedFileList()) SyncSelectionOrder(dv); return 0; }
        KillTimer(h, wp);
        if (wp == kRefreshTimer && g_open) OpenSelection();
        if (wp == kFlashTimer) { g_flash.clear(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // o foco continua no Explorer (setas funcionam)
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        RECT gr = GearRect();
        if (PtInRect(&gr, pt) || PageArrowAt(pt)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
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
        if (int d = PageArrowAt(pt)) { ChangePage(d); return 0; }
        g_dragging = true;
        g_dragFrom = pt;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT gr = GearRect();
        bool hotNow = PtInRect(&gr, p) != 0;
        int arrow = g_dragging ? 0 : PageArrowAt(p);
        if (hotNow != g_gearHot || arrow != g_arrowHot) {
            g_gearHot = hotNow;
            g_arrowHot = arrow;
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
    case WM_MOUSELEAVE: if (g_gearHot || g_arrowHot) { g_gearHot = false; g_arrowHot = 0; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: g_dragging = false; ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK: {
        POINT dp = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (int d = PageArrowAt(dp)) { ChangePage(d); return 0; } // cliques rápidos nas setas
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

// ---------- botões do Alt e aviso (vidro, Direct2D) ----------

static ui::Surface g_altSurf, g_toastSurf, g_setSurf;

static float WndScale(HWND h) { return GetDpiForWindow(h) / 96.0f; }

static D2D1_RECT_F AltButton(HWND h, int i) {
    RECT rc; GetClientRect(h, &rc);
    float s = WndScale(h), pad = 12 * s, gap = 8 * s, top = 38 * s;
    float bw = (rc.right - 2 * pad - 2 * gap) / 3;
    return D2D1::RectF(pad + i * (bw + gap), top, pad + i * (bw + gap) + bw, rc.bottom - pad);
}

static LRESULT CALLBACK AltProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hotNow = -1;
        for (int i = 0; i < 3; ++i) if (ui::Hit(AltButton(h, i), p)) hotNow = i;
        if (hotNow != g_altHot) { g_altHot = hotNow; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_altHot >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE;
    case WM_MOUSELEAVE:
        g_altHot = -1;
        if (GetAsyncKeyState(VK_MENU) >= 0) HideAltPopup(); // soltou o Alt e saiu de cima: some
        else InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (int i = 0; i < 3; ++i)
            if (ui::Hit(AltButton(h, i), p)) { HideAltPopup(); StartConversion((conv::Kind)i); break; }
        return 0;
    }
    case WM_SIZE: g_altSurf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        g_altSurf.hwnd = h;
        if (g_altSurf.Begin(g_d2d)) {
            float s = WndScale(h);
            ID2D1RenderTarget* rt = g_altSurf.rt;
            D2D1_SIZE_F sz = rt->GetSize();
            rt->Clear(g_theme.tint);
            ui::Painter p(rt, g_dw, g_theme, s);
            p.Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 8 * s, g_theme.stroke);
            size_t n = g_convFiles.size();
            std::wstring title = n == 1 ? L"Converter “" + std::wstring(PathFindFileNameW(g_convFiles[0].c_str())) + L"”"
                                        : L"Converter " + std::to_wstring(n) + L" arquivos";
            p.Text(title, D2D1::RectF(16 * s, 6 * s, sz.width - 16 * s, 34 * s), 12, g_theme.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            static const wchar_t* labels[] = {L"PNG", L"JPG", L"PDF"};
            static const wchar_t* icons[] = {L"", L"", L""};
            for (int i = 0; i < 3; ++i) {
                D2D1_RECT_F r = AltButton(h, i);
                bool hot = g_altHot == i;
                float rad = 14 * s;
                if (hot) p.Fill(r, rad, g_theme.accent); else p.Glass(r, rad);
                D2D1_COLOR_F c = hot ? g_theme.onAccent : g_theme.text;
                bool sub = i == 2 && n > 1;
                p.Icon(icons[i], D2D1::RectF(r.left, r.top + 6 * s, r.right, r.top + 28 * s), 16, c);
                p.Text(labels[i], D2D1::RectF(r.left, r.top + 26 * s, r.right, r.top + (sub ? 44 : 50) * s), 13, c, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                if (sub) p.Text(std::to_wstring(n) + L" páginas", D2D1::RectF(r.left, r.top + 42 * s, r.right, r.bottom - 4 * s), 10,
                                hot ? g_theme.onAccent : g_theme.text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            g_altSurf.End();
        }
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
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_converting ? IDC_ARROW : IDC_HAND)); return TRUE;
    case WM_LBUTTONUP: if (!g_converting) { ShowWindow(h, SW_HIDE); RevealOutputs(); } return 0;
    case WM_RBUTTONUP: if (!g_converting) ShowWindow(h, SW_HIDE); return 0;
    case WM_SIZE: g_toastSurf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        g_toastSurf.hwnd = h;
        if (g_toastSurf.Begin(g_d2d)) {
            float s = WndScale(h);
            ID2D1RenderTarget* rt = g_toastSurf.rt;
            D2D1_SIZE_F sz = rt->GetSize();
            rt->Clear(g_theme.tint);
            ui::Painter p(rt, g_dw, g_theme, s);
            p.Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 8 * s, g_theme.stroke);
            // ícone redondo: progresso, pronto ou erro
            D2D1_RECT_F ic = D2D1::RectF(16 * s, (sz.height - 40 * s) / 2, 56 * s, (sz.height + 40 * s) / 2);
            p.Fill(ic, 20 * s, g_toastState == 2 ? g_theme.danger : g_theme.accent);
            p.Icon(g_toastState == 0 ? L"" : g_toastState == 1 ? L"" : L"", ic, 16, g_theme.onAccent);
            float x = 70 * s;
            p.Text(g_toastTitle, D2D1::RectF(x, 14 * s, sz.width - 16 * s, 36 * s), 13.5f, g_theme.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            p.Text(g_toastText, D2D1::RectF(x, 36 * s, sz.width - 16 * s, 56 * s), 12, g_theme.text2);
            if (g_toastState == 0) { // barra de progresso arredondada
                D2D1_RECT_F bar = D2D1::RectF(x, sz.height - 18 * s, sz.width - 18 * s, sz.height - 13 * s);
                p.Fill(bar, 2.5f * s, g_theme.track);
                float f = g_toastTotal ? (float)g_toastDone / g_toastTotal : 0.05f;
                bar.right = bar.left + max(5 * s, (bar.right - bar.left) * f);
                p.Fill(bar, 2.5f * s, g_theme.accent);
            }
            g_toastSurf.End();
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- configurações (painel de vidro desenhado à mão) ----------

static void SetAutostart(bool on);
static bool AutostartEnabled();

enum { kTogSpace = 1, kTogAlt, kDelayMinus, kDelayPlus, kDpi72, kDpi300, kPdfSmall, kPdfRaw, kQualMinus, kQualPlus, kTogAuto, kBtnFolder, kBtnQuit, kBtnClose };

static const wchar_t* kAbout =
    L"O DesignThumbs ensina o Explorer a entender arquivos de design e fotografia: miniaturas e painel de visualização (Alt+P) "
    L"para Photoshop, Illustrator, InDesign, EPS, SVG, CorelDRAW, Affinity, XD, Sketch, Krita, Procreate e RAW de câmeras.\n\n"
    L"Preview com Espaço: selecione um arquivo e aperte Espaço. Setas trocam de arquivo, a roda do mouse dá zoom e arrastar move. "
    L"Z mostra em 100%, 0 ajusta, PgUp/PgDn troca páginas do InDesign, Enter abre no programa e Ctrl+C copia a imagem. "
    L"Embaixo aparecem tamanho, DPI, cor, câmera e as fontes usadas (as que faltam ficam em vermelho).\n\n"
    L"Conversão rápida: selecione arquivos e segure Alt para converter em PNG, JPG ou PDF. Com vários arquivos, o PDF segue "
    L"a ordem em que você clicou neles.\n\n"
    L"Leve: não escuta o teclado e só trabalha quando você pede.";

static std::vector<std::pair<int, D2D1_RECT_F>> g_setHits;
static int g_setHot;
static float g_setScroll, g_setContentH;

static void SettingsAction(int id) {
    switch (id) {
    case kTogSpace: g_cfg.space = !g_cfg.space; break;
    case kTogAlt: g_cfg.alt = !g_cfg.alt; break;
    case kDelayMinus: g_cfg.altDelay = max(150ul, (unsigned long)g_cfg.altDelay - 100); break;
    case kDelayPlus: g_cfg.altDelay = min(3000ul, (unsigned long)g_cfg.altDelay + 100); break;
    case kDpi72: g_cfg.dpi300 = false; break;
    case kDpi300: g_cfg.dpi300 = true; break;
    case kPdfSmall: g_cfg.pdfRaw = false; break;
    case kPdfRaw: g_cfg.pdfRaw = true; break;
    case kQualMinus: g_cfg.quality = max(50ul, (unsigned long)g_cfg.quality - 2); break;
    case kQualPlus: g_cfg.quality = min(100ul, (unsigned long)g_cfg.quality + 2); break;
    case kTogAuto: SetAutostart(!AutostartEnabled()); break;
    case kBtnFolder: {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(exe, nullptr, &pidl, 0, nullptr))) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); CoTaskMemFree(pidl); }
        return;
    }
    case kBtnQuit: DestroyWindow(g_settingsWnd); PostMessage(g_wnd, WM_CLOSE, 0, 0); return;
    case kBtnClose: DestroyWindow(g_settingsWnd); return;
    default: return;
    }
    SaveConfig();
    UpdateHotkeys();
}

static void PaintSettings(HWND h) {
    g_setSurf.hwnd = h;
    if (!g_setSurf.Begin(g_d2d)) return;
    float s = WndScale(h);
    ID2D1RenderTarget* rt = g_setSurf.rt;
    D2D1_SIZE_F sz = rt->GetSize();
    const ui::Theme& th = g_theme;
    rt->Clear(th.tint);
    ui::Painter p(rt, g_dw, th, s);
    g_setHits.clear();
    auto hit = [&](int id, D2D1_RECT_F r) { g_setHits.push_back({id, r}); return g_setHot == id; };

    // cabeçalho fixo
    p.Text(L"DesignPeek", D2D1::RectF(28 * s, 22 * s, sz.width - 80 * s, 54 * s), 24, th.text, DWRITE_FONT_WEIGHT_BOLD);
    p.Text(L"Configurações", D2D1::RectF(28 * s, 52 * s, sz.width - 80 * s, 74 * s), 13, th.text2);
    D2D1_RECT_F close = D2D1::RectF(sz.width - 52 * s, 22 * s, sz.width - 22 * s, 52 * s);
    p.CircleButton(close, L"", hit(kBtnClose, close));

    // conteúdo rolável
    float top = 92 * s;
    D2D1_RECT_F view = D2D1::RectF(0, top, sz.width, sz.height);
    rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    float x = 20 * s, w = sz.width - 40 * s, y = top - g_setScroll, rowH = 50 * s;
    auto section = [&](const wchar_t* title) {
        p.Text(title, D2D1::RectF(x + 10 * s, y, x + w, y + 22 * s), 11.5f, th.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        y += 26 * s;
    };
    auto card = [&](int rows) { D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + rows * rowH); p.Glass(c, 16 * s); return c; };
    auto label = [&](int row, const D2D1_RECT_F& c, const wchar_t* t, const wchar_t* sub = nullptr) {
        float ry = c.top + row * rowH;
        if (row) p.Fill(D2D1::RectF(c.left + 16 * s, ry, c.right - 16 * s, ry + 1 * s), 0, th.stroke);
        if (sub) {
            p.Text(t, D2D1::RectF(c.left + 16 * s, ry + 7 * s, c.right - 200 * s, ry + 28 * s), 13, th.text);
            p.Text(sub, D2D1::RectF(c.left + 16 * s, ry + 26 * s, c.right - 200 * s, ry + 44 * s), 11, th.text2);
        } else p.Text(t, D2D1::RectF(c.left + 16 * s, ry, c.right - 200 * s, ry + rowH), 13, th.text);
        return ry;
    };
    auto toggle = [&](int id, const D2D1_RECT_F& c, float ry, bool on) {
        D2D1_RECT_F r = D2D1::RectF(c.right - 16 * s - 46 * s, ry + (rowH - 26 * s) / 2, c.right - 16 * s, ry + (rowH + 26 * s) / 2);
        p.Toggle(r, on, hit(id, r));
    };
    auto rightBox = [&](const D2D1_RECT_F& c, float ry, float bw) {
        return D2D1::RectF(c.right - 16 * s - bw, ry + (rowH - 30 * s) / 2, c.right - 16 * s, ry + (rowH + 30 * s) / 2);
    };

    section(L"PREVIEW");
    D2D1_RECT_F c = card(1);
    toggle(kTogSpace, c, label(0, c, L"Preview com a barra de Espaço", L"Selecione um arquivo e aperte Espaço"), g_cfg.space);
    y = c.bottom + 22 * s;

    section(L"CONVERSÃO RÁPIDA");
    c = card(5);
    toggle(kTogAlt, c, label(0, c, L"Botões ao segurar Alt", L"PNG, JPG ou PDF sobre o mouse"), g_cfg.alt);
    {
        float ry = label(1, c, L"Tempo segurando");
        D2D1_RECT_F r = rightBox(c, ry, 150 * s), mi, pl;
        int hot = g_setHot == kDelayMinus ? 0 : g_setHot == kDelayPlus ? 1 : -1;
        p.Stepper(r, std::to_wstring(g_cfg.altDelay) + L" ms", hot, &mi, &pl);
        hit(kDelayMinus, mi); hit(kDelayPlus, pl);
    }
    {
        float ry = label(2, c, L"Resolução");
        D2D1_RECT_F r = rightBox(c, ry, 180 * s), parts[2];
        std::wstring l[2] = {L"72 dpi", L"300 dpi"};
        p.Segmented(r, l, 2, g_cfg.dpi300 ? 1 : 0, g_setHot == kDpi72 ? 0 : g_setHot == kDpi300 ? 1 : -1, parts);
        hit(kDpi72, parts[0]); hit(kDpi300, parts[1]);
    }
    {
        float ry = label(3, c, L"PDF");
        D2D1_RECT_F r = rightBox(c, ry, 230 * s), parts[2];
        std::wstring l[2] = {L"Comprimido", L"Sem compressão"};
        p.Segmented(r, l, 2, g_cfg.pdfRaw ? 1 : 0, g_setHot == kPdfSmall ? 0 : g_setHot == kPdfRaw ? 1 : -1, parts);
        hit(kPdfSmall, parts[0]); hit(kPdfRaw, parts[1]);
    }
    {
        float ry = label(4, c, L"Qualidade JPG/PDF");
        D2D1_RECT_F r = rightBox(c, ry, 150 * s), mi, pl;
        int hot = g_setHot == kQualMinus ? 0 : g_setHot == kQualPlus ? 1 : -1;
        p.Stepper(r, std::to_wstring(g_cfg.quality) + L"%", hot, &mi, &pl);
        hit(kQualMinus, mi); hit(kQualPlus, pl);
    }
    y = c.bottom + 22 * s;

    section(L"SISTEMA");
    c = card(1);
    toggle(kTogAuto, c, label(0, c, L"Iniciar com o Windows"), AutostartEnabled());
    y = c.bottom + 12 * s;
    {
        float bw = (w - 10 * s) / 2;
        D2D1_RECT_F b1 = D2D1::RectF(x, y, x + bw, y + 36 * s), b2 = D2D1::RectF(x + bw + 10 * s, y, x + w, y + 36 * s);
        p.Pill(b1, L"Abrir pasta do programa", hit(kBtnFolder, b1));
        p.Pill(b2, L"Encerrar o DesignPeek", hit(kBtnQuit, b2));
        y = b1.bottom + 22 * s;
    }

    section(L"SOBRE");
    {
        float ph = p.Paragraph(kAbout, x + 16 * s, y + 14 * s, w - 32 * s, 12, th.text2, false);
        D2D1_RECT_F about = D2D1::RectF(x, y, x + w, y + ph + 28 * s);
        p.Glass(about, 16 * s);
        p.Paragraph(kAbout, x + 16 * s, y + 14 * s, w - 32 * s, 12, th.text2);
        y = about.bottom + 20 * s;
    }
    g_setContentH = y + g_setScroll - top;
    rt->PopAxisAlignedClip();
    p.Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 8 * s, th.stroke);
    g_setSurf.End();
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: { // arrastar pelo cabeçalho
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        float s = WndScale(h);
        bool onButton = false;
        for (auto& e : g_setHits) if (ui::Hit(e.second, pt)) onButton = true;
        return pt.y < 86 * s && !onButton ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = 0;
        for (auto& e : g_setHits) if (ui::Hit(e.second, pt)) hot = e.first;
        if (hot != g_setHot) { g_setHot = hot; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, g_setHot ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_MOUSELEAVE: if (g_setHot) { g_setHot = 0; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (auto& e : g_setHits) if (ui::Hit(e.second, pt)) { int id = e.first; SettingsAction(id); break; }
        if (g_settingsWnd) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        RECT rc; GetClientRect(h, &rc);
        float viewH = rc.bottom - 92 * WndScale(h);
        g_setScroll = max(0.0f, min(max(0.0f, g_setContentH - viewH), g_setScroll - GET_WHEEL_DELTA_WPARAM(wp) * 0.6f));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_SIZE: g_setSurf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        PaintSettings(h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_setSurf.Release(); g_setHits.clear(); g_settingsWnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenSettings() {
    if (g_settingsWnd) { ShowWindow(g_settingsWnd, SW_RESTORE); SetForegroundWindow(g_settingsWnd); return; }
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    if (HWND fg = GetForegroundWindow()) dpi = GetDpiForWindow(fg);
    float s = dpi / 96.0f;
    int w = (int)(540 * s), h = min((int)(760 * s), (int)(mi.rcWork.bottom - mi.rcWork.top - 40));
    g_setScroll = 0;
    g_setHot = 0;
    g_settingsWnd = CreateWindowExW(WS_EX_APPWINDOW, L"DesignPeekSettings", L"DesignPeek — Configurações", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                    mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2,
                                    w, h, nullptr, nullptr, g_inst, nullptr);
    ui::EnableGlass(g_settingsWnd, g_theme.dark);
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

// ---------- Novo > Pasta Padrão Maker ----------

static const wchar_t* kFolderSet[] = {L"Edit", L"Links", L"Preview"};
// O Explorer ordena o menu Novo pela extensão: começar com "0" deixa o item logo depois de Pasta e Atalho.
static const wchar_t* kNewExt = L"Software\\Classes\\.0pastapadraomaker";
static const wchar_t* kNewExtOld = L"Software\\Classes\\.designpeekfolders"; // v6
static const wchar_t* kNewProgId = L"Software\\Classes\\DesignThumbs.PastaPadraoMaker";

// Entrada no menu Novo do Explorer (por usuário). O Explorer chama "DesignPeek.exe /newfolders <arquivo novo>".
static void SetNewMenu(bool on) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kNewExt);
    RegDeleteTreeW(HKEY_CURRENT_USER, kNewExtOld);
    RegDeleteTreeW(HKEY_CURRENT_USER, kNewProgId);
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" /newfolders \"%1\"";
        auto set = [](const std::wstring& key, const wchar_t* name, const std::wstring& v) {
            RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        };
        set(kNewExt, nullptr, L"DesignThumbs.PastaPadraoMaker");
        set(std::wstring(kNewExt) + L"\\ShellNew", L"Command", cmd);
        set(kNewProgId, nullptr, L"Pasta Padrão Maker");
        set(std::wstring(kNewProgId) + L"\\DefaultIcon", nullptr, L"%SystemRoot%\\System32\\imageres.dll,3");
    }
    // o Explorer guarda a lista do menu Novo em cache
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Discardable\\PostSetup\\ShellNew");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Cria Edit, Links e Preview na pasta onde o usuário clicou e deixa as três selecionadas.
static void CreateFolderSet(const std::wstring& newItemPath) {
    std::wstring dir = conv::DirOf(newItemPath);
    if (GetFileAttributesW(newItemPath.c_str()) != INVALID_FILE_ATTRIBUTES && !(GetFileAttributesW(newItemPath.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
        DeleteFileW(newItemPath.c_str()); // caso o Explorer tenha criado o arquivo vazio
    std::vector<PIDLIST_ABSOLUTE> items;
    for (auto name : kFolderSet) {
        std::wstring f = dir + L"\\" + name;
        if (CreateDirectoryW(f.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
            SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW, f.c_str(), nullptr);
            PIDLIST_ABSOLUTE p = nullptr;
            if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &p, 0, nullptr))) items.push_back(p);
        }
    }
    PIDLIST_ABSOLUTE folder = nullptr;
    if (!items.empty() && SUCCEEDED(SHParseDisplayName(dir.c_str(), nullptr, &folder, 0, nullptr))) {
        std::vector<PCUITEMID_CHILD> children;
        for (auto p : items) children.push_back(ILFindLastID(p));
        SHOpenFolderAndSelectItems(folder, (UINT)children.size(), children.data(), 0);
        CoTaskMemFree(folder);
    }
    for (auto p : items) CoTaskMemFree(p);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    g_inst = inst;
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"/autostart") != std::wstring::npos || args.find(L"/noautostart") != std::wstring::npos) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool on = args.find(L"/noautostart") == std::wstring::npos;
        SetAutostart(on);
        SetStartMenu(on);
        SetNewMenu(on);
        CoUninitialize();
        return 0;
    }
    size_t nf = args.find(L"/newfolders");
    if (nf != std::wstring::npos) {
        std::wstring f = args.substr(nf + 11);
        f.erase(0, f.find_first_not_of(L" \"")); f.erase(f.find_last_not_of(L" \"") + 1);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CreateFolderSet(f);
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
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DesignPeekWnd";
    RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = AltProc; wc.lpszClassName = L"DesignPeekAlt"; RegisterClassW(&wc);
    wc.lpfnWndProc = ToastProc; wc.lpszClassName = L"DesignPeekToast"; RegisterClassW(&wc);
    wc.lpfnWndProc = SettingsProc; wc.lpszClassName = L"DesignPeekSettings";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    g_wnd = CreateWindowExW(exStyle, L"DesignPeekWnd", L"DesignPeek", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_altWnd = CreateWindowExW(exStyle, L"DesignPeekAlt", L"DesignPeek", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_toastWnd = CreateWindowExW(exStyle, L"DesignPeekToast", L"DesignPeek", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    ApplyTheme(); // vidro + tema claro/escuro do Windows

    std::thread(WorkerLoop).detach();
    g_focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_selHook = SetWinEventHook(EVENT_OBJECT_SELECTION, EVENT_OBJECT_SELECTIONWITHIN, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    UpdateHotkeys();
    if (args.find(L"/settings") != std::wstring::npos) OpenSettings();
    size_t peek = args.find(L"/peek ");
    if (peek != std::wstring::npos) { // abre um arquivo direto (útil em "Abrir com")
        std::wstring f = args.substr(peek + 6);
        f.erase(0, f.find_first_not_of(L" \"")); f.erase(f.find_last_not_of(L" \"") + 1);
        OpenPath(f, {});
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnhookWinEvent(g_focusHook);
    UnhookWinEvent(g_fgHook);
    UnhookWinEvent(g_selHook);
    DiscardDeviceResources();
    SafeRelease(g_dw); SafeRelease(g_d2d);
    ExitProcess(0); // a thread de carregamento é encerrada junto
}
