// Vizu - "Quick Look" para o Explorer: selecione um arquivo e aperte Espaço.
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
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include "Convert.h" // antes do dwrite.h (ordem dos headers do MinGW)
#include "Jobs.h"
#include "VectorBundle.h"
#include <dwrite.h>
#include "Ui.h"
#include "Update.h"
#include "InstallerUi.h"
#include "ColorInfo.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>

using std::min; using std::max;
using conv::SafeRelease; using conv::Lower;

enum {
    WM_LOADED = WM_APP + 1, WM_CONV_PROGRESS, WM_CONV_DONE, WM_OPEN_SETTINGS, WM_UPDATE_FOUND, WM_UPDATE_NONE, WM_UPDATE_READY,
};

// ---------- configurações (HKCU\Software\DesignThumbs) ----------

struct Config { DWORD themeMode = 0, webpQuality = 75, neon = 0xFF6633; bool tray = true; bool space = true, alt = true, picker = true, autoUpdate = true, dpi300 = false, pdfRaw = false, vectorTextPaths = false, tempPermanent = false; DWORD altDelay = 500, quality = 75; };
static Config g_cfg;
static bool g_lite = false; // Vizu Lite: só prévia com Espaço e miniaturas
static const wchar_t* kCfgKey = L"Software\\DesignThumbs";

static DWORD ReadDword(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kCfgKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
static void WriteDword(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kCfgKey, name, REG_DWORD, &v, sizeof(v)); }
static void LoadConfig() {
    g_cfg.space = ReadDword(L"SpacePreview", 1) != 0;
    g_lite = vec::Edition() == vec::kLite;
    g_cfg.alt = !g_lite && ReadDword(L"AltButtons", 1) != 0;
    g_cfg.altDelay = max(150ul, min(3000ul, (unsigned long)ReadDword(L"AltDelay", 500)));
    g_cfg.quality = max(50ul, min(100ul, (unsigned long)ReadDword(L"JpgQuality", 75)));
    g_cfg.dpi300 = ReadDword(L"ConvertDpi", 72) == 300;
    g_cfg.picker = ReadDword(L"ColorPicker", 1) != 0;
    { // v9.2: novos temas (Minimalista Claro/Escuro, Padrão Windows, Crispy); migra o valor antigo
        DWORD old = ReadDword(L"Theme", 0), mig[4] = {ui::kThemeWindows, ui::kThemeLight, ui::kThemeDark, ui::kThemeCrispy};
        g_cfg.themeMode = min(4ul, (unsigned long)ReadDword(L"Theme2", mig[min(3ul, (unsigned long)old)]));
    }
    g_cfg.webpQuality = max(30ul, min(100ul, (unsigned long)ReadDword(L"WebpQuality", 75)));
    g_cfg.tray = ReadDword(L"TrayIcon", 1) != 0;
    g_cfg.autoUpdate = ReadDword(L"AutoUpdate", 1) != 0;
    g_cfg.pdfRaw = ReadDword(L"PdfUncompressed", 0) != 0;
    g_cfg.vectorTextPaths = ReadDword(L"VectorTextPaths", 0) != 0;
    g_cfg.tempPermanent = ReadDword(L"TempPermanent", 0) != 0;
    g_cfg.neon = ReadDword(L"NeonColor", 0xFF6633) & 0xFFFFFF; ui::NeonColor() = g_cfg.neon;
}
static void SaveConfig() {
    WriteDword(L"SpacePreview", g_cfg.space); WriteDword(L"AltButtons", g_cfg.alt);
    WriteDword(L"AltDelay", g_cfg.altDelay); WriteDword(L"JpgQuality", g_cfg.quality);
    WriteDword(L"ConvertDpi", g_cfg.dpi300 ? 300 : 72); WriteDword(L"ColorPicker", g_cfg.picker); WriteDword(L"Theme2", g_cfg.themeMode); WriteDword(L"WebpQuality", g_cfg.webpQuality); WriteDword(L"TrayIcon", g_cfg.tray); WriteDword(L"AutoUpdate", g_cfg.autoUpdate); WriteDword(L"PdfUncompressed", g_cfg.pdfRaw); WriteDword(L"VectorTextPaths", g_cfg.vectorTextPaths); WriteDword(L"TempPermanent", g_cfg.tempPermanent); WriteDword(L"NeonColor", g_cfg.neon);
}

// ---------- estado ----------

static HINSTANCE g_inst;
static HWND g_wnd, g_altWnd, g_toastWnd, g_settingsWnd, g_dimWnd;
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
static bool g_pickOn;              // conta-gotas: cursor sobre a imagem
static COLORREF g_pickColor;
static POINT g_pickPt, g_downPt;
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
enum { kRefreshTimer = 1, kFlashTimer, kAltTimer, kToastTimer, kSelTimer, kAnimTimer, kUpdateTimer };
static const float kCaption = 56, kPad = 18;
static ui::Theme g_theme = ui::MakeTheme(ui::kThemeDark, false);
static std::wstring g_uiFont; // fonte da interface escolhida pelo usuário (vazio = Bahnschrift)
static void ClockRender();
static bool FontInstalled(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf = {}; lf.lfCharSet = DEFAULT_CHARSET; wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *(bool*)p = true; return 0; }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}
static const wchar_t* DefaultUiFont() { // Inter, como no Design System Vizu; sem ela, a fonte do Windows
    static const wchar_t* f = nullptr;
    if (!f) f = FontInstalled(L"Inter") ? L"Inter" : FontInstalled(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    return f;
}
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

// Cor do pixel sob o cursor (para o conta-gotas). false fora da imagem ou em área transparente.
static bool PixelAt(POINT pt, COLORREF* c) {
    if (!g_dib) return false;
    D2D1_RECT_F a = ImageArea();
    float w = g_nw * g_scale, h = g_nh * g_scale;
    float left = (a.left + a.right) / 2 + g_ox - w / 2, top = (a.top + a.bottom) / 2 + g_oy - h / 2;
    if (pt.x < a.left || pt.x >= a.right || pt.y < a.top || pt.y >= a.bottom) return false;
    float fx = (pt.x - left) / w, fy = (pt.y - top) / h;
    if (fx < 0 || fx >= 1 || fy < 0 || fy >= 1) return false;
    DIBSECTION ds;
    if (GetObject(g_dib, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) return false;
    const BYTE* px = (const BYTE*)ds.dsBm.bmBits + ((size_t)(fy * g_bh) * g_bw + (size_t)(fx * g_bw)) * 4;
    if (px[3] < 8) return false;
    auto un = [&](BYTE v) { return (BYTE)min(255, v * 255 / px[3]); }; // tira a pré-multiplicação
    *c = RGB(un(px[2]), un(px[1]), un(px[0]));
    return true;
}

static std::wstring HexOf(COLORREF c) { wchar_t t[16]; swprintf(t, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c)); return t; }

static void CopyText(const std::wstring& t) {
    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (!m) return;
    memcpy(GlobalLock(m), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(m);
    if (OpenClipboard(g_wnd)) { EmptyClipboard(); if (SetClipboardData(CF_UNICODETEXT, m)) m = nullptr; CloseClipboard(); }
    if (m) GlobalFree(m);
}

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

// Abre o preview de um arquivo (seleção do Explorer ou "Vizu.exe /peek arquivo").
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
        Flash(T(L"Imagem copiada", L"Image copied", L"Imagen copiada"));
    }
    if (pngMem) GlobalFree(pngMem);
    if (dibMem) GlobalFree(dibMem);
}

// ---------- conversão (botões do Alt) ----------

static std::vector<std::wstring> g_convFiles;
static bool g_converting;
static std::wstring g_toastTitle, g_toastText;
static int g_toastState;           // 0 convertendo, 1 pronto, 2 erro
static std::wstring g_toastLog;    // detalhes do último erro de conversão
static int g_toastAction;          // clique: 0 mostra os arquivos, 1 instala a atualização
static upd::Release g_release;
static UINT g_toastDone, g_toastTotal;
static std::vector<std::wstring> g_toastOutputs;
static void ShowAltPopup();
static void HideAltPopup();
enum { WM_TRAY = WM_APP + 40, kTrayId = 1, kMenuSettings = 1, kMenuRestart, kMenuQuit };
static UINT g_taskbarCreated;
static void UpdateTrayIcon();
static void RemoveTrayIcon();
static void ShowTrayMenu();
static std::wstring g_jobLabel;

static bool CursorOver(HWND h) {
    if (!h || !IsWindowVisible(h)) return false;
    POINT pt; GetCursorPos(&pt);
    RECT rc; GetWindowRect(h, &rc);
    return PtInRect(&rc, pt) != 0;
}
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

// Procura versão nova (no máximo uma vez por dia, exceto quando pedido nas configurações).
static void CheckForUpdates(bool manual) {
    ULONGLONG now = GetTickCount64() / 1000 + 0;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ULL; // segundos
    DWORD sz = sizeof(ULONGLONG);
    ULONGLONG last = 0;
    RegGetValueW(HKEY_CURRENT_USER, kCfgKey, L"LastUpdateCheck", RRF_RT_REG_QWORD, nullptr, &last, &sz);
    if (!manual && t - last < 24 * 3600) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, kCfgKey, L"LastUpdateCheck", REG_QWORD, &t, sizeof(t));
    (void)now;
    HWND wnd = g_wnd;
    std::thread([manual, wnd] {
        upd::Release r;
        bool ok = upd::Latest(&r);
        if (ok && upd::Compare(upd::kVersion, r.tag) < 0) {
            auto* p = new upd::Release(r);
            if (!PostMessage(wnd, WM_UPDATE_FOUND, 0, (LPARAM)p)) delete p;
        } else if (manual) PostMessage(wnd, WM_UPDATE_NONE, ok, 0);
    }).detach();
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

static void StartOcr();
static const int kHotOcr = 77; // Ctrl+Alt+T: ler texto da tela
static void OnHotkey(int id) {
    if (id == kHotOcr) { StartOcr(); return; }
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
    if (!g_hasInfo) { parts.push_back(T(L"Carregando informações…", L"Loading details…", L"Cargando información…")); return parts; }
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
    if (in.pages > 1) { swprintf(t, 128, T(L"%u páginas", L"%u pages", L"%u páginas"), in.pages); parts.push_back(t); }
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
    ui::Backdrop(g_rt, th, size.width, size.height);
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
    } else p.Text(T(L"Carregando…", L"Loading…", L"Cargando…"), a, 13, th.text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

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

    // conta-gotas: cartão com a cor, o HEX e, embaixo, CMYK e PANTONE aproximados (clique copia só o HEX)
    if (g_cfg.picker && g_pickOn && !g_dragging) {
        std::wstring hex = HexOf(g_pickColor);
        const cinfo::Info& ci = cinfo::For(g_pickColor);
        const wchar_t* lead = T(L"Sugestão: ", L"Suggestion: ", L"Sugerencia: ");
        std::wstring cmykLine = lead + ci.cmyk;
        std::wstring pmsLine = lead + (ci.pantone.empty() ? std::wstring(T(L"PANTONE sem equivalente próximo", L"no close PANTONE match", L"sin PANTONE cercano")) : L"PANTONE " + ci.pantone);
        float tw = max(p.Measure(hex, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD), max(p.Measure(cmykLine, 11), p.Measure(pmsLine, 11)));
        float pw = tw + 48 * s, ph = 62 * s;
        float px = min(g_pickPt.x + 18 * s, size.width - pw - 8 * s), py = min(g_pickPt.y + 18 * s, size.height - ph - 8 * s);
        D2D1_RECT_F pr = D2D1::RectF(px, py, px + pw, py + ph);
        p.Fill(pr, 12 * s, th.dark ? ui::Rgba(0x1C1C1E, 0.92f) : ui::Rgba(0xFFFFFF, 0.96f));
        p.Stroke(pr, 12 * s, th.stroke);
        D2D1_RECT_F dot = D2D1::RectF(px + 9 * s, py + 9 * s, px + 27 * s, py + 27 * s);
        p.Fill(dot, 9 * s, D2D1::ColorF(GetRValue(g_pickColor) / 255.0f, GetGValue(g_pickColor) / 255.0f, GetBValue(g_pickColor) / 255.0f));
        p.Stroke(dot, 9 * s, th.stroke);
        p.Text(hex, D2D1::RectF(px + 36 * s, py + 6 * s, pr.right - 8 * s, py + 30 * s), 12, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        p.Text(cmykLine, D2D1::RectF(px + 36 * s, py + 27 * s, pr.right - 8 * s, py + 43 * s), 11, th.text2);
        p.Text(pmsLine, D2D1::RectF(px + 36 * s, py + 42 * s, pr.right - 8 * s, py + 58 * s), 11, th.text2);
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
        std::wstring f = g_flash.empty() ? std::wstring(T(L"Carregando…", L"Loading…", L"Cargando…")) : g_flash;
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
        std::wstring l2 = std::wstring(T(L"Fontes", L"Fonts", L"Fuentes")) + L"   ";
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
            lay->SetDrawingEffect(lbl, {0, (UINT32)wcslen(T(L"Fontes", L"Fonts", L"Fuentes"))});
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
// Menus nativos (bandeja) escuros quando o tema é escuro/Crispy (API não documentada do uxtheme, ordinal 135).
static void SetMenuDarkMode(bool dark) {
    typedef int(WINAPI * SetMode)(int);
    typedef void(WINAPI * Flush)();
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    auto set = (SetMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

// Tema: segue o Windows, claro, escuro ou Crispy. Reaplica o vidro e redesenha tudo.
static void ApplyTheme() {
    bool dark = g_cfg.themeMode == ui::kThemeWindows ? ui::SystemDark() : g_cfg.themeMode != ui::kThemeLight;
    bool glass = true;
    for (HWND w : {g_wnd, g_toastWnd, g_settingsWnd})
        if (w) glass &= ui::EnableGlass(w, dark);
    g_theme = ui::MakeTheme((int)g_cfg.themeMode, glass);
    g_theme.font = g_uiFont.empty() ? DefaultUiFont() : g_uiFont.c_str();
    SetMenuDarkMode(dark);
    SafeRelease(g_checker);
    for (HWND w : {g_wnd, g_toastWnd, g_settingsWnd}) if (w) InvalidateRect(w, nullptr, FALSE);
    ClockRender();
}

// ---------- janela do preview ----------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY: OnHotkey((int)wp); return 0;
    case WM_LOADED: OnLoaded((Loaded*)lp); return 0;
    case WM_CONV_PROGRESS: {
        wchar_t t[96];
        g_toastDone = (UINT)wp; g_toastTotal = (UINT)lp;
        swprintf(t, 96, T(L"%u de %u arquivo(s)", L"%u of %u file(s)", L"%u de %u archivo(s)"), (unsigned)wp, (unsigned)lp);
        g_toastText = t;
        InvalidateRect(g_toastWnd, nullptr, FALSE);
        return 0;
    }
    case WM_CONV_DONE: {
        auto* r = (jobs::Result*)lp;
        g_converting = false;
        g_toastOutputs = r->outputs;
        if (r->outputs.empty()) ShowToast(T(L"Não foi possível concluir", L"Couldn't finish", L"No se pudo completar"), g_jobLabel, 6000, 2);
        else {
            std::wstring text = r->outputs.size() == 1 ? std::wstring(PathFindFileNameW(r->outputs[0].c_str()))
                                                       : std::to_wstring(r->outputs.size()) + T(L" arquivos gerados", L" files created", L" archivos generados");
            if (r->failed) text += L"  ·  " + std::to_wstring(r->failed) + T(L" com erro", L" failed", L" con error");
            g_toastAction = 0;
            ShowToast(T(L"Pronto · clique para ver na pasta", L"Done · click to show in folder", L"Listo · clic para ver en la carpeta"), text, 7000, 1);
        }
        if (!r->errors.empty()) { // detalhes técnicos vão para um arquivo; o aviso fica curto
            std::wstring details;
            for (const auto& e : r->errors) details += e + L"\r\n\r\n";
            wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
            g_toastLog = std::wstring(tmp) + L"Vizu-conversao.txt";
            std::string u = vec::Utf8(L"\xFEFF" + details);
            HANDLE f = CreateFileW(g_toastLog.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD n; WriteFile(f, u.data(), (DWORD)u.size(), &n, nullptr); CloseHandle(f); }
            if (r->outputs.empty()) {
                std::wstring first = r->errors[0].substr(0, r->errors[0].find(L'\n'));
                if (first.size() > 140) first = first.substr(0, 140) + L"…";
                g_toastAction = 3;
                ShowToast(T(L"Não foi possível converter · clique para detalhes", L"Couldn't convert · click for details", L"No se pudo convertir · clic para detalles"), first, 9000, 2);
            }
        }
        delete r;
        return 0;
    }
    case WM_OPEN_SETTINGS: OpenSettings(); return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) ShowTrayMenu();
        return 0;
    case WM_UPDATE_FOUND: {
        auto* r = (upd::Release*)lp;
        g_release = *r;
        delete r;
        g_toastAction = 1;
        ShowToast(std::wstring(T(L"Nova versão ", L"New version ", L"Nueva versión ")) + g_release.tag,
                  T(L"Clique para atualizar", L"Click to update", L"Clic para actualizar"), 15000, 1);
        return 0;
    }
    case WM_UPDATE_NONE:
        g_toastAction = 2;
        ShowToast(wp ? T(L"Você já tem a versão mais recente", L"You're up to date", L"Ya tienes la versión más reciente")
                     : T(L"Não foi possível procurar atualização", L"Couldn't check for updates", L"No se pudo buscar actualización"),
                  std::wstring(T(L"Versão ", L"Version ", L"Versión ")) + upd::kVersion, 5000, wp ? 1 : 2);
        return 0;
    case WM_UPDATE_READY: {
        auto* path = (std::wstring*)lp;
        if (path->empty()) ShowToast(T(L"Falha ao baixar a atualização", L"Update download failed", L"Error al descargar la actualización"), g_release.tag, 6000, 2);
        else ShellExecuteW(nullptr, L"open", path->c_str(), L"/S /update", nullptr, SW_SHOWNORMAL); // modo silencioso
        delete path;
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) ApplyTheme();
        return 0;
    case WM_TIMER:
        if (wp == kAltTimer) { AltTick(); return 0; }
        if (wp == kUpdateTimer) { if (g_cfg.autoUpdate) CheckForUpdates(false); SetTimer(h, kUpdateTimer, 6 * 3600 * 1000, nullptr); return 0; }
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
        g_dragFrom = g_downPt = pt;
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
        if (g_cfg.picker && !g_dragging) {
            COLORREF c;
            bool on = !hotNow && !arrow && PixelAt(p, &c);
            if (on || g_pickOn) {
                g_pickOn = on; g_pickPt = p; if (on) g_pickColor = c;
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        if (g_dragging) {
            g_ox += p.x - g_dragFrom.x; g_oy += p.y - g_dragFrom.y;
            g_dragFrom = p;
            g_fitMode = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: if (g_gearHot || g_arrowHot || g_pickOn) { g_gearHot = false; g_arrowHot = 0; g_pickOn = false; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        bool click = g_dragging && abs(p.x - g_downPt.x) < 4 && abs(p.y - g_downPt.y) < 4;
        g_dragging = false;
        ReleaseCapture();
        COLORREF c;
        if (click && g_cfg.picker && PixelAt(p, &c)) { // clique sem arrastar: copia o HEX
            std::wstring hex = HexOf(c);
            CopyText(hex);
            Flash((hex + T(L" copiado", L" copied", L" copiado")).c_str());
        }
        return 0;
    }
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
    case WM_DESTROY: RemoveTrayIcon(); PostQuitMessage(0); return 0;
    }
    if (msg == g_taskbarCreated && g_taskbarCreated) { UpdateTrayIcon(); return 0; } // Explorer reiniciou: recoloca o ícone
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- botões do Alt e aviso (vidro, Direct2D) ----------

static ui::Surface g_toastSurf, g_setSurf;

static float WndScale(HWND h) { return GetDpiForWindow(h) / 96.0f; }

// ---------- execução das ações (em segundo plano, com aviso de progresso) ----------

struct Batch { std::vector<std::wstring> files; jobs::Job job; };

static jobs::Job BaseJob() {
    jobs::Job j;
    j.quality = g_cfg.quality / 100.0f;
    j.webpQuality = g_cfg.webpQuality / 100.0f;
    j.dpi = g_cfg.dpi300 ? 300 : 72;
    j.pdfRaw = g_cfg.pdfRaw;
    j.textToPaths = g_cfg.vectorTextPaths;
    return j;
}

static void StartBatches(std::vector<Batch> batches, const std::wstring& label) {
    if (g_converting) { ShowToast(T(L"Aguarde", L"Please wait", L"Espera"), T(L"Já há uma conversão em andamento.", L"A conversion is already running.", L"Ya hay una conversión en curso."), 3000, 0); return; }
    size_t total = 0;
    for (auto& b : batches) total += b.files.size();
    if (!total) return;
    g_converting = true;
    g_toastOutputs.clear();
    g_jobLabel = label;
    g_toastDone = 0; g_toastTotal = (UINT)total;
    g_toastAction = 0;
    ShowToast(label, T(L"Preparando…", L"Preparing…", L"Preparando…"), 0, 0);
    HWND wnd = g_wnd;
    std::thread([batches, total, wnd] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* all = new jobs::Result;
        size_t base = 0;
        for (auto& b : batches) {
            jobs::Result r = jobs::Run(b.files, b.job, [wnd, base, total](size_t d, size_t) { PostMessage(wnd, WM_CONV_PROGRESS, base + d, total); });
            all->outputs.insert(all->outputs.end(), r.outputs.begin(), r.outputs.end());
            all->failed += r.failed;
            all->errors.insert(all->errors.end(), r.errors.begin(), r.errors.end());
            base += b.files.size();
        }
        if (!PostMessage(wnd, WM_CONV_DONE, 0, (LPARAM)all)) delete all;
        CoUninitialize();
    }).detach();
}

static void StartJob(const std::vector<std::wstring>& files, const jobs::Job& j, const std::wstring& label) { StartBatches({{files, j}}, label); }

// ZIP simples (sem compressão) para quando o tar.exe não existir; inclui subpastas.
static bool WriteStoredZip(const std::wstring& out, const std::vector<std::wstring>& files, const std::wstring& dir) {
    struct Entry { std::string name; uint32_t crc, size, offset; };
    std::vector<Entry> entries;
    FILE* z = _wfopen(out.c_str(), L"wb");
    if (!z) return false;
    static uint32_t table[256];
    if (!table[1]) for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, z); };
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, z); };
    auto utf8 = [](const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string r(n > 0 ? n - 1 : 0, 0); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &r[0], n, nullptr, nullptr); return r; };
    bool ok = true;
    std::function<void(const std::wstring&, const std::wstring&)> add = [&](const std::wstring& path, const std::wstring& rel) {
        DWORD at = GetFileAttributesW(path.c_str());
        if (at == INVALID_FILE_ATTRIBUTES) return;
        if (at & FILE_ATTRIBUTE_DIRECTORY) {
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) add(path + L"\\" + fd.cFileName, rel + L"/" + fd.cFileName); } while (FindNextFileW(h, &fd));
            FindClose(h);
            return;
        }
        // lê em blocos: o arquivo nunca vai inteiro para a memória (o tamanho e o CRC são gravados depois)
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeHigh || fa.nFileSizeLow > 0xFFFFFFF0u) { ok = false; return; } // sem ZIP64
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { ok = false; return; }
        Entry e{utf8(rel), 0, 0, (uint32_t)ftell(z)};
        put32(0x04034b50); put16(20); put16(0x0800); put16(0); put16(0); put16(0x21); put32(0); put32(0); put32(0);
        put16((uint16_t)e.name.size()); put16(0); fwrite(e.name.data(), 1, e.name.size(), z);
        static char buf[1 << 16]; size_t n;
        uint32_t crc = 0xFFFFFFFFu, size = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            for (size_t i = 0; i < n; ++i) crc = table[(crc ^ (uint8_t)buf[i]) & 255] ^ (crc >> 8);
            fwrite(buf, 1, n, z); size += (uint32_t)n;
        }
        fclose(f);
        e.crc = crc ^ 0xFFFFFFFFu; e.size = size;
        long end = ftell(z);
        fseek(z, (long)e.offset + 14, SEEK_SET); put32(e.crc); put32(e.size); put32(e.size); // completa o cabeçalho
        fseek(z, end, SEEK_SET);
        entries.push_back(e);
    };
    for (auto& f : files) add(f, conv::DirOf(f) == dir ? std::wstring(PathFindFileNameW(f.c_str())) : std::wstring(PathFindFileNameW(f.c_str())));
    uint32_t cd = (uint32_t)ftell(z);
    for (auto& e : entries) {
        put32(0x02014b50); put16(20); put16(20); put16(0x0800); put16(0); put16(0); put16(0x21); put32(e.crc); put32(e.size); put32(e.size);
        put16((uint16_t)e.name.size()); put16(0); put16(0); put16(0); put16(0); put32(0); put32(e.offset); fwrite(e.name.data(), 1, e.name.size(), z);
    }
    uint32_t cdSize = (uint32_t)ftell(z) - cd;
    put32(0x06054b50); put16(0); put16(0); put16((uint16_t)entries.size()); put16((uint16_t)entries.size()); put32(cdSize); put32(cd); put16(0);
    fclose(z);
    return ok && !entries.empty();
}

// Compacta os itens selecionados em um .zip na mesma pasta, usando o tar.exe que vem no Windows 10/11.
static void StartZip(std::vector<std::wstring> files) {
    if (g_converting) { ShowToast(T(L"Aguarde", L"Please wait", L"Espera"), T(L"Já há uma conversão em andamento.", L"A conversion is already running.", L"Ya hay una conversión en curso."), 3000, 0); return; }
    if (files.empty()) return;
    g_converting = true;
    g_toastOutputs.clear();
    g_jobLabel = T(L"Compactando em ZIP", L"Compressing to ZIP", L"Comprimiendo en ZIP");
    g_toastDone = 0; g_toastTotal = (UINT)files.size(); g_toastAction = 0;
    ShowToast(g_jobLabel, T(L"Preparando…", L"Preparing…", L"Preparando…"), 0, 0);
    HWND wnd = g_wnd;
    std::thread([files, wnd] {
        auto* res = new jobs::Result;
        std::wstring dir = conv::DirOf(files[0]);
        std::wstring base;
        if (files.size() == 1) { base = PathFindFileNameW(files[0].c_str()); size_t dot = base.rfind(L'.'); if (dot != std::wstring::npos && dot > 0 && !(GetFileAttributesW(files[0].c_str()) & FILE_ATTRIBUTE_DIRECTORY)) base.resize(dot); }
        else { base = PathFindFileNameW(dir.c_str()); if (base.empty() || base.back() == L':') base = L"Arquivos"; }
        std::wstring out = dir + L"\\" + base + L".zip";
        for (int n = 2; GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) out = dir + L"\\" + base + L" (" + std::to_wstring(n) + L").zip";
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -c -f \"" + out + L"\"";
        for (auto& f : files) {
            std::wstring name = conv::DirOf(f) == dir ? std::wstring(PathFindFileNameW(f.c_str())) : f;
            cmd += L" \"" + name + L"\"";
        }
        STARTUPINFOW si = {sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        DWORD code = 1;
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        if (code != 0) { DeleteFileW(out.c_str()); if (WriteStoredZip(out, files, dir)) code = 0; } // sem tar.exe: ZIP sem compressão
        if (code == 0 && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES) res->outputs.push_back(out);
        else { DeleteFileW(out.c_str()); res->failed = (int)files.size(); }
        if (!PostMessage(wnd, WM_CONV_DONE, 0, (LPARAM)res)) delete res;
    }).detach();
}

// ---------- predefinições (%LOCALAPPDATA%\DesignThumbs\predefinicoes.txt) ----------


// ---------- tipos de arquivo da seleção ----------

enum { kSelImage = 1, kSelPdf = 2, kSelPsd = 4, kSelPalette = 8, kSelFont = 16 };

static UINT SelectionKinds(const std::vector<std::wstring>& files) {
    static const wchar_t* images[] = {L".jpg", L".jpeg", L".jfif", L".png", L".bmp", L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif",
                                      L".avif", L".jxl", L".ico", L".jxr", L".wdp", L".dds"};
    UINT k = 0;
    for (auto& f : files) {
        std::wstring e = conv::ExtOf(f);
        if (e == L".pdf" || e == L".ai") k |= kSelPdf | kSelImage;
        else if (e == L".psd" || e == L".psb") k |= kSelPsd | kSelImage;
        else if (fmt::IsPaletteExt(e)) k |= kSelPalette;
        else if (fmt::IsFontExt(e)) k |= kSelFont;
        else if (fmt::Handles(e) || fmt::InList(e, images, ARRAYSIZE(images))) k |= kSelImage;
    }
    return k;
}

// ---------- menu circular (segurar Alt) ----------

struct Slice { std::wstring label; const wchar_t* icon; std::function<void()> act; };
struct Side { std::wstring name; std::vector<Slice> items; };
static std::vector<Side> g_sides;
static int g_side, g_ringHot = -2;   // -1 = centro (troca de lado), -2 = nada
static BYTE g_ringAlpha;
static float g_ringScale = 1;

static void OpenResizeDialog();
static void OpenPagesDialog();
static void OpenCropEditor();

static void BuildSides() {
    g_sides.clear();
    const std::vector<std::wstring>& files = g_convFiles;
    UINT k = SelectionKinds(files);
    auto job = [](jobs::Action a) { jobs::Job j = BaseJob(); j.action = a; return j; };
    auto run = [](jobs::Job j, std::wstring label) { return [j, label] { StartJob(g_convFiles, j, label); }; };

    { // Converter: formatos (só para imagens, paletas e fontes) e ZIP (qualquer arquivo)
        Side s{T(L"Converter", L"Convert", L"Convertir"), {}};
        if (k & (kSelImage | kSelPalette | kSelFont)) {
            static const ops::Format order[] = {ops::kPng, ops::kJpg, ops::kPdf, ops::kWebp, ops::kTiff, ops::kSvg, ops::kEps};
            bool allVector = !files.empty() && vec::Available();
            for (const auto& file : files) allVector &= vec::IsInput(file);
            for (ops::Format f : order) {
                if ((f == ops::kSvg || f == ops::kEps) && !allVector) continue;
                jobs::Job j = BaseJob(); j.format = f;
                std::wstring name = ops::Name(f);
                const wchar_t* icon = f == ops::kPdf ? L"\uE8A5" : L"\uE91B";
                if (f == ops::kPdf && files.size() > 1) name += L" \u00D7" + std::to_wstring(files.size());
                s.items.push_back({name, icon, run(j, std::wstring(T(L"Convertendo para ", L"Converting to ", L"Convirtiendo a ")) + ops::Name(f))});
            }
        }
        s.items.push_back({L"ZIP", L"\uE7B8", [] { StartZip(g_convFiles); }});
        g_sides.push_back(s);
    }
    if (k & kSelImage) {
        Side s{T(L"Ferramentas", L"Tools", L"Herramientas"), {}};
        s.items.push_back({T(L"Redimensionar", L"Resize", L"Redimensionar"), L"", [] { OpenResizeDialog(); }});
        if (k & ~kSelPdf) s.items.push_back({T(L"Recortar", L"Crop", L"Recortar"), L"", [] { OpenCropEditor(); }});
        jobs::Job j = job(jobs::kEditSame);
        j.edit.rotate = 90; j.suffix = T(L"(girado)", L"(rotated)", L"(girado)");
        s.items.push_back({T(L"Girar 90°", L"Rotate 90°", L"Girar 90°"), L"", run(j, T(L"Girando", L"Rotating", L"Girando"))});
        j = job(jobs::kEditSame); j.edit.flipH = true; j.suffix = T(L"(espelhado)", L"(mirrored)", L"(espejado)");
        s.items.push_back({T(L"Espelhar", L"Mirror", L"Espejar"), L"", run(j, T(L"Espelhando", L"Mirroring", L"Espejando"))});
        j = job(jobs::kEditSame); j.edit.gray = true; j.suffix = T(L"(cinza)", L"(grayscale)", L"(gris)");
        s.items.push_back({T(L"Tons de cinza", L"Grayscale", L"Escala de grises"), L"", run(j, T(L"Convertendo para cinza", L"Converting to grayscale", L"Convirtiendo a grises"))});
        j = job(jobs::kEditSame); j.suffix = T(L"(sem metadados)", L"(no metadata)", L"(sin metadatos)");
        s.items.push_back({T(L"Sem metadados", L"Strip metadata", L"Sin metadatos"), L"", run(j, T(L"Removendo metadados", L"Removing metadata", L"Quitando metadatos"))});
        g_sides.push_back(s);
    }
    if (k & kSelPdf) {
        Side s{L"PDF", {}};
        if (files.size() > 1) {
            jobs::Job j = BaseJob(); j.format = ops::kPdf;
            s.items.push_back({T(L"Juntar", L"Merge", L"Unir"), L"", run(j, T(L"Juntando PDFs", L"Merging PDFs", L"Uniendo PDFs"))});
        }
        s.items.push_back({T(L"Dividir", L"Split", L"Dividir"), L"", run(job(jobs::kPdfSplit), T(L"Dividindo PDF", L"Splitting PDF", L"Dividiendo PDF"))});
        s.items.push_back({T(L"Páginas…", L"Pages…", L"Páginas…"), L"", [] { OpenPagesDialog(); }});
        jobs::Job j = job(jobs::kPdfCompress); j.suffix = T(L"(comprimido)", L"(compressed)", L"(comprimido)");
        s.items.push_back({T(L"Comprimir", L"Compress", L"Comprimir"), L"", run(j, T(L"Comprimindo PDF", L"Compressing PDF", L"Comprimiendo PDF"))});
        g_sides.push_back(s);
    }
    if (k & (kSelPsd | kSelPalette | kSelFont)) {
        Side s{T(L"Extras", L"Extras", L"Extras"), {}};
        if (k & kSelPsd) {
            jobs::Job j = job(jobs::kPsdLayers); j.suffix = T(L"- camadas", L"- layers", L"- capas");
            s.items.push_back({T(L"Camadas", L"Layers", L"Capas"), L"", run(j, T(L"Exportando camadas", L"Exporting layers", L"Exportando capas"))});
        }
        if (k & kSelPalette) {
            jobs::Job j = job(jobs::kPaletteSheet); j.suffix = T(L"(folha)", L"(sheet)", L"(hoja)");
            s.items.push_back({T(L"Folha de cores", L"Color sheet", L"Hoja de colores"), L"", run(j, T(L"Gerando folha de cores", L"Building color sheet", L"Generando hoja de colores"))});
            s.items.push_back({L"HEX / CSS", L"", run(job(jobs::kPaletteText), T(L"Gerando lista de cores", L"Building color list", L"Generando lista de colores"))});
            s.items.push_back({L"ASE ⇄ ACO", L"", run(job(jobs::kPaletteSwap), T(L"Convertendo paleta", L"Converting palette", L"Convirtiendo paleta"))});
        }
        if (k & kSelFont) {
            jobs::Job j = job(jobs::kFontSheet); j.suffix = T(L"(amostra)", L"(specimen)", L"(muestra)");
            s.items.push_back({T(L"Folha da fonte", L"Font specimen", L"Muestra de fuente"), L"", run(j, T(L"Gerando amostra", L"Building specimen", L"Generando muestra"))});
        }
        g_sides.push_back(s);
    }
    if (g_side >= (int)g_sides.size()) g_side = 0;
}

// Geometria do anel (em pixels da janela).
static float RingS() { return 400 * g_ringScale; }
static float RingIn() { return 64 * g_ringScale; }
static float RingOut() { return 180 * g_ringScale; }

static int RingHit(POINT p) {
    float c = RingS() / 2, dx = p.x - c, dy = p.y - c, r = sqrtf(dx * dx + dy * dy);
    if (r < RingIn() - 4 * g_ringScale) return -1;
    if (r > RingOut() + 6 * g_ringScale || g_sides.empty()) return -2;
    size_t n = g_sides[g_side].items.size();
    if (!n) return -2;
    float step = 2 * 3.14159265f / n, a = atan2f(dy, dx) + 3.14159265f / 2 + step / 2; // 0 = item no topo
    while (a < 0) a += 2 * 3.14159265f;
    return (int)(a / step) % (int)n;
}



static void RenderRing() {
    if (!g_altWnd || g_sides.empty()) return;
    UINT S = (UINT)RingS();
    fmt::Canvas cv(S, S); // bitmap com transparência (janela "layered", sem moldura)
    if (!cv.rt) return;
    ID2D1RenderTarget* rt = cv.rt;
    rt->Clear(D2D1::ColorF(0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const ui::Theme& th = g_theme;
    float s = g_ringScale, c = S / 2.0f, rIn = RingIn(), rOut = RingOut();
    D2D1_POINT_2F C = D2D1::Point2F(c, c);
    ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
    ID2D1SolidColorBrush* br = nullptr;
    auto solid = [&](D2D1_COLOR_F col) { if (br) br->Release(); rt->CreateSolidColorBrush(col, &br); return br; };
    bool crispy = th.neon;
    bool classic = th.classic, winTheme = g_cfg.themeMode == ui::kThemeWindows;
    D2D1_COLOR_F orange = ui::Rgba(ui::NeonColor());

    // sombra suave
    if (!th.dark) for (int i = 12; i >= 1; --i) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c + 5 * s), rOut + i * 2.2f * s, rOut + i * 2.2f * s), solid(ui::Rgba(0x1E2232, 0.011f)));
    else for (int i = 5; i >= 1; --i) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c + 3 * s), rOut + i * 3.5f * s, rOut + i * 3.5f * s), solid(ui::Rgba(0x000000, th.dark ? 0.07f : 0.035f)));
    // Vidro líquido calculado pixel a pixel (idêntico em qualquer PC): base translúcida, brilho laranja que segue
    // o mouse, reflexo no alto, borda de luz e granulado fino.
    {
        // Disco esculpido (v9.4.2): Crispy (neon), Minimalista Escuro (sem luzes) e Minimalista Claro (porcelana).
        // Vincos/divisórias entre fatias, centro rebaixado e a fatia ativa em vidro com cantos arredondados.
        size_t cn = g_sides[g_side].items.size();
        float cstep = 2 * 3.14159265f / max<size_t>(1, cn), hotA0 = 0, hotA1 = 0;
        if (g_ringHot >= 0) { hotA0 = -3.14159265f / 2 - cstep / 2 + g_ringHot * cstep; hotA1 = hotA0 + cstep; }
        float well = rIn - 6 * s;   // raio do centro
        UINT nc = ui::NeonColor();
        float nR = (float)(nc >> 16 & 255), nG = (float)(nc >> 8 & 255), nB = (float)(nc & 255);
        struct Style {
            float discA[3], discB[3], discAlpha, rim[3], rimA;
            bool ticks, creases, lines, wellLight;
            float lineA, plate[3], plateShadeA, plateShade[3], wellA[3], wellB[3], wellRim[3], wellRimA;
            float hot[3], hotIn, hotOut, edge[3], edgeA, halo[3], haloA;
        } st;
        if (crispy) st = {{30, 30, 33}, {5, 5, 6}, 0.98f, {140, 140, 150}, 0.6f, false, true, false, false, 0,
                          {20, 20, 22}, 0.55f, {0, 0, 0}, {28, 28, 30}, {2, 2, 3}, {70, 70, 74}, 0.5f,
                          {nR, nG, nB}, 0.95f, 0.10f, {nR + (255 - nR) * 0.3f, nG + (255 - nG) * 0.3f, nB + (255 - nB) * 0.3f}, 0.85f, {nR, nG, nB}, 0.20f};
        else if (classic) // Vizu Classic: Ardósia chapada, divisórias finas, fatia ativa laranja sólida
            st = {{74, 79, 87}, {74, 79, 87}, 1.0f, {92, 100, 114}, 0.0f, false, false, true, false, 1.0f,
                  {35, 39, 46}, 0.0f, {0, 0, 0}, {35, 39, 46}, {35, 39, 46}, {92, 100, 114}, 0.0f,
                  {255, 102, 51}, 1.0f, 1.0f, {0, 0, 0}, 0, {0, 0, 0}, 0};
        else if (g_cfg.themeMode == ui::kThemeDark || (winTheme && th.dark))
            st = {{30, 30, 33}, {5, 5, 6}, 0.98f, {160, 160, 168}, 0.45f, false, true, false, false, 0,
                  {20, 20, 22}, 0.55f, {0, 0, 0}, {28, 28, 30}, {2, 2, 3}, {70, 70, 74}, 0.5f,
                  {74, 79, 87}, 0.95f, 0.45f, {154, 161, 173}, 0.35f, {0, 0, 0}, 0}; // fatia ativa no grafite da Vizu
        else // Minimalista Claro
            st = {{251, 251, 252}, {220, 222, 227}, 1.0f, {255, 255, 255}, 1.0f, false, false, true, false, 0.22f,
                  {236, 238, 241}, 0.06f, {30, 34, 50}, {255, 255, 255}, {227, 229, 234}, {0, 0, 0}, 0.06f,
                  {255, 255, 255}, 1.0f, 0.55f, {0, 0, 0}, 0, {30, 34, 50}, 0.11f};
        if (winTheme) {
            UINT ac = ui::SystemAccent();
            float aR = (float)(ac >> 16 & 255), aG = (float)(ac >> 8 & 255), aB = (float)(ac & 255);
            st.hot[0] = aR; st.hot[1] = aG; st.hot[2] = aB;
            if (th.dark) { st.hotIn = 0.62f; st.hotOut = 0.14f; st.edge[0] = aR; st.edge[1] = aG; st.edge[2] = aB; st.edgeA = 0.45f; }
            else { st.hotIn = 0.34f; st.hotOut = 0.08f; st.halo[0] = aR; st.halo[1] = aG; st.halo[2] = aB; st.haloA = 0.14f; }
        }
        float lineCol[3] = {40, 44, 60};
        if (classic) { lineCol[0] = 92; lineCol[1] = 100; lineCol[2] = 114; }
        auto SculptPixel = [&](float dx, float dy, float r, float rO) -> UINT32 {
            float ang = atan2f(dy, dx);
            float t = max(0.0f, min(1.0f, 0.5f + 0.35f * (dy / rO) + 0.15f * (dx / rO)));
            float R = st.discA[0] + (st.discB[0] - st.discA[0]) * t, G = st.discA[1] + (st.discB[1] - st.discA[1]) * t, B = st.discA[2] + (st.discB[2] - st.discA[2]) * t, A = st.discAlpha;
            auto mix = [&](float cr, float cg, float cb, float a) { a = max(0.0f, min(1.0f, a)); R += (cr - R) * a; G += (cg - G) * a; B += (cb - B) * a; };
            auto mixv = [&](const float* c3, float a) { mix(c3[0], c3[1], c3[2], a); };
            float top = 0.5f - 0.5f * dy / max(r, 1.0f);
            float rim = fabsf(r - (rO - 1.0f * s));
            if (rim < 1.3f * s) mixv(st.rim, (1 - rim / (1.3f * s)) * st.rimA * (0.25f + 0.75f * top));
            float rel0 = fmodf(ang + 3.14159265f / 2 + cstep / 2 + 8 * 3.14159265f, cstep); // posição dentro da fatia
            if (st.ticks) { // régua: 72 marcas; as das divisões são maiores
                float tstep = 2 * 3.14159265f / 72, q = fmodf(rel0, tstep), da = min(q, tstep - q) * r;
                int idx = (int)floorf(rel0 / tstep + 0.5f);
                bool major = idx % max(1, 72 / (int)max<size_t>(1, cn)) == 0 && 72 % max<size_t>(1, cn) == 0;
                float r0 = rO - (major ? 13 : 9) * s, r1 = rO - 5 * s;
                if (r >= r0 && r <= r1 && da < 0.6f * s) mix(255, 255, 255, 0.16f * (1 - da / (0.6f * s)));
                if (g_ringHot >= 0) { // trecho aceso da fatia ativa
                    float a = ang; while (a < hotA0) a += 2 * 3.14159265f; while (a > hotA0 + 2 * 3.14159265f) a -= 2 * 3.14159265f;
                    if (a > hotA0 + 0.03f && a < hotA1 - 0.03f) {
                        float d = fabsf(r - (rO - 7 * s));
                        if (d < 0.9f * s) mix(nR, nG, nB, 0.95f);
                        else if (d < 4 * s) mix(nR, nG, nB, 0.35f * (1 - (d - 0.9f * s) / (3.1f * s)));
                    }
                }
            }
            if (cn > 1 && r > well) {
                float da = min(rel0, cstep - rel0) * r;
                int boundary = (int)floorf((ang + 3.14159265f / 2 + cstep / 2 + 8 * 3.14159265f) / cstep + 0.5f) % (int)cn;
                bool nearHot = g_ringHot >= 0 && (boundary == g_ringHot || boundary == (g_ringHot + 1) % (int)cn);
                if (st.creases) { // vinco afilado
                    float rel = (r - well) / ((rO - well) * 0.55f);
                    if (rel < 1) {
                        float w = (4.0f * s) * (1 - rel) + 0.6f * s;
                        float d = expf(-(da * da) / (w * w)) * (1 - rel * rel) * 0.75f;
                        R *= 1 - d; G *= 1 - d; B *= 1 - d;
                    }
                }
                if (st.lines && !nearHot) { // linha fina que some em direção à borda
                    float r0 = well + 12 * s, r1 = rO - 14 * s;
                    if (r > r0 && r < r1 && da < 0.7f * s) mix(lineCol[0], lineCol[1], lineCol[2], st.lineA * (classic ? 1.0f : 1 - (r - r0) / (r1 - r0)) * (1 - da / (0.7f * s)));
                }
            }
            // centro rebaixado
            if (!classic && r < well + 14 * s && r >= well) {
                float k = (r - well) / (14 * s);
                if (st.plateShadeA > 0) mixv(st.plateShade, st.plateShadeA * (1 - k));
                else { R *= 0.45f + 0.55f * k; G *= 0.45f + 0.55f * k; B *= 0.45f + 0.55f * k; }
            }
            if (r < well) {
                float inner = well - 10 * s;
                if (r >= inner) { R = st.plate[0]; G = st.plate[1]; B = st.plate[2]; }
                else {
                    float k = r / inner, kk = k * k * k;
                    for (int ch = 0; ch < 3; ++ch) { float v = st.wellA[ch] + (st.wellB[ch] - st.wellA[ch]) * kk; (ch == 0 ? R : ch == 1 ? G : B) = v; }
                }
                if (fabsf(r - inner) < 1.0f * s) mixv(st.wellRim, st.wellRimA);
                if (st.wellLight && g_ringHot >= 0 && r > inner - 16 * s) {
                    float am = (hotA0 + hotA1) / 2, cc = cosf(ang - am);
                    if (cc > 0) { float k = powf(cc, 5) * max(0.0f, 1 - fabsf(r - inner + 3 * s) / (13 * s)); mix(nR, nG, nB, k * 0.9f); }
                }
            }
            // fatia ativa
            if (g_ringHot >= 0) {
                float ri = well + 16 * s, ro = rO - (st.ticks ? 18 : 12) * s, gap = 4 * s, rad = 11 * s;
                float a = ang; while (a < hotA0 - 3.14159265f) a += 2 * 3.14159265f; while (a > hotA0 + 3 * 3.14159265f) a -= 2 * 3.14159265f;
                float dA0 = r * sinf(a - hotA0) - gap, dA1 = r * sinf(hotA1 - a) - gap;
                if (cosf(a - (hotA0 + hotA1) / 2) < 0) dA0 = dA1 = -999;
                float ex = min(dA0, dA1), ey = min(r - ri, ro - r), dist;
                if (ex < rad && ey < rad && min(ex, ey) > -999) { float qx = rad - ex, qy = rad - ey; dist = (qx > 0 && qy > 0) ? rad - sqrtf(qx * qx + qy * qy) : min(ex, ey); }
                else dist = min(ex, ey);
                if (dist > 0) {
                    float tt = (r - ri) / (ro - ri);
                    float al = st.hotOut + (st.hotIn - st.hotOut) * powf(max(0.0f, 1 - tt), 2.0f);
                    if (crispy) { R = R * (1 - al) + st.hot[0] * al; G = G * (1 - al) + st.hot[1] * al; B = B * (1 - al) + st.hot[2] * al; R *= 0.92f; G *= 0.92f; B *= 0.92f; }
                    else mixv(st.hot, al);
                    A = max(A, 0.99f);
                    if (st.edgeA > 0 && dist < 1.4f * s) mixv(st.edge, (1 - dist / (1.4f * s)) * st.edgeA);
                } else if (st.haloA > 0 && dist > -16 * s) {
                    float k = 1 + dist / (16 * s); mixv(st.halo, k * k * st.haloA);
                }
            }
            UINT32 hsh = ((UINT32)(dx + 1000) * 73856093u) ^ ((UINT32)(dy + 1000) * 19349663u); hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
            float nz = ((hsh & 255) / 255.0f - 0.5f) * (st.lines ? 2.5f : 5);
            R = max(0.0f, min(255.0f, R + nz)); G = max(0.0f, min(255.0f, G + nz)); B = max(0.0f, min(255.0f, B + nz));
            float cov = max(0.0f, min(1.0f, rO + 0.5f - r)); A *= cov;
            return (UINT32)(A * 255) << 24 | (UINT32)(R * A) << 16 | (UINT32)(G * A) << 8 | (UINT32)(B * A);
        };
        std::vector<UINT32> px((size_t)S * S, 0);
        for (UINT y = 0; y < S; ++y)
            for (UINT x = 0; x < S; ++x) {
                float dx = x + 0.5f - c, dy = y + 0.5f - c, r = sqrtf(dx * dx + dy * dy);
                if (r > rOut + 1) continue;
                px[(size_t)y * S + x] = SculptPixel(dx, dy, r, rOut);
            }
        ID2D1Bitmap* bg = nullptr;
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(S, S), px.data(), S * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bg))) {
            rt->DrawBitmap(bg, D2D1::RectF(0, 0, (float)S, (float)S));
            bg->Release();
        }
    }

    const Side& side = g_sides[g_side];
    size_t n = side.items.size();
    float step = 2 * 3.14159265f / max<size_t>(1, n);
    ui::Painter p(rt, g_dw, th, s);
    bool formats = g_side == 0 && side.name == T(L"Converter", L"Convert", L"Convertir");
    for (size_t i = 0; i < n; ++i) {
        float a0 = -3.14159265f / 2 - step / 2 + i * step, a1 = a0 + step, am = (a0 + a1) / 2;
        bool hot = (int)i == g_ringHot;
        float rm = (rIn + rOut) / 2 + 3 * s, cx = c + rm * cosf(am), cy = c + rm * sinf(am);
        D2D1_COLOR_F col = hot ? (classic ? ui::Rgba(0x000000) : th.dark ? ui::Rgba(0xFFFFFF) : ui::Rgba(0x1D1D1F))
                              : classic ? ui::Rgba(0xFFFFFF) : crispy ? ui::Rgba(0x6C6C73) : th.dark ? ui::Rgba(0x66666C) : ui::Rgba(0xBDBDC3);
        if (hot && crispy) // sombra escura atrás do nome, para separar do brilho
            for (int q = 8; q >= 1; --q) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 2 * s), (26 + q * 1.6f) * s, (14 + q * 1.2f) * s), solid(ui::Rgba(0x000000, 0.035f)));
        float bw = min(rm * step * 0.95f, 96 * s);
        if (formats) { // só o nome do formato
            const std::wstring& lab = side.items[i].label;
            D2D1_RECT_F lr = D2D1::RectF(cx - bw / 2, cy - 11 * s, cx + bw / 2, cy + 11 * s);
            p.TextSpaced(lab, lr, hot ? 15.5f : 13, col, hot ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, 0.1f); // letras espaçadas
        } else {
            p.Icon(side.items[i].icon, D2D1::RectF(cx - 20 * s, cy - 23 * s, cx + 20 * s, cy - 3 * s), 14, col);
            p.Text(side.items[i].label, D2D1::RectF(cx - bw / 2, cy, cx + bw / 2, cy + 18 * s), 11, col, hot ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    // centro: lente de vidro com o nome do lado
    D2D1_ELLIPSE lens = D2D1::Ellipse(C, rIn - 8 * s, rIn - 8 * s);
    std::wstring cnt = std::to_wstring(g_convFiles.size()) + (g_convFiles.size() == 1 ? T(L" arquivo", L" file", L" archivo") : T(L" arquivos", L" files", L" archivos"));
    {
        D2D1_COLOR_F titleCol = crispy ? orange : th.dark ? ui::Rgba(0xFFFFFF) : ui::Rgba(0x1D1D1F), muted = classic ? ui::Rgba(0xBCC2CC) : th.dark ? ui::Rgba(0x8E8E95) : ui::Rgba(0x7A7A82);
        if (g_ringHot == -1) {
            if (crispy) { rt->DrawEllipse(lens, solid(ui::Rgba(ui::NeonColor(), 0.9f)), 1.5f * s); rt->DrawEllipse(lens, solid(ui::Rgba(ui::NeonColor(), 0.25f)), 5 * s); }
            else rt->DrawEllipse(lens, solid(th.dark ? ui::Rgba(0xFFFFFF, 0.35f) : ui::Rgba(0x000000, 0.18f)), 1.2f * s);
        }
        std::wstring up; for (wchar_t ch : cnt) up += (wchar_t)towupper(ch);
        p.Text(up, D2D1::RectF(c - rIn, c - 26 * s, c + rIn, c - 14 * s), 8.5f, muted, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        p.Text(side.name, D2D1::RectF(c - rIn, c - 14 * s, c + rIn, c + 6 * s), 15, titleCol, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (g_sides.size() > 1) {
            auto shortName = [](std::wstring n) { return n.size() > 7 ? n.substr(0, 6) + L"." : n; };
            size_t ns = g_sides.size();
            std::wstring nb = L"\u2039 " + shortName(g_sides[(g_side + ns - 1) % ns].name) + L" \u00B7 " + shortName(g_sides[(g_side + 1) % ns].name) + L" \u203A";
            p.Text(nb, D2D1::RectF(c - rIn, c + 6 * s, c + rIn, c + 18 * s), 8.5f, muted, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            float dw = 6 * s, dl = 14 * s, gapd = 4 * s, total = (ns - 1) * dw + dl + (ns - 1) * gapd, x = c - total / 2;
            for (size_t i = 0; i < ns; ++i) {
                float w2 = (int)i == g_side ? dl : dw;
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, c + 24 * s, x + w2, c + 27 * s), 1.5f * s, 1.5f * s),
                                         solid((int)i == g_side ? (crispy ? orange : classic ? ui::Rgba(0xFF6633) : winTheme ? th.accent : th.dark ? ui::Rgba(0x9AA1AD) : ui::Rgba(0x1D1D1F)) : (th.dark ? ui::Rgba(0xFFFFFF, 0.18f) : ui::Rgba(0x000000, 0.14f))));
                x += w2 + gapd;
            }
        }
    }
    if (br) br->Release();
    fac->Release();
    HBITMAP bmp = nullptr;
    if (FAILED(cv.Finish(&bmp)) || !bmp) return;
    RECT wr; GetWindowRect(g_altWnd, &wr);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT pos = {wr.left, wr.top}, zero = {0, 0};
    SIZE size = {(LONG)S, (LONG)S};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, g_ringAlpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_altWnd, screen, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem); ReleaseDC(nullptr, screen);
    DeleteObject(bmp);
}

static void HideAltPopup() { if (g_altWnd && IsWindowVisible(g_altWnd)) ShowWindow(g_altWnd, SW_HIDE); g_ringHot = -2; }

static void ShowAltPopup() {
    SyncSelectionOrder(g_defView);
    g_convFiles = g_selOrder;
    if (g_convFiles.empty()) return;
    BuildSides();
    if (g_sides.empty()) return;
    POINT pt; GetCursorPos(&pt);
    g_ringScale = GetDpiForWindow(g_defView) / 96.0f;
    int S = (int)RingS();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = max((int)mi.rcWork.left, min((int)mi.rcWork.right - S, (int)pt.x - S / 2));
    int y = max((int)mi.rcWork.top, min((int)mi.rcWork.bottom - S, (int)pt.y - S / 2));
    g_ringHot = -2;
    g_ringAlpha = 60;
    SetWindowPos(g_altWnd, HWND_TOPMOST, x, y, S, S, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RenderRing();
    SetTimer(g_altWnd, 1, 15, nullptr); // aparece suavemente
}

static LRESULT CALLBACK AltProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: { // fora do disco, os cliques passam para o que está atrás
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        return RingHit(p) == -2 ? HTTRANSPARENT : HTCLIENT;
    }
    case WM_TIMER:
        g_ringAlpha = (BYTE)min(255, g_ringAlpha + 65);
        RenderRing();
        if (g_ringAlpha == 255) KillTimer(h, 1);
        return 0;
    case WM_MOUSEMOVE: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = RingHit(p);
        if (hot != g_ringHot) { g_ringHot = hot; RenderRing(); } // só redesenha quando a fatia muda
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_ringHot >= -1 ? IDC_HAND : IDC_ARROW)); return TRUE;
    case WM_MOUSEWHEEL: // roda do mouse: troca de lado
        if (g_sides.size() > 1) {
            int n = (int)g_sides.size();
            g_side = (g_side + (GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : n - 1)) % n;
            g_ringHot = -2;
            RenderRing();
        }
        return 0;
    case WM_MOUSELEAVE:
        g_ringHot = -2;
        if (GetAsyncKeyState(VK_MENU) >= 0) HideAltPopup(); // soltou o Alt e saiu de cima: some
        else RenderRing();
        return 0;
    case WM_LBUTTONUP: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = RingHit(p);
        if (hit == -1 && g_sides.size() > 1) { g_side = (g_side + 1) % (int)g_sides.size(); RenderRing(); return 0; }
        if (hit >= 0 && hit < (int)g_sides[g_side].items.size()) {
            auto act = g_sides[g_side].items[hit].act;
            HideAltPopup();
            act();
        }
        return 0;
    }
    case WM_RBUTTONUP: HideAltPopup(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- diálogos: redimensionar e páginas ----------

static HWND g_dlgWnd, g_dlgEdit;
static int g_dlgMode;                 // 0 redimensionar, 1 páginas
static int g_dlgFormat;               // redimensionar: 0 manter, 1 JPG, 2 PNG, 3 WEBP
static int g_dlgHot;
static std::vector<std::pair<int, D2D1_RECT_F>> g_dlgHits;
static ui::Surface g_dlgSurf;
static HBRUSH g_dlgEditBrush;
static HFONT g_dlgFont;
static const UINT kResizeChoices[] = {25, 50, 75, 1080, 1600, 1920, 2048, 3840}; // até 100: porcentagem; acima: maior lado em px

static void CloseDialog() { if (g_dlgWnd) DestroyWindow(g_dlgWnd); }

static void DialogAction(int id) {
    if (id == 1) { CloseDialog(); return; }                                 // cancelar
    if (id >= 10 && id < 14) { g_dlgFormat = id - 10; InvalidateRect(g_dlgWnd, nullptr, FALSE); return; }
    jobs::Job j = BaseJob();
    if (g_dlgMode == 0 && id >= 20 && id < 20 + (int)ARRAYSIZE(kResizeChoices)) {
        UINT v = kResizeChoices[id - 20];
        if (v <= 100) { j.edit.scalePct = v; j.suffix = L"(" + std::to_wstring(v) + L"%)"; }
        else { j.edit.maxW = j.edit.maxH = v; j.suffix = L"(" + std::to_wstring(v) + L"px)"; }
        static const ops::Format fmts[] = {ops::kPng, ops::kJpg, ops::kPng, ops::kWebp};
        if (g_dlgFormat == 0) j.action = jobs::kEditSame; else j.format = fmts[g_dlgFormat];
        CloseDialog();
        StartJob(g_convFiles, j, T(L"Redimensionando", L"Resizing", L"Redimensionando"));
        return;
    }
    if (g_dlgMode == 1 && id == 2) { // extrair páginas
        wchar_t buf[256] = {};
        GetWindowTextW(g_dlgEdit, buf, 256);
        j.action = jobs::kPdfExtract;
        j.pages = buf;
        j.suffix = std::wstring(L"(p") + buf + L")";
        for (auto& c : j.suffix) if (c == L',' || c == L' ') c = c == L',' ? L'_' : c;
        CloseDialog();
        StartJob(g_convFiles, j, T(L"Extraindo páginas", L"Extracting pages", L"Extrayendo páginas"));
    }
}

static void PaintDialog(HWND h) {
    g_dlgSurf.hwnd = h;
    if (!g_dlgSurf.Begin(g_d2d)) return;
    float s = WndScale(h);
    ID2D1RenderTarget* rt = g_dlgSurf.rt;
    D2D1_SIZE_F sz = rt->GetSize();
    const ui::Theme& th = g_theme;
    rt->Clear(th.tint);
    ui::Painter p(rt, g_dw, th, s);
    g_dlgHits.clear();
    auto hit = [&](int id, D2D1_RECT_F r) { g_dlgHits.push_back({id, r}); return g_dlgHot == id; };
    float x = 24 * s, w = sz.width - 48 * s, y = 22 * s;
    std::wstring title = g_dlgMode == 0 ? T(L"Redimensionar", L"Resize", L"Redimensionar") : T(L"Extrair páginas", L"Extract pages", L"Extraer páginas");
    p.Text(title, D2D1::RectF(x, y, x + w, y + 28 * s), 18, th.text, DWRITE_FONT_WEIGHT_BOLD);
    std::wstring sub = std::to_wstring(g_convFiles.size()) + (g_convFiles.size() == 1 ? T(L" arquivo", L" file", L" archivo") : T(L" arquivos", L" files", L" archivos"));
    p.Text(sub, D2D1::RectF(x, y + 28 * s, x + w, y + 46 * s), 12, th.text2);
    y += 62 * s;
    if (g_dlgMode == 0) {
        float bw = (w - 3 * 8 * s) / 4, bh = 38 * s;
        for (int i = 0; i < (int)ARRAYSIZE(kResizeChoices); ++i) {
            UINT v = kResizeChoices[i];
            D2D1_RECT_F r = D2D1::RectF(x + (i % 4) * (bw + 8 * s), y + (i / 4) * (bh + 8 * s), x + (i % 4) * (bw + 8 * s) + bw, y + (i / 4) * (bh + 8 * s) + bh);
            p.Pill(r, std::to_wstring(v) + (v <= 100 ? L"%" : L" px"), hit(20 + i, r));
        }
        y += 2 * (bh + 8 * s) + 10 * s;
        p.Text(T(L"Formato", L"Format", L"Formato"), D2D1::RectF(x, y, x + 120 * s, y + 32 * s), 13, th.text);
        D2D1_RECT_F seg = D2D1::RectF(x + w - 280 * s, y, x + w, y + 32 * s), parts[4];
        std::wstring l[4] = {T(L"Manter", L"Keep", L"Mantener"), L"JPG", L"PNG", L"WEBP"};
        p.Segmented(seg, l, 4, g_dlgFormat, g_dlgHot >= 10 && g_dlgHot < 14 ? g_dlgHot - 10 : -1, parts);
        for (int i = 0; i < 4; ++i) hit(10 + i, parts[i]);
        y += 48 * s;
        p.Text(T(L"Porcentagem reduz/amplia; px limita o maior lado.", L"Percent scales; px limits the longest side.", L"Porcentaje escala; px limita el lado mayor."),
               D2D1::RectF(x, y, x + w, y + 18 * s), 11, th.text2);
    } else {
        p.Text(T(L"Exemplos: 1-3   ·   2, 5, 9   ·   4-", L"Examples: 1-3   ·   2, 5, 9   ·   4-", L"Ejemplos: 1-3   ·   2, 5, 9   ·   4-"), D2D1::RectF(x, y, x + w, y + 20 * s), 12, th.text2);
        y += 28 * s;
        D2D1_RECT_F box = D2D1::RectF(x, y, x + w - 130 * s, y + 36 * s);
        p.Fill(box, 10 * s, th.dark ? ui::Rgba(0x2C2C2E) : ui::Rgba(0xFFFFFF));
        p.Stroke(box, 10 * s, th.stroke);
        if (g_dlgEdit) SetWindowPos(g_dlgEdit, nullptr, (int)(box.left + 12 * s), (int)(box.top + 8 * s), (int)(box.right - box.left - 24 * s), (int)(20 * s), SWP_NOZORDER | SWP_NOACTIVATE);
        D2D1_RECT_F go = D2D1::RectF(x + w - 120 * s, y, x + w, y + 36 * s);
        p.Pill(go, T(L"Extrair", L"Extract", L"Extraer"), hit(2, go), true);
    }
    D2D1_RECT_F cancel = D2D1::RectF(sz.width - 24 * s - 110 * s, sz.height - 52 * s, sz.width - 24 * s, sz.height - 18 * s);
    p.Pill(cancel, T(L"Cancelar", L"Cancel", L"Cancelar"), hit(1, cancel));
    p.Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 8 * s, th.stroke);
    g_dlgSurf.End();
}

static LRESULT CALLBACK DialogProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        for (auto& e : g_dlgHits) if (ui::Hit(e.second, pt)) return HTCLIENT;
        return pt.y < 70 * WndScale(h) ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = 0;
        for (auto& e : g_dlgHits) if (ui::Hit(e.second, pt)) hot = e.first;
        if (hot != g_dlgHot) { g_dlgHot = hot; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_dlgHot) { g_dlgHot = 0; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, g_dlgHot ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (auto& e : g_dlgHits) if (ui::Hit(e.second, pt)) { DialogAction(e.first); break; }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        COLORREF bg = g_theme.dark ? RGB(0x2C, 0x2C, 0x2E) : RGB(255, 255, 255);
        SetBkColor(dc, bg);
        SetTextColor(dc, g_theme.dark ? RGB(245, 245, 245) : RGB(29, 29, 31));
        if (!g_dlgEditBrush) g_dlgEditBrush = CreateSolidBrush(bg);
        return (LRESULT)g_dlgEditBrush;
    }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) CloseDialog(); return 0;
    case WM_SIZE: g_dlgSurf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintDialog(h); EndPaint(h, &ps); return 0; }
    case WM_DESTROY:
        g_dlgSurf.Release(); g_dlgHits.clear(); g_dlgWnd = g_dlgEdit = nullptr;
        if (g_dlgEditBrush) { DeleteObject(g_dlgEditBrush); g_dlgEditBrush = nullptr; }
        if (g_dlgFont) { DeleteObject(g_dlgFont); g_dlgFont = nullptr; }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenDialog(int mode) {
    CloseDialog();
    g_dlgMode = mode; g_dlgHot = 0; g_dlgFormat = 0;
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    float s = g_defView ? GetDpiForWindow(g_defView) / 96.0f : 1;
    int w = (int)(460 * s), h = (int)((mode == 0 ? 330 : 230) * s);
    int x = max((int)mi.rcWork.left, min((int)mi.rcWork.right - w, (int)pt.x - w / 2));
    int y = max((int)mi.rcWork.top, min((int)mi.rcWork.bottom - h, (int)pt.y - h / 2));
    g_dlgWnd = CreateWindowExW(WS_EX_TOPMOST, L"VizuDialog", L"Vizu", WS_POPUP, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
    ui::EnableGlass(g_dlgWnd, g_theme.dark);
    if (mode == 1) {
        g_dlgEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_dlgWnd, nullptr, g_inst, nullptr);
        g_dlgFont = CreateFontW(-(int)(14 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendMessageW(g_dlgEdit, WM_SETFONT, (WPARAM)g_dlgFont, TRUE);
        SetWindowTextW(g_dlgEdit, L"");
    }
    ShowWindow(g_dlgWnd, SW_SHOW);
    SetForegroundWindow(g_dlgWnd);
    if (g_dlgEdit) { SetFocus(g_dlgEdit); SendMessageW(g_dlgEdit, EM_SETSEL, 0, -1); }
}

static void OpenResizeDialog() { OpenDialog(0); }
static void OpenPagesDialog() { OpenDialog(1); }

// ---------- editor de recorte (uma imagem por vez) ----------

static HWND g_cropWnd;
static ui::Surface g_cropSurf;
static ID2D1Bitmap* g_cropGpu;
static HBITMAP g_cropBmp;
static size_t g_cropIndex;
static std::vector<std::pair<std::wstring, ops::Edit>> g_cropDone;
static D2D1_RECT_F g_cropRect = {0.1f, 0.1f, 0.9f, 0.9f}; // normalizado
static int g_cropAspect;   // 0 livre, 1 1:1, 2 4:5, 3 3:2, 4 16:9, 5 9:16
static int g_cropDrag;     // 0 nada, 1 mover, 2..5 cantos
static POINT g_cropFrom;
static D2D1_RECT_F g_cropStart, g_cropImg;
static int g_cropHot;
static std::vector<std::pair<int, D2D1_RECT_F>> g_cropHits;
static const float kAspects[] = {0, 1, 4.0f / 5, 3.0f / 2, 16.0f / 9, 9.0f / 16};

static void CropApplyAspect(bool fromTopLeft) {
    if (!g_cropAspect || !g_cropBmp) return;
    float iw = g_cropImg.right - g_cropImg.left, ih = g_cropImg.bottom - g_cropImg.top;
    float a = kAspects[g_cropAspect] * ih / iw; // proporção em coordenadas normalizadas
    D2D1_RECT_F& r = g_cropRect;
    float w = r.right - r.left, h = r.bottom - r.top;
    if (w / h > a) w = h * a; else h = w / a;
    if (fromTopLeft) { r.right = r.left + w; r.bottom = r.top + h; }
    else { float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2; r = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }
    if (r.left < 0) { r.right -= r.left; r.left = 0; }
    if (r.top < 0) { r.bottom -= r.top; r.top = 0; }
    if (r.right > 1) { r.left -= r.right - 1; r.right = 1; }
    if (r.bottom > 1) { r.top -= r.bottom - 1; r.bottom = 1; }
}

static bool CropLoad() {
    SafeRelease(g_cropGpu);
    if (g_cropBmp) { DeleteObject(g_cropBmp); g_cropBmp = nullptr; }
    while (g_cropIndex < g_convFiles.size()) {
        bool alpha; int pages;
        if (conv::RenderFile(g_convFiles[g_cropIndex], 2048, 0, &g_cropBmp, &alpha, &pages)) break;
        ++g_cropIndex;
    }
    g_cropRect = D2D1::RectF(0.1f, 0.1f, 0.9f, 0.9f);
    if (g_cropWnd) InvalidateRect(g_cropWnd, nullptr, FALSE);
    return g_cropBmp != nullptr;
}

static void CropNext(bool apply) {
    if (apply) {
        ops::Edit e;
        e.crop = true;
        e.cx0 = max(0.0f, g_cropRect.left); e.cy0 = max(0.0f, g_cropRect.top); e.cx1 = min(1.0f, g_cropRect.right); e.cy1 = min(1.0f, g_cropRect.bottom);
        g_cropDone.push_back({g_convFiles[g_cropIndex], e});
    }
    ++g_cropIndex;
    if (g_cropIndex < g_convFiles.size() && CropLoad()) { CropApplyAspect(false); return; }
    // terminou: executa os recortes, cada arquivo com o seu
    std::vector<Batch> batches;
    for (auto& d : g_cropDone) {
        jobs::Job j = BaseJob();
        j.action = jobs::kEditSame; j.edit = d.second; j.suffix = T(L"(recorte)", L"(cropped)", L"(recorte)");
        batches.push_back({{d.first}, j});
    }
    DestroyWindow(g_cropWnd);
    if (!batches.empty()) StartBatches(batches, T(L"Recortando", L"Cropping", L"Recortando"));
}

static void PaintCrop(HWND h) {
    g_cropSurf.hwnd = h;
    if (!g_cropSurf.Begin(g_d2d)) return;
    float s = WndScale(h);
    ID2D1RenderTarget* rt = g_cropSurf.rt;
    D2D1_SIZE_F sz = rt->GetSize();
    const ui::Theme& th = g_theme;
    rt->Clear(th.tint);
    ui::Painter p(rt, g_dw, th, s);
    g_cropHits.clear();
    auto hit = [&](int id, D2D1_RECT_F r) { g_cropHits.push_back({id, r}); return g_cropHot == id; };
    std::wstring title = std::wstring(T(L"Recortar", L"Crop", L"Recortar")) + L"  ·  " + PathFindFileNameW(g_convFiles[min(g_cropIndex, g_convFiles.size() - 1)].c_str());
    p.Text(title, D2D1::RectF(20 * s, 12 * s, sz.width - 140 * s, 40 * s), 14, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    p.Text(std::to_wstring(g_cropIndex + 1) + L" / " + std::to_wstring(g_convFiles.size()), D2D1::RectF(sz.width - 140 * s, 12 * s, sz.width - 20 * s, 40 * s), 12, th.text2,
           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (g_cropBmp && !g_cropGpu) {
        BYTE* px; UINT w, hh;
        if (ops::Pixels(g_cropBmp, &px, &w, &hh))
            rt->CreateBitmap(D2D1::SizeU(w, hh), px, w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &g_cropGpu);
    }
    D2D1_RECT_F area = D2D1::RectF(20 * s, 52 * s, sz.width - 20 * s, sz.height - 70 * s);
    if (g_cropGpu) {
        D2D1_SIZE_F bs = g_cropGpu->GetSize();
        float k = min((area.right - area.left) / bs.width, (area.bottom - area.top) / bs.height);
        float w = bs.width * k, hh = bs.height * k, cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
        g_cropImg = D2D1::RectF(cx - w / 2, cy - hh / 2, cx + w / 2, cy + hh / 2);
        rt->DrawBitmap(g_cropGpu, g_cropImg, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        D2D1_RECT_F c = D2D1::RectF(g_cropImg.left + g_cropRect.left * w, g_cropImg.top + g_cropRect.top * hh, g_cropImg.left + g_cropRect.right * w, g_cropImg.top + g_cropRect.bottom * hh);
        ID2D1SolidColorBrush* dim = nullptr; rt->CreateSolidColorBrush(ui::Rgba(0x000000, 0.55f), &dim);
        rt->FillRectangle(D2D1::RectF(g_cropImg.left, g_cropImg.top, g_cropImg.right, c.top), dim);
        rt->FillRectangle(D2D1::RectF(g_cropImg.left, c.bottom, g_cropImg.right, g_cropImg.bottom), dim);
        rt->FillRectangle(D2D1::RectF(g_cropImg.left, c.top, c.left, c.bottom), dim);
        rt->FillRectangle(D2D1::RectF(c.right, c.top, g_cropImg.right, c.bottom), dim);
        dim->Release();
        ID2D1SolidColorBrush* white = nullptr; rt->CreateSolidColorBrush(ui::Rgba(0xFFFFFF, 0.95f), &white);
        rt->DrawRectangle(c, white, 1.5f * s);
        white->SetOpacity(0.35f); // regra dos terços
        for (int i = 1; i < 3; ++i) {
            rt->DrawLine(D2D1::Point2F(c.left + (c.right - c.left) * i / 3, c.top), D2D1::Point2F(c.left + (c.right - c.left) * i / 3, c.bottom), white, 1);
            rt->DrawLine(D2D1::Point2F(c.left, c.top + (c.bottom - c.top) * i / 3), D2D1::Point2F(c.right, c.top + (c.bottom - c.top) * i / 3), white, 1);
        }
        white->SetOpacity(1);
        float hs = 7 * s;
        for (D2D1_POINT_2F q : {D2D1::Point2F(c.left, c.top), D2D1::Point2F(c.right, c.top), D2D1::Point2F(c.left, c.bottom), D2D1::Point2F(c.right, c.bottom)})
            rt->FillEllipse(D2D1::Ellipse(q, hs, hs), white);
        white->Release();
    }
    float by = sz.height - 52 * s;
    D2D1_RECT_F seg = D2D1::RectF(20 * s, by, 20 * s + 380 * s, by + 34 * s), parts[6];
    std::wstring l[6] = {T(L"Livre", L"Free", L"Libre"), L"1:1", L"4:5", L"3:2", L"16:9", L"9:16"};
    p.Segmented(seg, l, 6, g_cropAspect, g_cropHot >= 10 && g_cropHot < 16 ? g_cropHot - 10 : -1, parts);
    for (int i = 0; i < 6; ++i) hit(10 + i, parts[i]);
    D2D1_RECT_F ok = D2D1::RectF(sz.width - 20 * s - 120 * s, by, sz.width - 20 * s, by + 34 * s);
    D2D1_RECT_F skip = D2D1::RectF(ok.left - 10 * s - 100 * s, by, ok.left - 10 * s, by + 34 * s);
    p.Pill(ok, T(L"Aplicar", L"Apply", L"Aplicar"), hit(2, ok), true);
    p.Pill(skip, T(L"Pular", L"Skip", L"Omitir"), hit(3, skip));
    p.Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 8 * s, th.stroke);
    if (!g_cropSurf.End()) SafeRelease(g_cropGpu);
}

static int CropGrab(POINT pt) {
    float w = g_cropImg.right - g_cropImg.left, h = g_cropImg.bottom - g_cropImg.top, tol = 14;
    float l = g_cropImg.left + g_cropRect.left * w, t = g_cropImg.top + g_cropRect.top * h, r = g_cropImg.left + g_cropRect.right * w, b = g_cropImg.top + g_cropRect.bottom * h;
    auto near_ = [&](float x, float y) { return fabsf(pt.x - x) < tol && fabsf(pt.y - y) < tol; };
    if (near_(l, t)) return 2; if (near_(r, t)) return 3; if (near_(l, b)) return 4; if (near_(r, b)) return 5;
    if (pt.x > l && pt.x < r && pt.y > t && pt.y < b) return 1;
    return 0;
}

static LRESULT CALLBACK CropProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        return pt.y < 46 * WndScale(h) ? HTCAPTION : HTCLIENT;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (auto& e : g_cropHits) if (ui::Hit(e.second, pt)) return 0;
        g_cropDrag = CropGrab(pt);
        g_cropFrom = pt; g_cropStart = g_cropRect;
        if (g_cropDrag) SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_cropDrag) {
            float w = g_cropImg.right - g_cropImg.left, hh = g_cropImg.bottom - g_cropImg.top;
            float dx = (pt.x - g_cropFrom.x) / w, dy = (pt.y - g_cropFrom.y) / hh;
            D2D1_RECT_F r = g_cropStart;
            if (g_cropDrag == 1) {
                float cw = r.right - r.left, ch = r.bottom - r.top;
                r.left = max(0.0f, min(1 - cw, r.left + dx)); r.top = max(0.0f, min(1 - ch, r.top + dy));
                r.right = r.left + cw; r.bottom = r.top + ch;
            } else {
                if (g_cropDrag == 2 || g_cropDrag == 4) r.left = max(0.0f, min(r.right - 0.02f, r.left + dx));
                if (g_cropDrag == 3 || g_cropDrag == 5) r.right = min(1.0f, max(r.left + 0.02f, r.right + dx));
                if (g_cropDrag == 2 || g_cropDrag == 3) r.top = max(0.0f, min(r.bottom - 0.02f, r.top + dy));
                if (g_cropDrag == 4 || g_cropDrag == 5) r.bottom = min(1.0f, max(r.top + 0.02f, r.bottom + dy));
                if (g_cropAspect) { // mantém a proporção a partir do canto oposto
                    float a = kAspects[g_cropAspect] * hh / w, cw = r.right - r.left, ch = cw / a;
                    if (g_cropDrag == 2 || g_cropDrag == 3) r.top = r.bottom - ch; else r.bottom = r.top + ch;
                    if (r.top < 0 || r.bottom > 1) { r = g_cropRect; }
                }
            }
            g_cropRect = r;
            InvalidateRect(h, nullptr, FALSE);
        } else {
            int hot = 0;
            for (auto& e : g_cropHits) if (ui::Hit(e.second, pt)) hot = e.first;
            if (hot != g_cropHot) { g_cropHot = hot; InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            int g = g_cropDrag ? g_cropDrag : CropGrab(pt);
            SetCursor(LoadCursor(nullptr, g_cropHot ? IDC_HAND : g == 1 ? IDC_SIZEALL : g == 2 || g == 5 ? IDC_SIZENWSE : g ? IDC_SIZENESW : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_cropDrag) { g_cropDrag = 0; ReleaseCapture(); return 0; }
        for (auto& e : g_cropHits)
            if (ui::Hit(e.second, pt)) {
                if (e.first >= 10 && e.first < 16) { g_cropAspect = e.first - 10; CropApplyAspect(false); InvalidateRect(h, nullptr, FALSE); }
                else if (e.first == 2) CropNext(true);
                else if (e.first == 3) CropNext(false);
                break;
            }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (wp == VK_RETURN) CropNext(true);
        return 0;
    case WM_SIZE: g_cropSurf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintCrop(h); EndPaint(h, &ps); return 0; }
    case WM_DESTROY:
        g_cropSurf.Release(); SafeRelease(g_cropGpu);
        if (g_cropBmp) { DeleteObject(g_cropBmp); g_cropBmp = nullptr; }
        g_cropWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenCropEditor() {
    if (g_cropWnd) DestroyWindow(g_cropWnd);
    g_cropIndex = 0; g_cropDone.clear(); g_cropAspect = 0; g_cropDrag = 0; g_cropHot = 0;
    if (!CropLoad()) return;
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int w = (int)((mi.rcWork.right - mi.rcWork.left) * 0.7), h = (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.8);
    g_cropWnd = CreateWindowExW(WS_EX_APPWINDOW, L"VizuCrop", T(L"Vizu — Recortar", L"Vizu — Crop", L"Vizu — Recortar"), WS_POPUP,
                                mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h,
                                nullptr, nullptr, g_inst, nullptr);
    ui::EnableGlass(g_cropWnd, g_theme.dark);
    ShowWindow(g_cropWnd, SW_SHOW);
    SetForegroundWindow(g_cropWnd);
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
    case WM_LBUTTONUP:
        if (g_converting) return 0;
        ShowWindow(h, SW_HIDE);
        if (g_toastAction == 1) { // baixa e roda o instalador (ele fecha e reabre o Vizu)
            g_toastAction = 2;
            g_toastDone = 0; g_toastTotal = 0;
            ShowToast(T(L"Baixando atualização…", L"Downloading update…", L"Descargando actualización…"), g_release.tag, 0, 0);
            std::wstring url = g_release.url; std::string sha = g_release.sha256;
            HWND wnd = g_wnd;
            std::thread([url, sha, wnd] {
                std::wstring path;
                auto* p = new std::wstring(upd::Download(url, &path, sha) ? path : L"");
                if (!PostMessage(wnd, WM_UPDATE_READY, 0, (LPARAM)p)) delete p;
            }).detach();
        } else if (g_toastAction == 0) RevealOutputs();
        else if (g_toastAction == 3) ShellExecuteW(nullptr, L"open", g_toastLog.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
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

#include "Clock.inc.h"
#include "OcrUi.inc.h"
#include "Agenda.inc.h"
#include "Hearts.inc.h"

enum { kTogSpace = 1, kTogAlt, kDelayMinus, kDelayPlus, kDpi72, kDpi300, kPdfSmall, kPdfRaw, kQualMinus, kQualPlus, kTogAuto, kBtnFolder, kBtnQuit, kBtnClose,
       kTogPicker, kBtnFolders, kLangPt, kLangEn, kLangEs, kTogUpdate, kBtnUpdate, kTheme0, kTheme1, kTheme2, kTheme3, kWebpMinus, kWebpPlus, kTogTray, kTogTempDel, kNeon0, kNeon1, kNeon2, kNeon3, kNeon4, kNeon5, kNeonRgb,
       kClockOn, kClockFont, kPomoT0, kPomoT1, kPomoT2, kPomoC0, kPomoC1, kPomoC2, kPomoC3, kTheftHist, kUiFont, kOcrOn, kOcrKeep, kOcrJoin, kOcrLang, kClk100, kClk90, kClk70, kClockDock, kSte, kTheme4, kTheftOn, kFocoOn, kAgendaOn, kVecText, kVecCurves, kSysToggle, kGmailOff };
static const UINT kNeonPresets[6] = {0xFF6633, 0xFF2D95, 0x00E5FF, 0xB6FF3B, 0xA855F7, 0xFFD400};


static std::vector<std::wstring> FolderList();
static std::wstring FolderListPath(bool create);
static void CheckForUpdates(bool manual);

static const wchar_t* kAbout =
    L"O Vizu ensina o Explorer a entender arquivos de design e fotografia: miniaturas e painel de visualização (Alt+P) "
    L"para Photoshop, Illustrator, InDesign, PDF, EPS, SVG, CorelDRAW, Affinity, XD, Sketch, Krita, Procreate, fontes, paletas e RAW de câmeras.\n\n"
    L"Preview com Espaço: selecione um arquivo e aperte Espaço. Setas trocam de arquivo, a roda do mouse dá zoom e arrastar move. "
    L"Z mostra em 100%, 0 ajusta, PgUp/PgDn troca páginas, Enter abre no programa e Ctrl+C copia a imagem. "
    L"Embaixo aparecem tamanho, DPI, cor, câmera e as fontes usadas (as que faltam ficam em vermelho).\n\n"
    L"Menu circular: selecione arquivos e segure Alt. Converta para PNG, JPG, PDF, WEBP ou TIFF, ou compacte em ZIP; use ferramentas "
    L"(redimensionar, recortar, girar, cinza, sem metadados), PDF (juntar na ordem dos cliques, dividir, extrair páginas, comprimir), "
    L"extras (camadas do PSD, folha de cores, HEX/CSS, ASE↔ACO, amostra de fonte). Gire a roda do mouse ou clique no centro para trocar de lado.\n\n"
    L"Conta-gotas: no preview, passe o mouse para ver o HEX da cor e clique para copiar (ótimo com paletas .ase/.aco).\n\n"
    L"Novo › Pastas Padrão Vizu cria as pastas da lista, cada uma com _Morto (edite em Outros › Pastas Padrão Vizu).\n\n"
    L"Leve: não escuta o teclado e só trabalha quando você pede.";

static const wchar_t* kAboutEn =
    L"Vizu teaches File Explorer to understand design and photo files: thumbnails and the preview pane (Alt+P) "
    L"for Photoshop, Illustrator, InDesign, PDF, EPS, SVG, CorelDRAW, Affinity, XD, Sketch, Krita, Procreate, fonts, palettes and camera RAW.\n\n"
    L"Space preview: select a file and press Space. Arrows switch files, the mouse wheel zooms and dragging pans. "
    L"Z shows 100%, 0 fits, PgUp/PgDn change pages, Enter opens the file and Ctrl+C copies the image. "
    L"The bottom bar shows size, DPI, color, camera and the fonts used (missing ones in red).\n\n"
    L"Radial menu: select files and hold Alt. Convert to PNG, JPG, PDF, WEBP or TIFF, or compress to ZIP; use tools (resize, crop, rotate, grayscale, strip metadata), "
    L"PDF (merge in click order, split, extract pages, compress), extras (PSD layers, color sheet, HEX/CSS, ASE↔ACO, font specimen). "
    L"Scroll the mouse wheel or click the center to flip sides.\n\n"
    L"Color picker: in the preview, hover to see the HEX color and click to copy it (great with .ase/.aco palettes).\n\n"
    L"New › Vizu folder set creates the folders on your list, each with _Morto (edit it in Other › Vizu folder set).\n\n"
    L"Lightweight: it doesn't listen to the keyboard and only works when you ask.";

static const wchar_t* kAboutEs =
    L"Vizu enseña al Explorador a entender archivos de diseño y fotografía: miniaturas y panel de vista previa (Alt+P) "
    L"para Photoshop, Illustrator, InDesign, PDF, EPS, SVG, CorelDRAW, Affinity, XD, Sketch, Krita, Procreate, fuentes, paletas y RAW de cámaras.\n\n"
    L"Vista previa con Espacio: selecciona un archivo y pulsa Espacio. Las flechas cambian de archivo, la rueda del ratón hace zoom y arrastrar mueve. "
    L"Z muestra al 100%, 0 ajusta, RePág/AvPág cambian de página, Enter abre el archivo y Ctrl+C copia la imagen. "
    L"Abajo aparecen tamaño, DPI, color, cámara y las fuentes usadas (las que faltan en rojo).\n\n"
    L"Menú circular: selecciona archivos y mantén Alt. Convierte a PNG, JPG, PDF, WEBP o TIFF, o comprime en ZIP; usa herramientas (redimensionar, recortar, girar, grises, sin metadatos), "
    L"PDF (unir en el orden de clic, dividir, extraer páginas, comprimir), extras (capas de PSD, hoja de colores, HEX/CSS, ASE↔ACO, muestra de fuente). "
    L"Gira la rueda del ratón o haz clic al centro para cambiar de lado.\n\n"
    L"Cuentagotas: en la vista previa, pasa el ratón para ver el color HEX y haz clic para copiarlo (ideal con paletas .ase/.aco).\n\n"
    L"Nuevo › Carpetas estándar Vizu crea las carpetas de tu lista, cada una con _Morto (edítala en Otros › Carpetas estándar Vizu).\n\n"
    L"Ligero: no escucha el teclado y solo trabaja cuando lo pides.";

static std::vector<std::pair<int, D2D1_RECT_F>> g_setHits;
static int g_setHot;
static float g_setScroll, g_setContentH;
static bool g_sysOpen = false; // SISTEMA começa recolhido

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
    case kVecText: g_cfg.vectorTextPaths = false; break;
    case kVecCurves: g_cfg.vectorTextPaths = true; break;
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
    case kTogPicker: g_cfg.picker = !g_cfg.picker; break;
    case kNeon0: case kNeon1: case kNeon2: case kNeon3: case kNeon4: case kNeon5:
        g_cfg.neon = kNeonPresets[id - kNeon0]; ui::NeonColor() = g_cfg.neon; SaveConfig(); ApplyTheme(); return;
    case kNeonRgb: {
        static COLORREF custom[16];
        for (int i = 0; i < 6; ++i) custom[i] = RGB(kNeonPresets[i] >> 16, kNeonPresets[i] >> 8 & 255, kNeonPresets[i] & 255);
        CHOOSECOLORW cc = {sizeof(cc)};
        cc.hwndOwner = g_settingsWnd; cc.lpCustColors = custom; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        cc.rgbResult = RGB(g_cfg.neon >> 16, g_cfg.neon >> 8 & 255, g_cfg.neon & 255);
        if (ChooseColorW(&cc)) {
            g_cfg.neon = GetRValue(cc.rgbResult) << 16 | GetGValue(cc.rgbResult) << 8 | GetBValue(cc.rgbResult);
            ui::NeonColor() = g_cfg.neon; SaveConfig(); ApplyTheme();
        }
        return;
    }
    case kClockOn: ClockEnable(!g_clock.on); break;
    case kClockFont: if (PickFont(g_settingsWnd, &g_clock.font, &g_clock.fontPx)) { ClockSave(); ClockRender(); } break;
    case kPomoT0: case kPomoT1: case kPomoT2: g_clock.pomoTimes = id - kPomoT0; ClockSave(); break;
    case kPomoC0: case kPomoC1: case kPomoC2: case kPomoC3: g_clock.pomoColors = id - kPomoC0; ClockSave(); ClockRender(); break;
    case kTheftHist: ShowTheftHistory(); return;
    case kClk100: case kClk90: case kClk70: { static const int pc[3] = {100, 90, 70}; g_clock.scalePct = pc[id - kClk100]; WriteDword(L"ClockScale", g_clock.scalePct); ClockRender(); break; }
    case kClockDock: ClockDock(!g_clock.docked); break;
    case kSte: StartHearts(g_settingsWnd); return;
    case kOcrOn:
        g_clock.ocr = !g_clock.ocr; WriteDword(L"OcrOn", g_clock.ocr);
        if (g_clock.ocr) RegisterHotKey(g_wnd, kHotOcr, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T'); else UnregisterHotKey(g_wnd, kHotOcr);
        ClockRender(); break;
    case kOcrKeep: case kOcrJoin: WriteDword(L"OcrJoin", id == kOcrJoin); break;
    case kOcrLang: ShellExecuteW(nullptr, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL); return;
    case kUiFont: {
        std::wstring f = g_uiFont.empty() ? DefaultUiFont() : g_uiFont;
        if (PickFont(g_settingsWnd, &f, nullptr)) { g_uiFont = f == DefaultUiFont() ? L"" : f; WriteStr(L"UiFont", g_uiFont); ApplyTheme(); }
        return;
    }
    case kTogTempDel: g_cfg.tempPermanent = !g_cfg.tempPermanent; break;
    case kTheftOn: if (g_clock.theft) ToggleTheft(); g_clock.theftOn = !g_clock.theftOn; WriteDword(L"ClockTheft", g_clock.theftOn); ClockRender(); break;
    case kFocoOn: if (g_clock.phase) PomoStop(); g_clock.pomoOn = !g_clock.pomoOn; WriteDword(L"ClockPomo", g_clock.pomoOn); ClockRender(); break;
    case kAgendaOn: g_clock.agendaOn = !g_clock.agendaOn; WriteDword(L"ClockAgenda", g_clock.agendaOn); ClockRender(); break;
    case kTheme0: case kTheme1: case kTheme2: case kTheme3: case kTheme4:
        g_cfg.themeMode = id == kTheme4 ? ui::kThemeClassic : id - kTheme0; SaveConfig(); ApplyTheme();
        if (g_settingsWnd) ui::EnableGlass(g_settingsWnd, g_theme.dark);
        return;
    case kWebpMinus: g_cfg.webpQuality = max(30ul, (unsigned long)g_cfg.webpQuality - 5); break;
    case kWebpPlus: g_cfg.webpQuality = min(100ul, (unsigned long)g_cfg.webpQuality + 5); break;
    case kTogTray: g_cfg.tray = !g_cfg.tray; SaveConfig(); UpdateTrayIcon(); return;
    case kTogUpdate: g_cfg.autoUpdate = !g_cfg.autoUpdate; break;
    case kBtnUpdate: CheckForUpdates(true); return;
    case kBtnFolders: ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + FolderListPath(true) + L"\"").c_str(), nullptr, SW_SHOWNORMAL); return;
    case kLangPt: case kLangEn: case kLangEs: {
        DWORD v = (DWORD)(id - kLangPt + 1);
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\DesignThumbs", L"Language", REG_DWORD, &v, sizeof(v));
        i18n::Load();
        return;
    }
    case kSysToggle: g_sysOpen = !g_sysOpen; return;
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
    ui::Brand(rt, th, 28 * s, 26 * s, 24 * s);
    p.Text(T(L"Configurações", L"Settings", L"Configuración"), D2D1::RectF(28 * s, 52 * s, sz.width - 80 * s, 74 * s), 13, th.text2);
    D2D1_RECT_F close = D2D1::RectF(sz.width - 52 * s, 22 * s, sz.width - 22 * s, 52 * s);
    p.CircleButton(close, L"", hit(kBtnClose, close));

    // conteúdo rolável
    float top = 92 * s;
    D2D1_RECT_F view = D2D1::RectF(0, top, sz.width, sz.height);
    rt->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    float x = 20 * s, w = sz.width - 40 * s, y = top - g_setScroll;
    auto section = [&](const wchar_t* title) {
        p.Text(title, D2D1::RectF(x + 10 * s, y, x + w, y + 22 * s), 11.5f, th.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        y += 26 * s;
    };

    // Linhas de um cartão: cada uma desenha a si mesma; "sub" = subitem (abre abaixo da opção, como um dropdown).
    struct SRow { float h; int level; std::function<void(D2D1_RECT_F)> draw; };
    D2D1_COLOR_F subBg = th.classic ? ui::Rgba(0x23272E) : th.neon ? ui::Rgba(ui::NeonColor(), 0.06f) : th.dark ? ui::Rgba(0xFFFFFF, 0.04f) : ui::Rgba(0x000000, 0.035f);
    auto drawCard = [&](std::vector<SRow>& rows) {
        float hsum = 0;
        for (auto& r : rows) hsum += r.h;
        D2D1_RECT_F c = D2D1::RectF(x, y, x + w, y + hsum + 8 * s);
        p.Glass(c, 16 * s);
        float yy = c.top + 4 * s;
        for (size_t i = 0; i < rows.size(); ++i) { // fundo dos subitens, um bloco por sequência
            if (rows[i].level && (i == 0 || !rows[i - 1].level)) {
                float y0 = yy, y1 = yy;
                for (size_t j = i; j < rows.size() && rows[j].level; ++j) y1 += rows[j].h;
                p.Fill(D2D1::RectF(c.left + 10 * s, y0 + 2 * s, c.right - 10 * s, y1 - 2 * s), 12 * s, subBg);
            }
            yy += rows[i].h;
        }
        yy = c.top + 4 * s;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i && rows[i].level == rows[i - 1].level && rows[i].level < 2)
                p.Fill(D2D1::RectF(c.left + (rows[i].level ? 24 : 16) * s, yy, c.right - (rows[i].level ? 24 : 16) * s, yy + 1 * s), 0, th.stroke);
            float ind = rows[i].level == 0 ? 0 : rows[i].level == 1 ? 10 * s : 28 * s;
            if (rows[i].level == 2) p.Fill(D2D1::RectF(c.left + 30 * s, yy + 6 * s, c.left + 32 * s, yy + rows[i].h - 6 * s), 1 * s, th.stroke); // fio do terceiro nível
            rows[i].draw(D2D1::RectF(c.left + ind, yy, c.right - (rows[i].level ? 10 * s : 0), yy + rows[i].h));
            yy += rows[i].h;
        }
        y = c.bottom + 22 * s;
    };
    auto rlabel = [&](D2D1_RECT_F r, const wchar_t* t, const wchar_t* sub) {
        if (sub && *sub) {
            p.Text(t, D2D1::RectF(r.left + 16 * s, r.top + (r.bottom - r.top) / 2 - 18 * s, r.right - 200 * s, r.top + (r.bottom - r.top) / 2 + 2 * s), 13, th.text);
            p.Text(sub, D2D1::RectF(r.left + 16 * s, r.top + (r.bottom - r.top) / 2 + 1 * s, r.right - 200 * s, r.top + (r.bottom - r.top) / 2 + 18 * s), 11, th.text2);
        } else p.Text(t, D2D1::RectF(r.left + 16 * s, r.top, r.right - 200 * s, r.bottom), 13, th.text);
    };
    auto rbox = [&](D2D1_RECT_F r, float bw, float bh = 30) { float cy = (r.top + r.bottom) / 2; return D2D1::RectF(r.right - 16 * s - bw, cy - bh * s / 2, r.right - 16 * s, cy + bh * s / 2); };
    auto rtoggle = [&](int id, D2D1_RECT_F r, bool on) { D2D1_RECT_F b = rbox(r, 46 * s, 26); p.Toggle(b, on, hit(id, b)); };
    auto rstep = [&](D2D1_RECT_F r, int idMinus, int idPlus, const std::wstring& v) {
        D2D1_RECT_F b = rbox(r, 150 * s), mi, pl;
        p.Stepper(b, v, g_setHot == idMinus ? 0 : g_setHot == idPlus ? 1 : -1, &mi, &pl);
        hit(idMinus, mi); hit(idPlus, pl);
    };
    auto rseg = [&](D2D1_RECT_F r, float bw, std::vector<std::wstring> l, int sel, int id0) {
        D2D1_RECT_F b = rbox(r, bw), parts[6];
        int hot = g_setHot >= id0 && g_setHot < id0 + (int)l.size() ? g_setHot - id0 : -1;
        p.Segmented(b, l.data(), (int)l.size(), sel, hot, parts);
        for (size_t i = 0; i < l.size(); ++i) hit(id0 + (int)i, parts[i]);
    };
    auto rpill = [&](D2D1_RECT_F r, int id, const std::wstring& t, float bw) { D2D1_RECT_F b = rbox(r, bw); p.Pill(b, t, hit(id, b)); };
    const float RH = 50 * s, SH = 46 * s;
    std::vector<SRow> rows;

    // VER
    section(T(L"VER", L"VIEW", L"VER"));
    rows = {
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ver com a barra de espaço", L"View with the Space bar", L"Ver con la barra espaciadora"), T(L"Selecione um arquivo e aperte Espaço", L"Select a file and press Space", L"Selecciona un archivo y pulsa Espacio")); rtoggle(kTogSpace, r, g_cfg.space); }},
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Conta-gotas", L"Color picker", L"Cuentagotas"), T(L"HEX, CMYK e PANTONE aproximado; clique copia o HEX", L"HEX, CMYK and approximate PANTONE; click copies the HEX", L"HEX, CMYK y PANTONE aproximado; clic copia el HEX")); rtoggle(kTogPicker, r, g_cfg.picker); }},
    };
    drawCard(rows);

    if (!g_lite) { // Lite: só VER, SISTEMA e TEMAS
    // CONVERTER
    section(T(L"CONVERTER", L"CONVERT", L"CONVERTIR"));
    rows = {
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Menu circular ao segurar Alt", L"Radial menu when holding Alt", L"Menú circular al mantener Alt"), T(L"Roda do mouse ou clique no centro troca de lado", L"Mouse wheel or center click flips sides", L"Rueda del ratón o clic al centro cambia de lado")); rtoggle(kTogAlt, r, g_cfg.alt); }},
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Tempo segurando", L"Hold time", L"Tiempo manteniendo"), nullptr); rstep(r, kDelayMinus, kDelayPlus, std::to_wstring(g_cfg.altDelay) + L" ms"); }},
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Resolução", L"Resolution", L"Resolución"), nullptr); rseg(r, 180 * s, {L"72 dpi", L"300 dpi"}, g_cfg.dpi300 ? 1 : 0, kDpi72); }},
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, L"PDF", nullptr); rseg(r, 240 * s, {T(L"Compactado", L"Compressed", L"Comprimido"), T(L"Sem compressão", L"Uncompressed", L"Sin compresión")}, g_cfg.pdfRaw ? 1 : 0, kPdfSmall); }},
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Qualidade JPG e PDF", L"JPG and PDF quality", L"Calidad JPG y PDF"), nullptr); rstep(r, kQualMinus, kQualPlus, std::to_wstring(g_cfg.quality) + L"%"); }},
        {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Qualidade WEBP", L"WEBP quality", L"Calidad WEBP"), T(L"Menor = arquivo menor (75 é o ponto ideal)", L"Lower = smaller file (75 is the sweet spot)", L"Menor = archivo más pequeño (75 es lo ideal)")); rstep(r, kWebpMinus, kWebpPlus, std::to_wstring(g_cfg.webpQuality) + L"%"); }},
    };
    if (vec::Available()) rows.insert(rows.begin()+4, {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Texto em SVG / EPS", L"Text in SVG / EPS", L"Texto en SVG / EPS"), T(L"CDR/SVG: instale as fontes originais", L"CDR/SVG: install the original fonts", L"CDR/SVG: instala las fuentes originales")); rseg(r, 240 * s, {T(L"Editável", L"Editable", L"Editable"), T(L"Em curvas", L"As paths", L"En curvas")}, g_cfg.vectorTextPaths ? 1 : 0, kVecText); }});
    drawCard(rows);

    // TEMPO: Relógio (com opções) e Agenda
    section(T(L"TEMPO", L"TIME", L"TIEMPO"));
    rows.clear();
    rows.push_back({RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ativar Relógio", L"Enable Clock", L"Activar Reloj"), T(L"Relógio na área de trabalho", L"Desktop clock", L"Reloj en el escritorio")); rtoggle(kClockOn, r, g_clock.on); }});
    if (g_clock.on) {
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { rlabel(r, T(L"Tamanho do relógio", L"Clock size", L"Tamaño del reloj"), nullptr); rseg(r, 200 * s, {L"100%", L"90%", L"70%"}, g_clock.scalePct == 90 ? 1 : g_clock.scalePct == 70 ? 2 : 0, kClk100); }});
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { rlabel(r, T(L"Fonte do relógio", L"Clock font", L"Fuente del reloj"), T(L"Qualquer fonte instalada no Windows", L"Any font installed in Windows", L"Cualquier fuente instalada en Windows")); wchar_t b[96]; swprintf(b, 96, L"%ls · %lu px", g_clock.font.c_str(), g_clock.fontPx); rpill(r, kClockFont, b, 170 * s); }});
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { rlabel(r, T(L"Anexar à barra de tarefas", L"Attach to the taskbar", L"Anclar a la barra de tareas"), T(L"O relógio do Windows some enquanto estiver anexado", L"The Windows clock hides while attached", L"El reloj de Windows se oculta mientras esté anclado")); rtoggle(kClockDock, r, g_clock.docked); }});
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ativar Roubo de tempo", L"Enable Time theft", L"Activar Robo de tiempo"), T(L"Caveira: conta quem te interrompeu", L"Skull: counts who interrupted you", L"Calavera: cuenta quién te interrumpió")); rtoggle(kTheftOn, r, g_clock.theftOn); }});
        if (g_clock.theftOn)
            rows.push_back({SH, 2, [&](D2D1_RECT_F r) { rlabel(r, T(L"Histórico", L"History", L"Historial"), T(L"Hoje e últimos 7 dias", L"Today and last 7 days", L"Hoy y últimos 7 días")); rpill(r, kTheftHist, T(L"Abrir", L"Open", L"Abrir"), 90 * s); }});
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ativar Foco · Pomodoro", L"Enable Focus · Pomodoro", L"Activar Enfoque · Pomodoro"), T(L"Foco e pausa por cores", L"Focus and break by color", L"Enfoque y pausa por colores")); rtoggle(kFocoOn, r, g_clock.pomoOn); }});
        if (g_clock.pomoOn) {
            rows.push_back({SH, 2, [&](D2D1_RECT_F r) { rlabel(r, T(L"Tempos", L"Times", L"Tiempos"), T(L"Foco · pausa · pausa longa (min)", L"Focus · break · long break (min)", L"Enfoque · pausa · pausa larga (min)")); rseg(r, 270 * s, {L"25 · 5 · 15", L"50 · 10 · 20", L"15 · 3 · 10"}, (int)g_clock.pomoTimes, kPomoT0); }});
            rows.push_back({SH, 2, [&](D2D1_RECT_F r) {
                rlabel(r, T(L"Cores do Pomodoro", L"Pomodoro colors", L"Colores del Pomodoro"), T(L"Foco / pausa", L"Focus / break", L"Enfoque / pausa"));
                D2D1_RECT_F box = rbox(r, 3 * 50 * s);
                float cy = (box.top + box.bottom) / 2;
                for (int i = 0; i < 3; ++i) {
                    float x0 = box.left + i * 50 * s + 6 * s;
                    D2D1_RECT_F all = D2D1::RectF(x0 - 4 * s, cy - 15 * s, x0 + 42 * s, cy + 15 * s);
                    bool h = hit(kPomoC0 + i, all), sel = (int)g_clock.pomoColors == i;
                    p.Fill(D2D1::RectF(x0, cy - 11 * s, x0 + 38 * s, cy + 11 * s), 11 * s, ui::Rgba(kPomoPairs[i][1]));
                    p.Fill(D2D1::RectF(x0, cy - 11 * s, x0 + 27 * s, cy + 11 * s), 11 * s, ui::Rgba(kPomoPairs[i][0]));
                    p.Fill(D2D1::RectF(x0 + 19 * s, cy - 11 * s, x0 + 27 * s, cy + 11 * s), 0, ui::Rgba(kPomoPairs[i][1]));
                    if (sel || h) p.Stroke(all, 15 * s, sel ? th.text : th.text2, 1.4f * s);
                }
            }});
        }
    }
    rows.push_back({RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ativar Agenda", L"Enable Agenda", L"Activar Agenda"), T(L"Seu dia e avisos antes dos eventos", L"Your day and reminders before events", L"Tu día y avisos antes de los eventos")); rtoggle(kAgendaOn, r, g_clock.agendaOn); }});
    if (g_clock.agendaOn)
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { // "Conta Google": desativado por enquanto, só mostra o que virá
            rlabel(r, T(L"Conta Google", L"Google account", L"Cuenta de Google"), T(L"Conecte para ver seus eventos", L"Connect to see your events", L"Conecta para ver tus eventos"));
            D2D1_RECT_F b = rbox(r, 190 * s);
            float rad = (b.bottom - b.top) / 2;
            p.Fill(b, rad, ui::Rgba(0x2E333B));
            p.Stroke(b, rad, ui::Rgba(0x3F444C));
            p.Text(T(L"Conectar Gmail", L"Connect Gmail", L"Conectar Gmail"), b, 12.5f, ui::Rgba(0x5E6570), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        }});
    drawCard(rows);

    section(T(L"OUTROS", L"OTHER", L"OTROS"));
    rows.clear();
    rows.push_back({RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ativar OCR", L"Enable OCR", L"Activar OCR"), T(L"Lê o texto de qualquer área da tela · Ctrl+Alt+T", L"Reads text from any area of the screen · Ctrl+Alt+T", L"Lee el texto de cualquier área de la pantalla · Ctrl+Alt+T")); rtoggle(kOcrOn, r, g_clock.ocr); }});
    if (g_clock.ocr) {
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) { rlabel(r, T(L"Quebras de linha", L"Line breaks", L"Saltos de línea"), T(L"Ao copiar o texto lido", L"When copying the text", L"Al copiar el texto")); rseg(r, 200 * s, {T(L"Manter", L"Keep", L"Mantener"), T(L"Juntar", L"Join", L"Unir")}, ReadDword(L"OcrJoin", 1) ? 1 : 0, kOcrKeep); }});
        rows.push_back({SH, 1, [&](D2D1_RECT_F r) {
            static std::wstring langs; static bool got = false;
            if (!got) { got = true; for (auto& n : ocr::Languages()) langs += (langs.empty() ? L"" : L", ") + n; if (langs.empty()) langs = T(L"nenhum encontrado", L"none found", L"ninguno"); }
            std::wstring sub = std::wstring(T(L"Instalados: ", L"Installed: ", L"Instalados: ")) + langs;
            rlabel(r, T(L"Idiomas de leitura", L"Reading languages", L"Idiomas de lectura"), sub.c_str()); rpill(r, kOcrLang, T(L"Adicionar idioma…", L"Add language…", L"Añadir idioma…"), 150 * s); }});
    }
    rows.push_back({RH, 0, [&](D2D1_RECT_F r) {
        std::wstring names;
        for (auto& n : FolderList()) names += (names.empty() ? L"" : L", ") + n;
        names += T(L" · cada uma com _Morto", L" · each with _Morto", L" · cada una con _Morto");
        rlabel(r, T(L"Pastas Padrão Vizu", L"Vizu folder set", L"Carpetas estándar Vizu"), names.c_str()); rpill(r, kBtnFolders, T(L"Editar", L"Edit", L"Editar"), 90 * s); }});
    rows.push_back({RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Limpeza", L"Cleanup", L"Limpieza"), T(L"Excluir temporários direto, sem passar pela Lixeira", L"Delete temp files directly, skipping the Recycle Bin", L"Eliminar temporales directamente, sin pasar por la Papelera")); rtoggle(kTogTempDel, r, g_cfg.tempPermanent); }});
    drawCard(rows);
    } // !g_lite

    // SISTEMA: recolhível (começa fechado), como um dropdown
    {
        D2D1_RECT_F hd = D2D1::RectF(x, y, x + w, y + 46 * s);
        bool hh = hit(kSysToggle, hd);
        p.Text(T(L"Sistema", L"System", L"Sistema"), D2D1::RectF(x + 10 * s, y + 6 * s, x + w - 40 * s, y + 26 * s), 13, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (!g_sysOpen) p.Text(T(L"5 ajustes (clique para expandir)", L"5 settings (click to expand)", L"5 ajustes (clic para expandir)"), D2D1::RectF(x + 10 * s, y + 26 * s, x + w - 40 * s, y + 44 * s), 11, th.text2);
        D2D1_RECT_F chev = D2D1::RectF(x + w - 34 * s, y + 12 * s, x + w - 10 * s, y + 36 * s);
        if (hh) p.Fill(D2D1::RectF(x, y, x + w, y + 46 * s), 10 * s, ui::Rgba(0xFFFFFF, 0.05f));
        p.Feather(g_sysOpen ? "m18 15-6-6-6 6" : "m6 9 6 6 6-6", chev, 18 * s, th.text2, 1.5f);
        y += 52 * s;
    }
    if (g_sysOpen) {
        rows = {
            {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Fonte da interface", L"Interface font", L"Fuente de la interfaz"), T(L"Qualquer fonte instalada no Windows", L"Any font installed in Windows", L"Cualquier fuente instalada en Windows")); rpill(r, kUiFont, g_uiFont.empty() ? std::wstring(DefaultUiFont()) : g_uiFont, 150 * s); }},
            {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Iniciar com o Windows", L"Start with Windows", L"Iniciar con Windows"), nullptr); rtoggle(kTogAuto, r, AutostartEnabled()); }},
            {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Ícone na área de notificação", L"Notification area icon", L"Icono en el área de notificación"), T(L"Reiniciar ou encerrar com um clique", L"Restart or quit in one click", L"Reiniciar o salir con un clic")); rtoggle(kTogTray, r, g_cfg.tray); }},
            {RH, 0, [&](D2D1_RECT_F r) { rlabel(r, T(L"Idioma", L"Language", L"Idioma"), nullptr); rseg(r, 270 * s, {L"Português", L"English", L"Español"}, i18n::Current(), kLangPt); }},
            {RH, 0, [&](D2D1_RECT_F r) {
                std::wstring ver = std::wstring(T(L"Versão ", L"Version ", L"Versión ")) + upd::kVersion;
                rlabel(r, T(L"Atualizar automaticamente", L"Update automatically", L"Actualizar automáticamente"), ver.c_str());
                D2D1_RECT_F tg = rbox(r, 46 * s, 26);
                float cy = (tg.top + tg.bottom) / 2;
                D2D1_RECT_F pb = D2D1::RectF(tg.left - 12 * s - 170 * s, cy - 15 * s, tg.left - 12 * s, cy + 15 * s);
                p.Pill(pb, T(L"Procurar nova versão", L"Check for new version", L"Buscar nueva versión"), hit(kBtnUpdate, pb));
                p.Toggle(tg, g_cfg.autoUpdate, hit(kTogUpdate, tg));
            }},
        };
        drawCard(rows);
    }

    // TEMAS: miniaturas
    section(T(L"TEMAS", L"THEMES", L"TEMAS"));
    rows.clear();
    rows.push_back({112 * s, 0, [&](D2D1_RECT_F r) {
        struct TC { const wchar_t* name; UINT bg, panel, acc; int mode; };
        TC tc[5] = {{T(L"Claro", L"Light", L"Claro"), 0xF5F5F7, 0xFFFFFF, 0x1D1D1F, ui::kThemeLight}, {T(L"Escuro", L"Dark", L"Oscuro"), 0x0B0B0C, 0x1C1C1E, 0x4A4F57, ui::kThemeDark},
                    {L"Windows", 0xE4E9F1, 0xFFFFFF, ui::SystemAccent(), ui::kThemeWindows}, {L"Vizu Classic", 0x2E333B, 0x4A4F57, 0xFF6633, ui::kThemeClassic},
                    {L"Vizu Crispy", 0x070708, 0x141416, ui::NeonColor(), ui::kThemeCrispy}};
        float cw = 88 * s, chh = 58 * s, gap = (r.right - r.left - 32 * s - 5 * cw) / 4, cx = r.left + 16 * s, cy = r.top + 14 * s;
        for (int i = 0; i < 5; ++i) {
            D2D1_RECT_F b = D2D1::RectF(cx, cy, cx + cw, cy + chh);
            int id = tc[i].mode == ui::kThemeClassic ? kTheme4 : kTheme0 + tc[i].mode;
            bool sel = (int)g_cfg.themeMode == tc[i].mode, h = hit(id, D2D1::RectF(b.left - 4 * s, b.top - 4 * s, b.right + 4 * s, b.bottom + 26 * s));
            if (sel) p.Stroke(D2D1::RectF(b.left - 4 * s, b.top - 4 * s, b.right + 4 * s, b.bottom + 4 * s), 13 * s, th.classic || th.neon ? th.accent : th.text, 2 * s);
            else if (h) p.Stroke(D2D1::RectF(b.left - 4 * s, b.top - 4 * s, b.right + 4 * s, b.bottom + 4 * s), 13 * s, th.text2, 1 * s);
            p.Fill(b, 10 * s, ui::Rgba(tc[i].bg));
            p.Stroke(b, 10 * s, th.stroke, 1 * s);
            p.Fill(D2D1::RectF(b.left + 8 * s, b.top + 8 * s, b.right - 8 * s, b.top + 20 * s), 4 * s, ui::Rgba(tc[i].panel));
            p.Fill(D2D1::RectF(b.left + 8 * s, b.top + 26 * s, b.left + 46 * s, b.top + 38 * s), 4 * s, ui::Rgba(tc[i].panel));
            p.Fill(D2D1::RectF(b.right - 32 * s, b.bottom - 20 * s, b.right - 8 * s, b.bottom - 8 * s), 6 * s, ui::Rgba(tc[i].acc));
            p.Text(tc[i].name, D2D1::RectF(b.left - 10 * s, b.bottom + 6 * s, b.right + 10 * s, b.bottom + 24 * s), 11.5f, sel ? th.text : th.text2, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            cx += cw + gap;
        }
    }});
    if (g_cfg.themeMode == ui::kThemeCrispy)
        rows.push_back({RH, 0, [&](D2D1_RECT_F r) { // cor do neon: amostras + RGB personalizado
            rlabel(r, T(L"Cor do neon", L"Neon color", L"Color del neón"), T(L"Muda o destaque de toda a interface", L"Changes the accent of the whole interface", L"Cambia el acento de toda la interfaz"));
            D2D1_RECT_F box = rbox(r, 7 * 30 * s + 24 * s);
            float d = 22 * s, cy = (box.top + box.bottom) / 2;
            for (int i = 0; i < 7; ++i) {
                float cx = box.left + 6 * s + i * 30 * s + d / 2;
                D2D1_RECT_F rr = D2D1::RectF(cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
                bool h = hit(i < 6 ? kNeon0 + i : kNeonRgb, D2D1::RectF(rr.left - 4 * s, rr.top - 4 * s, rr.right + 4 * s, rr.bottom + 4 * s));
                bool sel = i < 6 ? g_cfg.neon == kNeonPresets[i] : std::find(kNeonPresets, kNeonPresets + 6, (UINT)g_cfg.neon) == kNeonPresets + 6;
                if (i < 6) p.Fill(rr, d / 2, ui::Rgba(kNeonPresets[i]));
                else {
                    p.Fill(rr, d / 2, sel ? ui::Rgba(g_cfg.neon) : ui::Rgba(0xFFFFFF, 0.08f));
                    p.Text(L"RGB", D2D1::RectF(rr.left - 6 * s, rr.top, rr.right + 6 * s, rr.bottom), 8.5f, sel ? ui::Rgba(0x0B0B0C) : th.text, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                if (sel || h) p.Stroke(D2D1::RectF(rr.left - 4 * s, rr.top - 4 * s, rr.right + 4 * s, rr.bottom + 4 * s), d / 2 + 4 * s, sel ? ui::Rgba(0xFFFFFF, 0.9f) : ui::Rgba(0xFFFFFF, 0.35f), 1.4f * s);
            }
        }});
    drawCard(rows);

    section(T(L"AGRADECIMENTO ESPECIAL", L"SPECIAL THANKS", L"AGRADECIMIENTO ESPECIAL"));
    {
        const std::wstring rest = L" por você ser o amor da minha vida!", line2 = L"Gustavo (Primão), Eliza e Thais",
                           line3 = L"Obrigado por me incentivarem e a darem ideias para melhorar nosso dia-a-dia!";
        float tx = x + 16 * s, ty = y + 14 * s, tw = w - 32 * s, lh = 20 * s;
        float h3 = p.Paragraph(line3, tx, 0, tw, 12.5f, th.text2, false);
        D2D1_RECT_F box = D2D1::RectF(x, y, x + w, y + 14 * s + lh * 2 + 6 * s + h3 + 14 * s);
        p.Glass(box, 16 * s);
        float sw = p.Measure(L"Ste", 13.5f, DWRITE_FONT_WEIGHT_BOLD);
        D2D1_RECT_F ste = D2D1::RectF(tx, ty, tx + sw, ty + lh);
        bool hs = hit(kSte, D2D1::RectF(ste.left - 4 * s, ste.top - 2 * s, ste.right + 4 * s, ste.bottom + 2 * s));
        p.Text(L"Ste", ste, 13.5f, ui::Rgba(hs ? 0xF0386B : 0xE0115F), DWRITE_FONT_WEIGHT_BOLD);
        if (hs) p.Fill(D2D1::RectF(ste.left, ste.bottom - 2 * s, ste.right, ste.bottom - 0.8f * s), 0, ui::Rgba(0xF0386B, 0.8f));
        p.Text(rest, D2D1::RectF(ste.right, ty, x + w - 16 * s, ty + lh), 13.5f, th.text);
        p.Text(line2, D2D1::RectF(tx, ty + lh, x + w - 16 * s, ty + 2 * lh), 13.5f, th.text);
        p.Paragraph(line3, tx, ty + 2 * lh + 6 * s, tw, 12.5f, th.text2);
        y = box.bottom + 22 * s;
    }

    section(T(L"SOBRE", L"ABOUT", L"ACERCA DE"));
    {
        const wchar_t* aboutText = T(kAbout, kAboutEn, kAboutEs);
        float ph = p.Paragraph(aboutText, x + 16 * s, y + 14 * s, w - 32 * s, 12, th.text2, false);
        D2D1_RECT_F about = D2D1::RectF(x, y, x + w, y + ph + 28 * s);
        p.Glass(about, 16 * s);
        p.Paragraph(aboutText, x + 16 * s, y + 14 * s, w - 32 * s, 12, th.text2);
        y = about.bottom + 20 * s;
    }
    {
        float bw = (w - 10 * s) / 2;
        D2D1_RECT_F b1 = D2D1::RectF(x, y, x + bw, y + 36 * s), b2 = D2D1::RectF(x + bw + 10 * s, y, x + w, y + 36 * s);
        p.Pill(b1, T(L"Pasta do programa", L"Program folder", L"Carpeta del programa"), hit(kBtnFolder, b1));
        p.Pill(b2, T(L"Encerrar", L"Quit", L"Salir"), hit(kBtnQuit, b2));
        y = b1.bottom + 22 * s;
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
    case WM_SIZE: // minimizada: o fundo escuro some junto; restaurada: volta
        if (g_dimWnd) ShowWindow(g_dimWnd, wp == SIZE_MINIMIZED ? SW_HIDE : SW_SHOWNOACTIVATE);
        g_setSurf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        PaintSettings(h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_setSurf.Release(); g_setHits.clear(); g_settingsWnd = nullptr; if (g_dimWnd) { DestroyWindow(g_dimWnd); g_dimWnd = nullptr; } return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Fundo das Configurações: a tela esmaece para o preto (70%) com desfoque, destacando a janela.
static LRESULT CALLBACK DimProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) { if (g_settingsWnd) DestroyWindow(g_settingsWnd); return 0; } // clicar fora fecha
    if (msg == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    return DefWindowProcW(h, msg, wp, lp);
}
static HWND ShowDim(HMONITOR mon) {
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = DimProc; wc.hInstance = g_inst; wc.lpszClassName = L"VizuDim"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc); reg = true; }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT r = mi.rcWork; // a barra de tarefas continua visível
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"VizuDim", L"", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_inst, nullptr);
    if (!h) return nullptr;
    // vidro escuro: desfoque "acrylic" com preto a 70%; sem ele, só o preto translúcido
    struct AccentPolicy { int state, flags; DWORD color; int anim; };
    struct CompositionData { int attrib; void* data; SIZE_T size; };
    typedef BOOL(WINAPI * SetWca)(HWND, CompositionData*);
    static SetWca set = (SetWca)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    AccentPolicy ap = {4, 0, 0xB3000000, 0};
    CompositionData cd = {19, &ap, sizeof(ap)};
    bool blur = set && set(h, &cd);
    SetLayeredWindowAttributes(h, 0, blur ? 255 : 178, LWA_ALPHA);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
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
    int w = (int)(580 * s), h = min((int)(820 * s), (int)(mi.rcWork.bottom - mi.rcWork.top - 40));
    g_setScroll = 0;
    g_setHot = 0;
    if (g_dimWnd) DestroyWindow(g_dimWnd);
    g_dimWnd = ShowDim(mon);
    g_settingsWnd = CreateWindowExW(WS_EX_APPWINDOW, L"VizuSettings", T(L"Vizu — Configurações", L"Vizu — Settings", L"Vizu — Configuración"), WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                    mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2,
                                    w, h, g_dimWnd, nullptr, g_inst, nullptr); // o fundo escuro é o dono: a janela fica sempre por cima dele
    ui::EnableGlass(g_settingsWnd, g_theme.dark);
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
}

// Atalho na pasta Inicializar (em vez de chave Run no registro).
static std::wstring StartupLink() {
    PWSTR startup = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startup))) return L"";
    std::wstring lnk = std::wstring(startup) + L"\\Vizu.lnk";
    CoTaskMemFree(startup);
    return lnk;
}
static HWND FindRunning() { // Vizu atual ou uma versão antiga (janela com o nome anterior)
    if (HWND w = FindWindowW(L"VizuWnd", nullptr)) return w;
    return FindWindowW(L"DesignPeekWnd", nullptr);
}
static std::wstring OldLink(const std::wstring& lnk) { return lnk.substr(0, lnk.size() - 8) + L"DesignPeek.lnk"; } // atalho das versões antigas
static bool AutostartEnabled() {
    std::wstring l = StartupLink();
    return !l.empty() && (GetFileAttributesW(l.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(OldLink(l).c_str()) != INVALID_FILE_ATTRIBUTES);
}

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
    DeleteFileW(OldLink(lnk).c_str());
    if (on) CreateLink(lnk, L"", L"Vizu");
    else DeleteFileW(lnk.c_str());
}

// Atalho no menu Iniciar para abrir as configurações.
static void SetStartMenu(bool on) {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return;
    std::wstring lnk = std::wstring(programs) + L"\\Vizu.lnk";
    CoTaskMemFree(programs);
    DeleteFileW(OldLink(lnk).c_str());
    if (on) CreateLink(lnk, L"/settings", L"Vizu - configurações"); else DeleteFileW(lnk.c_str());
}

// ---------- ícone na área de notificação: Configurações, Reiniciar, Encerrar ----------

static void UpdateTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_wnd; nid.uID = kTrayId;
    if (!g_cfg.tray) { Shell_NotifyIconW(NIM_DELETE, &nid); return; }
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(1), LIM_SMALL, &nid.hIcon);
    swprintf(nid.szTip, ARRAYSIZE(nid.szTip), L"Vizu %ls", upd::kVersion);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_wnd; nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Reinicia: abre uma nova instância que espera esta terminar.
static void RestartSelf() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" /waitpid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    PostMessage(g_wnd, WM_CLOSE, 0, 0);
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    std::wstring title = std::wstring(L"Vizu ") + upd::kVersion;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, title.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuSettings, T(L"Configurações", L"Settings", L"Configuración"));
    AppendMenuW(m, MF_STRING, kMenuRestart, T(L"Reiniciar", L"Restart", L"Reiniciar"));
    AppendMenuW(m, MF_STRING, kMenuQuit, T(L"Encerrar", L"Quit", L"Salir"));
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd); // necessário para o menu fechar ao clicar fora
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (cmd == kMenuSettings) OpenSettings();
    else if (cmd == kMenuRestart) RestartSelf();
    else if (cmd == kMenuQuit) PostMessage(g_wnd, WM_CLOSE, 0, 0);
}

// ---------- Novo > Pasta Padrão Maker ----------

// Lista de pastas em %LOCALAPPDATA%\\DesignThumbs\\pastas.txt (uma por linha; "Links\\Fontes" cria subpasta).
static std::wstring FolderListPath(bool create = true) {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return L"";
    std::wstring d = std::wstring(dir) + L"\\DesignThumbs";
    CreateDirectoryW(d.c_str(), nullptr);
    std::wstring f = d + L"\\pastas.txt";
    if (create && GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const char def[] = "\xEF\xBB\xBF" "Edit\r\nLinks\r\nPreview\r\n"; // UTF-8 com BOM (o Bloco de Notas abre certo)
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, def, sizeof(def) - 1, &w, nullptr); CloseHandle(h); }
    }
    return f;
}

static std::vector<std::wstring> FolderList() {
    std::vector<std::wstring> out;
    std::wstring f = FolderListPath();
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[16384]; DWORD n = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
        CloseHandle(h);
        std::string txt(buf, n);
        if (txt.compare(0, 3, "\xEF\xBB\xBF") == 0) txt.erase(0, 3);
        std::wstring w = fmt::Widen(txt), line;
        for (wchar_t c : w + L"\n") {
            if (c == L'\n' || c == L'\r') {
                line.erase(0, line.find_first_not_of(L" \t\\/"));
                line.erase(line.find_last_not_of(L" \t\\/") + 1);
                bool ok = !line.empty() && line.find_first_of(L":*?\"<>|") == std::wstring::npos && line.find(L"..") == std::wstring::npos;
                if (ok) out.push_back(line);
                line.clear();
            } else line += c;
        }
    }
    if (out.empty()) out = {L"Edit", L"Links", L"Preview"};
    return out;
}
// O Explorer ordena o menu Novo pela extensão: começar com "0" deixa o item logo depois de Pasta e Atalho.
static const wchar_t* kNewExt = L"Software\\Classes\\.0pastapadraomaker";
static const wchar_t* kNewExtOld = L"Software\\Classes\\.designpeekfolders"; // v6
static const wchar_t* kNewProgId = L"Software\\Classes\\DesignThumbs.PastaPadraoMaker";

// Entrada no menu Novo do Explorer (por usuário). O Explorer chama "Vizu.exe /newfolders <arquivo novo>".
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
        set(kNewProgId, nullptr, T(L"Pastas Padrão Vizu", L"Vizu folder set", L"Carpetas estándar Vizu"));
        set(std::wstring(kNewProgId) + L"\\DefaultIcon", nullptr, L"%SystemRoot%\\System32\\imageres.dll,3");
    }
    // Botão direito no fundo de uma pasta / na área de trabalho: "Limpar arquivos temporários"
    for (const wchar_t* bg : {L"Software\\Classes\\Directory\\Background\\shell\\DesignThumbs.LimparTemp", L"Software\\Classes\\DesktopBackground\\shell\\DesignThumbs.LimparTemp"}) {
        RegDeleteTreeW(HKEY_CURRENT_USER, bg);
        if (!on) continue;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring k = bg, verb = T(L"Limpar arquivos temporários", L"Clean temporary files", L"Limpiar archivos temporales"),
                     icon = L"%SystemRoot%\\System32\\imageres.dll,-54", cmd = L"\"" + std::wstring(exe) + L"\" /cleantemp";
        RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), L"MUIVerb", REG_SZ, verb.c_str(), (DWORD)((verb.size() + 1) * 2));
        RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), L"Icon", REG_EXPAND_SZ, icon.c_str(), (DWORD)((icon.size() + 1) * 2));
        RegSetKeyValueW(HKEY_CURRENT_USER, (k + L"\\command").c_str(), nullptr, REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * 2));
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
    std::set<std::wstring> selected;
    for (auto& name : FolderList()) {
        std::wstring f = dir + L"\\" + name;
        for (auto& c : f) if (c == L'/') c = L'\\';
        int r = SHCreateDirectoryExW(nullptr, f.c_str(), nullptr); // cria subpastas também
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) continue;
        if (Lower(f).size() < 6 || Lower(f).compare(Lower(f).size() - 6, 6, L"_morto")) CreateDirectoryW((f + L"\\_Morto").c_str(), nullptr); // v9.2: pasta "_Morto" em cada uma
        std::wstring top = dir + L"\\" + name.substr(0, name.find_first_of(L"\\/")); // seleciona a pasta de primeiro nível
        if (!selected.insert(Lower(top)).second) continue;
        SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW, top.c_str(), nullptr);
        PIDLIST_ABSOLUTE p = nullptr;
        if (SUCCEEDED(SHParseDisplayName(top.c_str(), nullptr, &p, 0, nullptr))) items.push_back(p);
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

// ---------- Limpar arquivos temporários ----------
// Pega o que está na pasta TEMP do usuário e não foi mexido há mais de 24 h; o que estiver em uso fica.
static void TempScan(const std::wstring& dir, ULONGLONG* bytes, size_t* files) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) TempScan(dir + L"\\" + fd.cFileName, bytes, files);
        else { *bytes += (ULONGLONG)fd.nFileSizeHigh << 32 | fd.nFileSizeLow; ++*files; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void DeleteTree(const std::wstring& path, bool dir) { // exclusão definitiva; ignora o que estiver em uso
    if (dir) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                std::wstring c = path + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(c.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(c.c_str()) || DeleteFileW(c.c_str());
                else DeleteTree(c, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path.c_str());
    } else DeleteFileW(path.c_str());
}

static std::wstring SizeText(ULONGLONG b) {
    wchar_t t[64];
    if (b >= 1ull << 30) swprintf(t, 64, L"%.1f GB", b / 1073741824.0);
    else if (b >= 1ull << 20) swprintf(t, 64, L"%.0f MB", b / 1048576.0);
    else swprintf(t, 64, L"%.0f KB", b / 1024.0);
    return t;
}

static ULONGLONG TempUsage(const std::wstring& tmp) { ULONGLONG b = 0; size_t n = 0; TempScan(tmp, &b, &n); return b; }

static void CleanTemp() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (!n) return;
    std::wstring tmp(buf, n);
    while (!tmp.empty() && tmp.back() == L'\\') tmp.pop_back();
    if (tmp.size() < 4) return; // segurança: nunca a raiz de um disco
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = (ULONGLONG)nowFt.dwHighDateTime << 32 | nowFt.dwLowDateTime, day = 24ull * 3600 * 10000000;
    struct Item { std::wstring path; bool dir; };
    std::vector<Item> items;
    ULONGLONG bytes = 0; size_t files = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((tmp + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            ULONGLONG w = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
            if (now - w < day) continue; // recente: pode estar em uso por algum programa
            std::wstring p = tmp + L"\\" + fd.cFileName;
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) TempScan(p, &bytes, &files);
            else if (!dir) { bytes += (ULONGLONG)fd.nFileSizeHigh << 32 | fd.nFileSizeLow; ++files; }
            items.push_back({p, dir});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const wchar_t* title = T(L"Limpar arquivos temporários", L"Clean temporary files", L"Limpiar archivos temporales");
    if (items.empty()) {
        MessageBoxW(nullptr, T(L"Não há arquivos temporários antigos para limpar.", L"There are no old temporary files to clean.", L"No hay archivos temporales antiguos para limpiar."), title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    bool perm = g_cfg.tempPermanent;
    wchar_t msg[512];
    swprintf(msg, 512, T(L"Encontrados %zu arquivos inúteis (%ls) na pasta TEMP, sem uso há mais de 1 dia.\n\n%ls",
                         L"Found %zu useless files (%ls) in the TEMP folder, unused for more than 1 day.\n\n%ls",
                         L"Se encontraron %zu archivos inútiles (%ls) en la carpeta TEMP, sin uso hace más de 1 día.\n\n%ls"),
             files, SizeText(bytes).c_str(),
             perm ? T(L"Excluir definitivamente (sem Lixeira)?", L"Delete permanently (no Recycle Bin)?", L"¿Eliminar definitivamente (sin Papelera)?")
                  : T(L"Mover para a Lixeira?", L"Move to the Recycle Bin?", L"¿Mover a la Papelera?"));
    if (MessageBoxW(nullptr, msg, title, MB_OKCANCEL | (perm ? MB_ICONWARNING : MB_ICONQUESTION)) != IDOK) return;
    ULONGLONG before = TempUsage(tmp);
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    if (perm) for (auto& it : items) DeleteTree(it.path, it.dir);
    else {
        IFileOperation* op = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
            op->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD);
            for (auto& it : items) {
                IShellItem* si = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(it.path.c_str(), nullptr, IID_PPV_ARGS(&si)))) { op->DeleteItem(si, nullptr); si->Release(); }
            }
            op->PerformOperations(); // o que estiver em uso é pulado
            op->Release();
        }
    }
    SetCursor(old);
    ULONGLONG after = TempUsage(tmp), freed = before > after ? before - after : 0;
    if (perm) {
        swprintf(msg, 512, T(L"Pronto: %ls liberados.", L"Done: %ls freed.", L"Listo: %ls liberados."), SizeText(freed).c_str());
        MessageBoxW(nullptr, msg, title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    SHQUERYRBINFO rb = {sizeof(rb)};
    SHQueryRecycleBinW(nullptr, &rb);
    swprintf(msg, 512, T(L"%ls foram para a Lixeira (o que estava em uso ficou).\n\nA Lixeira tem %ls no total. Esvaziar a Lixeira agora?",
                         L"%ls went to the Recycle Bin (files in use were kept).\n\nThe Recycle Bin holds %ls in total. Empty the Recycle Bin now?",
                         L"%ls fueron a la Papelera (lo que estaba en uso se quedó).\n\nLa Papelera tiene %ls en total. ¿Vaciar la Papelera ahora?"),
             SizeText(freed).c_str(), SizeText((ULONGLONG)rb.i64Size).c_str());
    if (MessageBoxW(nullptr, msg, title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION);
}

// Diagnóstico: "Vizu.exe /diag arquivo" mostra, passo a passo, como o arquivo é lido.
static void Diagnose(const std::wstring& path) {
    std::wstring r;
    wchar_t t[512];
    auto line = [&](const wchar_t* fmtStr, ...) { va_list ap; va_start(ap, fmtStr); vswprintf(t, 512, fmtStr, ap); va_end(ap); r += t; r += L"\r\n"; };
    line(L"Vizu %ls  ·  %ls", upd::kVersion, path.c_str());
    std::wstring ext = conv::ExtOf(path);
    line(L"Extensão: %ls  ·  tratada pelo Vizu: %ls", ext.c_str(), fmt::Handles(ext) ? L"sim" : L"NÃO");
    IStream* s = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s);
    line(L"Abrir arquivo: 0x%08X", (unsigned)hr);
    if (s) {
        BYTE magic[8] = {};
        fmt::ReadAt(s, 0, magic, 8);
        line(L"Começo do arquivo: %.4hs", (const char*)magic);
        DWORD t0 = GetTickCount();
        pdfrt::Document doc;
        bool ok = doc.Open(s);
        line(L"Leitor de PDF do Windows: %ls (%u ms)  ·  páginas: %u", ok ? L"OK" : L"FALHOU", GetTickCount() - t0, ok ? doc.Pages() : 0);
        if (ok) {
            t0 = GetTickCount();
            IStream* png = doc.RenderPng(0, 512);
            line(L"Renderizar página 1: %ls (%u ms)", png ? L"OK" : L"FALHOU", GetTickCount() - t0);
            if (png) png->Release();
        }
        HBITMAP b = nullptr; bool a; int pages = 0;
        t0 = GetTickCount();
        hr = fmt::Render(s, ext, 512, 1, &b, &a, &pages, true);
        BITMAP bm = {}; if (b) GetObject(b, sizeof(bm), &bm);
        line(L"Render (página 2): 0x%08X  ·  %dx%d  ·  páginas: %d  (%u ms)", (unsigned)hr, bm.bmWidth, std::abs(bm.bmHeight), pages, GetTickCount() - t0);
        if (b) DeleteObject(b);
        fmt::Info in;
        fmt::GetInfo(s, ext, &in);
        line(L"Informações: páginas %u  ·  %ls", in.pages, in.pageSize.c_str());
        s->Release();
    }
    HBITMAP b = nullptr; bool a; int pages = 0;
    DWORD t0 = GetTickCount();
    bool ok = conv::RenderFile(path, 512, 0, &b, &a, &pages);
    line(L"RenderFile (usado pelo preview): %ls  ·  páginas: %d  (%u ms)", ok ? L"OK" : L"FALHOU", pages, GetTickCount() - t0);
    if (b) DeleteObject(b);
    wchar_t v[128] = {}; DWORD sz = sizeof(v);
    RegGetValueW(HKEY_CLASSES_ROOT, (ext + L"\\ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, v, &sz);
    line(L"Painel Alt+P registrado para %ls: %ls", ext.c_str(), v[0] ? v : L"(nenhum)");
    wchar_t prog[128] = {}; sz = sizeof(prog);
    RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, prog, &sz);
    v[0] = 0; sz = sizeof(v);
    if (prog[0]) RegGetValueW(HKEY_CLASSES_ROOT, (std::wstring(prog) + L"\\ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, v, &sz);
    line(L"Programa padrão: %ls  ·  painel dele: %ls", prog[0] ? prog : L"(nenhum)", v[0] ? v : L"(nenhum)");
    line(L"");
    line(L"Ctrl+C copia este texto.");
    MessageBoxW(nullptr, r.c_str(), L"Vizu — diagnóstico", MB_OK | MB_ICONINFORMATION);
}

// ---------- instalação (o próprio Vizu.exe é o instalador; a DLL vai embutida) ----------
// Sem derrubar o Explorer: DLL/EXE em uso são renomeados para *.old e apagados depois.

static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DesignThumbs";

static std::wstring InstallDir() {
    wchar_t d[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
    return std::wstring(d) + L"\\Programs\\DesignThumbs";
}
static std::wstring SelfPath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }
static bool RunningInstalled() { return !_wcsicmp(conv::DirOf(SelfPath()).c_str(), InstallDir().c_str()); }

// Escreve `data` em `path`; se o arquivo estiver em uso (DLL no Explorer), renomeia o antigo antes.
static bool ReplaceFile(const std::wstring& path, const void* data, DWORD size, const std::wstring& copyFrom = L"") {
    auto write = [&]() -> bool {
        if (!copyFrom.empty()) return CopyFileW(copyFrom.c_str(), path.c_str(), FALSE) != 0;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0;
        bool ok = WriteFile(h, data, size, &w, nullptr) && w == size;
        CloseHandle(h);
        return ok;
    };
    if (write()) return true;
    std::wstring old = path + L"." + std::to_wstring(GetTickCount()) + L".old";
    return MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && write();
}

static void CleanupOld(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.old").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(f, &fd));
    FindClose(f);
}

static bool CallDll(const std::wstring& dll, const char* fn) {
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) return false;
    typedef HRESULT(STDAPICALLTYPE * Fn)();
    Fn f = (Fn)(void*)GetProcAddress(m, fn);
    bool ok = f && SUCCEEDED(f());
    FreeLibrary(m);
    return ok;
}

static void QuitRunning() {
    if (HWND w = FindRunning()) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessage(w, WM_CLOSE, 0, 0);
        if (p) { WaitForSingleObject(p, 4000); CloseHandle(p); }
    }
}

static void SetAutostart(bool on);
static void SetStartMenu(bool on);
static void SetNewMenu(bool on);

static int Uninstall(bool silent, bool ask = true);

struct InstallLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\Vizu.Installer");
    bool owned = false;
    InstallLock() {
        if (handle) { DWORD wait = WaitForSingleObject(handle, 0); owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }
    }
    ~InstallLock() { if (owned) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
};

static bool InstallFiles(int edition, const installui::Progress& progress, std::wstring& error) {
    bool vectors = edition == vec::kUltra;
    std::wstring dir = InstallDir();
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = T(L"Não foi possível criar a pasta do Vizu.",L"Could not create the Vizu folder.",L"No se pudo crear la carpeta de Vizu."); return false;
    }
    QuitRunning();
    progress(10, vectors ? T(L"Preparando o motor vetorial…",L"Preparing the vector engine…",L"Preparando el motor vectorial…") : T(L"Preparando os arquivos do Vizu…",L"Preparing Vizu files…",L"Preparando los archivos de Vizu…"));
    vbundle::Cancel().store(false);
    bool engineReady = vbundle::Configure(dir, vectors, error, [&progress](unsigned long long got, unsigned long long total, double bps) {
        int pct = total ? 10 + (int)(60.0 * got / total) : 10;
        progress(pct, vbundle::Status(got, total, bps));
    });
    if (!engineReady) return false;
    WriteDword(L"Edition", (DWORD)edition); // antes do /autostart (menu Novo depende da edição)
    WriteDword(L"VectorConverterInstalled", vectors ? 1 : 0);
    progress(65, T(L"Instalando arquivos e miniaturas…",L"Installing files and thumbnails…",L"Instalando archivos y miniaturas…"));
    std::wstring exe = dir + L"\\Vizu.exe", dll = dir + L"\\DesignThumbs.dll";
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    bool ok = g && ReplaceFile(dll, LockResource(g), SizeofResource(nullptr, r)) && ReplaceFile(exe, nullptr, 0, SelfPath());
    DeleteFileW((dir + L"\\Uninstall.exe").c_str()); // sobra do instalador antigo
    DeleteFileW((dir + L"\\DesignPeek.exe").c_str()); // nome da versão antiga (9.8.2 e anteriores)
    ok = ok && CallDll(dll, "DllRegisterServer");
    if (!ok) {
        error = T(L"Não foi possível instalar os arquivos do Vizu.", L"Could not install Vizu files.", L"No se pudieron instalar los archivos de Vizu.");
        return false;
    }
    progress(85, T(L"Configurando atalhos e miniaturas…",L"Setting up shortcuts and thumbnails…",L"Configurando accesos y miniaturas…"));
    // atalhos (Inicializar, menu Iniciar) e menu Novo, apontando para o EXE instalado
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi;
    std::wstring cmd = L"\"" + exe + L"\" /autostart";
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    auto sz = [](const std::wstring& v) { return (DWORD)((v.size() + 1) * sizeof(wchar_t)); };
    std::wstring un = L"\"" + exe + L"\" /uninstall", quiet = un + L" /S";
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"DisplayName", REG_SZ, L"Vizu", 10);
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"DisplayVersion", REG_SZ, upd::kVersion, sz(upd::kVersion));
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"Publisher", REG_SZ, L"Thiago Crispim", 30);
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"DisplayIcon", REG_SZ, exe.c_str(), sz(exe));
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", REG_SZ, dir.c_str(), sz(dir));
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"UninstallString", REG_SZ, un.c_str(), sz(un));
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"QuietUninstallString", REG_SZ, quiet.c_str(), sz(quiet));
    DWORD one = 1, size = vectors ? 450000 : 4000; // KB; o Ultra inclui o motor vetorial extraído
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"NoModify", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"NoRepair", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"EstimatedSize", REG_DWORD, &size, 4);

    progress(100, T(L"Instalação concluída.",L"Installation complete.",L"Instalación completada."));
    return true;
}

static void StartInstalledApp(bool settings) {
    std::wstring command = L"\"" + InstallDir() + L"\\Vizu.exe\"" + (settings ? L" /settings" : L"");
    STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
    if(CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {CloseHandle(pi.hProcess);CloseHandle(pi.hThread);}
}

static int Install(bool silent) {
    InstallLock lock;
    if(!lock.owned) {
        if(!silent) MessageBoxW(nullptr,T(L"Já há uma instalação em andamento.",L"An installation is already running.",L"Ya hay una instalación en curso."),L"Vizu",MB_ICONINFORMATION);
        return 1;
    }
    LoadConfig();
    if(silent) {
        std::wstring error;
        bool ok=InstallFiles((int)vec::Edition(), [](int,const std::wstring&){}, error);
        if(ok) StartInstalledApp(false);
        return ok?0:1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    installui::Window ui;
    HKEY existing;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,kUninstKey,0,KEY_READ,&existing)==ERROR_SUCCESS){ui.installed=true;RegCloseKey(existing);}
    ui.edition=(int)vec::Edition();
    ui.theme=ui::MakeTheme(ReadDword(L"Theme2",ui::kThemeClassic),false);
    ui.work=InstallFiles;
    ui.cancel=&vbundle::Cancel();
    int result=ui.Show(g_inst);
    if(result==3) return Uninstall(false,false);
    if(result==2) StartInstalledApp(true);
    return result==1?1:0;
}

static int Uninstall(bool silent, bool ask) {
    InstallLock lock;
    if (!lock.owned) {
        if (!silent) MessageBoxW(nullptr, T(L"Aguarde a instalação do Vizu terminar antes de remover o aplicativo.", L"Wait for the Vizu installation to finish before uninstalling.", L"Espera a que termine la instalación de Vizu antes de desinstalar."), L"Vizu", MB_ICONINFORMATION);
        return 1;
    }
    if (!silent && ask && MessageBoxW(nullptr, T(L"Desinstalar o Vizu?", L"Uninstall Vizu?", L"¿Desinstalar Vizu?"), L"Vizu",
                               MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
    std::wstring dir = InstallDir();
    QuitRunning();
    SetAutostart(false); SetStartMenu(false); SetNewMenu(false);
    CallDll(dir + L"\\DesignThumbs.dll", "DllUnregisterServer");
    vbundle::Cleanup(dir, true);
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    // Arquivos em uso (a DLL no Explorer e este EXE) vão para a pasta temporária; a pasta do programa some.
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    for (const wchar_t* f : {L"DesignThumbs.dll", L"Vizu.exe", L"DesignPeek.exe"}) { // DesignPeek.exe: sobra da versão antiga
        std::wstring src = dir + L"\\" + f;
        if (!DeleteFileW(src.c_str()))
            MoveFileExW(src.c_str(), (std::wstring(tmp) + L"DesignThumbs-" + std::to_wstring(GetTickCount()) + L"-" + f + L".old").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    CleanupOld(dir);
    for (const wchar_t* f : {L"Vizu.ico", L"DesignPeek.ico", L"diagnostico.bat", L"README.md", L"Uninstall.exe"}) DeleteFileW((dir + L"\\" + f).c_str());
    RemoveDirectoryW(dir.c_str());
    if (!silent) MessageBoxW(nullptr, T(L"Vizu removido.", L"Vizu removed.", L"Vizu eliminado."), L"Vizu", MB_ICONINFORMATION);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    g_inst = inst;
    i18n::Load();
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"/autostart") != std::wstring::npos || args.find(L"/noautostart") != std::wstring::npos) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool on = args.find(L"/noautostart") == std::wstring::npos;
        SetAutostart(on);
        SetStartMenu(on);
        SetNewMenu(on && vec::Edition() != vec::kLite); // Lite: sem Pastas Padrão no menu Novo
        CoUninitialize();
        return 0;
    }
    if(args.find(L"/installer-preview") != std::wstring::npos) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        installui::Window ui; ui.preview=true; ui.theme=ui::ClassicTheme();
        if(args.find(L"/installer-preview-progress")!=std::wstring::npos){ui.page=1;ui.percent=65;ui.phase=T(L"Instalando arquivos e miniaturas…",L"Installing files and thumbnails…",L"Instalando archivos y miniaturas…");}
        if(args.find(L"/installer-preview-done")!=std::wstring::npos){ui.page=2;ui.success=true;ui.focus=2;}
        ui.Show(g_inst); CoUninitialize(); return 0;
    }
    bool silent = args.find(L"/S") != std::wstring::npos;
    if (args.find(L"/uninstall") != std::wstring::npos) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); return Uninstall(silent); }
    // Rodando de fora da pasta de instalação (o instalador baixado), sem comandos: instala.
    if (args.find(L"/install") != std::wstring::npos || (!RunningInstalled() && (args.empty() || args.find(L"/update") != std::wstring::npos))) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        return Install(silent);
    }
    if (RunningInstalled()) CleanupOld(InstallDir());
    size_t dg = args.find(L"/diag");
    if (dg != std::wstring::npos) {
        std::wstring f = args.substr(dg + 5);
        f.erase(0, f.find_first_not_of(L" \"")); f.erase(f.find_last_not_of(L" \"") + 1);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Diagnose(f);
        CoUninitialize();
        return 0;
    }
    if (args.find(L"/cleantemp") != std::wstring::npos) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        LoadConfig();
        CleanTemp();
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
        if (HWND w = FindRunning()) PostMessage(w, WM_CLOSE, 0, 0);
        return 0;
    }
    size_t wp = args.find(L"/waitpid ");
    if (wp != std::wstring::npos) { // reinício: espera a instância anterior fechar
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtoi(args.c_str() + wp + 9));
        if (old) { WaitForSingleObject(old, 8000); CloseHandle(old); }
        args.erase(wp);
    }
    CreateMutexW(nullptr, TRUE, L"Vizu.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { // já rodando: abrir de novo = abrir as configurações
        if (HWND w = FindRunning()) { AllowSetForegroundWindow(ASFW_ANY); PostMessage(w, WM_OPEN_SETTINGS, 0, 0); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadConfig();
    i18n::Load();
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw)))
        return 1;

    WNDCLASSW wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VizuWnd";
    RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = AltProc; wc.lpszClassName = L"VizuAlt"; RegisterClassW(&wc);
    wc.lpfnWndProc = ToastProc; wc.lpszClassName = L"VizuToast"; RegisterClassW(&wc);
    wc.lpfnWndProc = DialogProc; wc.lpszClassName = L"VizuDialog"; RegisterClassW(&wc);
    wc.lpfnWndProc = CropProc; wc.lpszClassName = L"VizuCrop"; RegisterClassW(&wc);
    wc.lpfnWndProc = SettingsProc; wc.lpszClassName = L"VizuSettings";
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1)); // ícone do programa
    RegisterClassW(&wc);

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    g_wnd = CreateWindowExW(exStyle, L"VizuWnd", L"Vizu", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_altWnd = CreateWindowExW(exStyle | WS_EX_LAYERED, L"VizuAlt", L"Vizu", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_toastWnd = CreateWindowExW(exStyle, L"VizuToast", L"Vizu", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    g_uiFont = ReadStr(L"UiFont", L"");
    ApplyTheme(); // vidro + tema
    ClockLoad();
    if (g_clock.ocr) RegisterHotKey(g_wnd, kHotOcr, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T'); // OCR funciona mesmo com o relógio desligado
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    UpdateTrayIcon();

    std::thread(WorkerLoop).detach();
    g_focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_selHook = SetWinEventHook(EVENT_OBJECT_SELECTION, EVENT_OBJECT_SELECTIONWITHIN, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    UpdateHotkeys();
    if (args.find(L"/settings") != std::wstring::npos) OpenSettings();
    SetTimer(g_wnd, kUpdateTimer, 60 * 1000, nullptr); // primeira verificação um minuto depois de iniciar
    // "/ring a b c" abre o menu circular para esses arquivos; "/resize", "/pages" e "/crop" abrem as ferramentas
    for (const wchar_t* cmdName : {L"/ring ", L"/resize ", L"/pages ", L"/crop "}) {
        size_t at = args.find(cmdName);
        if (at == std::wstring::npos) continue;
        int n = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
        g_convFiles.clear();
        for (int i = 1; i < n; ++i) if (argv[i][0] != L'/' && IsFile(argv[i])) g_convFiles.push_back(argv[i]);
        LocalFree(argv);
        if (g_convFiles.empty()) break;
        std::wstring c = cmdName;
        if (c == L"/ring ") {
            BuildSides();
            POINT pt; GetCursorPos(&pt);
            g_ringScale = GetDpiForSystem() / 96.0f;
            int S = (int)RingS();
            g_ringAlpha = 255;
            SetWindowPos(g_altWnd, HWND_TOPMOST, pt.x - S / 2, pt.y - S / 2, S, S, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RenderRing();
        } else if (c == L"/resize ") OpenResizeDialog();
        else if (c == L"/pages ") OpenPagesDialog();
        else OpenCropEditor();
        break;
    }
    size_t peek = args.find(L"/peek ");
    if (peek != std::wstring::npos) { // abre um arquivo direto (útil em "Abrir com")
        std::wstring f = args.substr(peek + 6);
        f.erase(0, f.find_first_not_of(L" \"")); f.erase(f.find_last_not_of(L" \"") + 1);
        OpenPath(f, {});
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g_dlgEdit && m.hwnd == g_dlgEdit && m.message == WM_KEYDOWN && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE)) {
            if (m.wParam == VK_RETURN) DialogAction(2); else CloseDialog();
            continue;
        }
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
