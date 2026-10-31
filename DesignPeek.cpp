// DesignPeek - "Quick Look" para o Explorer: selecione um arquivo e aperte Espaço.
// Espaço/Esc/clique fecham. Setas trocam de arquivo com o preview aberto.
// Usa os thumbnail providers do Windows (incluindo o DesignThumbs), então funciona com
// PSD, AI, INDD, SVG, JPG, PNG, PDF... sem decodificar nada aqui.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <algorithm>
#include <string>

static HWND g_wnd;
static HHOOK g_hook;
static bool g_open, g_swallowSpaceUp;
static HWND g_defView;           // lista de arquivos de onde veio o Espaço
static HBITMAP g_bmp;
static SIZE g_bmpSize;
static std::wstring g_path, g_name;

enum { WM_PEEK_OPEN = WM_APP + 1, WM_PEEK_CLOSE, WM_PEEK_REFRESH };
static const int kCaption = 40, kPad = 12, kRefreshTimer = 1;
static const COLORREF kBg = RGB(30, 30, 30), kText = RGB(230, 230, 230);

static std::wstring ClassOf(HWND h) {
    wchar_t c[64] = {};
    GetClassNameW(h, c, 64);
    return c;
}

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
    if (fv) fv->Release(); if (sv) sv->Release(); if (sb) sb->Release(); if (sp) sp->Release();
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

// Alguns bitmaps 32bpp vêm com alpha zerado; outros com alpha não pré-multiplicado.
static void PrepareAlpha(HBITMAP bmp) {
    DIBSECTION ds;
    if (GetObject(bmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) return;
    BYTE* p = (BYTE*)ds.dsBm.bmBits;
    size_t n = (size_t)ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight);
    bool anyAlpha = false, premul = true;
    for (size_t i = 0; i < n; ++i) {
        BYTE a = p[i * 4 + 3];
        if (a) anyAlpha = true;
        if (p[i * 4] > a || p[i * 4 + 1] > a || p[i * 4 + 2] > a) premul = false;
    }
    for (size_t i = 0; i < n; ++i, p += 4) {
        if (!anyAlpha) p[3] = 255;
        else if (!premul) { p[0] = p[0] * p[3] / 255; p[1] = p[1] * p[3] / 255; p[2] = p[2] * p[3] / 255; }
    }
}

static void ClosePreview() {
    g_open = false;
    KillTimer(g_wnd, kRefreshTimer);
    ShowWindow(g_wnd, SW_HIDE);
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = nullptr; }
    g_path.clear();
}

static bool LoadSelection() {
    IShellItem* item = SelectedItem(g_defView);
    if (!item) return false;
    PWSTR path = nullptr, name = nullptr;
    item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path);
    item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
    std::wstring p = path ? path : L"";
    bool same = g_open && p == g_path;
    if (!same) {
        HMONITOR mon = MonitorFromWindow(g_defView, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        RECT wa = mi.rcWork;
        int maxW = (wa.right - wa.left) * 85 / 100, maxH = (wa.bottom - wa.top) * 85 / 100 - kCaption;

        HBITMAP bmp = nullptr;
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&f)))) {
            SIZE req = {maxW - 2 * kPad, maxH - kPad};
            if (FAILED(f->GetImage(req, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bmp)))
                f->GetImage({256, 256}, SIIGBF_ICONONLY, &bmp); // sem preview: mostra o ícone
            f->Release();
        }
        if (bmp) {
            if (g_bmp) DeleteObject(g_bmp);
            g_bmp = bmp;
            PrepareAlpha(bmp);
            BITMAP bm; GetObject(bmp, sizeof(bm), &bm);
            g_bmpSize = {bm.bmWidth, std::abs(bm.bmHeight)};
            g_path = p;
            g_name = name ? name : L"";
            int w = std::max<int>(g_bmpSize.cx + 2 * kPad, 360), h = g_bmpSize.cy + kPad + kCaption;
            SetWindowPos(g_wnd, HWND_TOPMOST, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                         w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(g_wnd, nullptr, TRUE);
            g_open = true;
        }
    }
    CoTaskMemFree(path); CoTaskMemFree(name);
    item->Release();
    return g_open;
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* kb = (KBDLLHOOKSTRUCT*)lp;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (kb->vkCode == VK_SPACE && !down && g_swallowSpaceUp) { g_swallowSpaceUp = false; return 1; }
        if (down) {
            if (g_open) {
                if (kb->vkCode == VK_SPACE || kb->vkCode == VK_ESCAPE) {
                    g_swallowSpaceUp = kb->vkCode == VK_SPACE;
                    PostMessage(g_wnd, WM_PEEK_CLOSE, 0, 0);
                    return 1;
                }
                if (kb->vkCode >= VK_LEFT && kb->vkCode <= VK_DOWN) PostMessage(g_wnd, WM_PEEK_REFRESH, 0, 0);
                else if (kb->vkCode != VK_SHIFT && kb->vkCode != VK_LSHIFT && kb->vkCode != VK_RSHIFT) PostMessage(g_wnd, WM_PEEK_CLOSE, 0, 0);
            } else if (kb->vkCode == VK_SPACE && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
                       !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000)) {
                if (HWND dv = FocusedFileList()) {
                    g_defView = dv;
                    g_swallowSpaceUp = true;
                    PostMessage(g_wnd, WM_PEEK_OPEN, 0, 0);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PEEK_OPEN: LoadSelection(); return 0;
    case WM_PEEK_CLOSE: ClosePreview(); return 0;
    case WM_PEEK_REFRESH: SetTimer(h, kRefreshTimer, 120, nullptr); return 0; // deixa o Explorer mudar a seleção antes
    case WM_TIMER:
        KillTimer(h, kRefreshTimer);
        if (g_open) LoadSelection();
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: ClosePreview(); return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE && g_open) ClosePreview(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBuf = SelectObject(mem, buf);
        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        if (g_bmp) {
            HDC src = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(src, g_bmp);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            int x = (rc.right - g_bmpSize.cx) / 2;
            GdiAlphaBlend(mem, x, kCaption, g_bmpSize.cx, g_bmpSize.cy, src, 0, 0, g_bmpSize.cx, g_bmpSize.cy, bf);
            SelectObject(src, old);
            DeleteDC(src);
        }
        HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, kText);
        RECT tr = {kPad, 0, rc.right - kPad, kCaption};
        DrawTextW(mem, g_name.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, oldFont);
        DeleteObject(font);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBuf);
        DeleteObject(buf);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"/quit")) { // encerra a instância em execução
        if (HWND w = FindWindowW(L"DesignPeekWnd", nullptr)) PostMessage(w, WM_CLOSE, 0, 0);
        return 0;
    }
    CreateMutexW(nullptr, TRUE, L"DesignPeek.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = {};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DesignPeekWnd";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"DesignPeek", WS_POPUP,
                            0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    DWORD corner = 2; // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark));

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, inst, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    UnhookWindowsHookEx(g_hook);
    CoUninitialize();
    return 0;
}
