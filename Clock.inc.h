static void StartOcr();
// Clock.inc.h - DesktopClock integrado ao Vizu: relógio na área de trabalho, agenda, Pomodoro e Roubo de Tempo.
// Incluído dentro do Vizu.cpp (usa g_theme, g_dw, g_inst, T(), ReadDword...). Desligado até o usuário ativar.

static std::wstring ReadStr(const wchar_t* name, const wchar_t* def) {
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kCfgKey, name, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) return buf;
    return def;
}
static void WriteStr(const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kCfgKey, name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static std::wstring DataPath(const wchar_t* file) {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return L"";
    std::wstring d = std::wstring(dir) + L"\\DesignThumbs";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\" + file;
}

// Cores do Pomodoro: foco / pausa.
static const UINT kPomoPairs[3][2] = {{0x25D366, 0xE0245E}, {0x0A84FF, 0x8E8E93}, {0xB6FF3B, 0xA855F7}};
static const int kPomoTimes[3][3] = {{25, 5, 15}, {50, 10, 20}, {15, 3, 10}}; // foco, pausa, pausa longa (min)

struct ClockState {
    HWND wnd = nullptr;
    bool on = false;
    std::wstring font = L"Bahnschrift", app = L"https://chatgpt.com";
    DWORD fontPx = 38, pomoTimes = 0, pomoColors = 0;
    int phase = 0;            // 0 parado, 1 foco, 2 pausa, 3 pausa longa
    int done = 0;             // focos concluídos no ciclo
    bool paused = false;
    ULONGLONG phaseEnd = 0, phaseLen = 1, pausedLeft = 0;
    bool theft = false;
    ULONGLONG theftStart = 0; // GetTickCount64
    SYSTEMTIME theftWall = {};
    int hot = -1, lastSec = -1;
    D2D1_RECT_F rTri{}, rChip[4]{};
    float s = 1;
    bool reminderOpen = false;
    bool ocr = true;          // botão e atalho Ctrl+Alt+T do OCR
    bool theftOn = true, pomoOn = true, agendaOn = true; // recursos do relógio que o usuário pode desligar
    DWORD scalePct = 100;     // tamanho: 100, 90 ou 70%
    bool docked = false;      // anexado à barra de tarefas
    RECT tb{};                // barra de tarefas quando anexado
};
static ClockState g_clock;
enum { kClkTri = 0, kClkChip0 = 1, kClkChip1, kClkChip2, kClkChip3 };

static ULONGLONG ClockLeft() {
    if (!g_clock.phase) return 0;
    if (g_clock.paused) return g_clock.pausedLeft;
    ULONGLONG now = GetTickCount64();
    return g_clock.phaseEnd > now ? g_clock.phaseEnd - now : 0;
}

// ---------- agenda (%LOCALAPPDATA%\DesignThumbs\agenda.txt) ----------
// Uma linha por evento: "2026-11-06 16:00 d30 a10,5 Reunião" (d = duração em min, a = avisos em min antes;
// linhas antigas sem d/a valem 30 min com avisos de 10 e 5 min).

enum { kAlert10 = 1, kAlert5 = 2, kAlert0 = 4 };
struct AgendaItem { SYSTEMTIME when; std::wstring title; int dur = 30; int alerts = kAlert10 | kAlert5; };
static std::vector<AgendaItem> LoadAgenda() {
    std::vector<AgendaItem> r;
    FILE* f = _wfopen(DataPath(L"agenda.txt").c_str(), L"rt, ccs=UTF-8");
    if (!f) return r;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        AgendaItem a;
        a.when = {};
        int y, mo, d, h, mi, n = 0;
        if (swscanf(line, L"%d-%d-%d %d:%d %n", &y, &mo, &d, &h, &mi, &n) != 5 || n <= 0) continue;
        a.when.wYear = (WORD)y; a.when.wMonth = (WORD)mo; a.when.wDay = (WORD)d; a.when.wHour = (WORD)h; a.when.wMinute = (WORD)mi;
        const wchar_t* p = line + n;
        for (int k = 0; k < 2; ++k) { // tokens opcionais
            int v = 0, used = 0;
            if (p[0] == L'd' && swscanf(p + 1, L"%d %n", &v, &used) == 1 && used) { a.dur = max(5, min(600, v)); p += 1 + used; }
            else if (p[0] == L'a' && (iswdigit(p[1]) || p[1] == L'n')) {
                a.alerts = 0; ++p; if (*p == L'n') ++p;
                while (iswdigit(*p)) { int m = (int)wcstol(p, (wchar_t**)&p, 10); a.alerts |= m == 10 ? kAlert10 : m == 5 ? kAlert5 : m == 0 ? kAlert0 : 0; if (*p == L',') ++p; }
                while (*p == L' ') ++p;
            }
        }
        a.title = p;
        while (!a.title.empty() && (a.title.back() == L'\n' || a.title.back() == L'\r')) a.title.pop_back();
        if (a.title.empty()) a.title = T(L"(sem título)", L"(untitled)", L"(sin título)");
        r.push_back(a);
    }
    fclose(f);
    return r;
}
static void SaveAgenda(const std::vector<AgendaItem>& v) {
    FILE* f = _wfopen(DataPath(L"agenda.txt").c_str(), L"wt, ccs=UTF-8");
    if (!f) return;
    for (auto& a : v) {
        std::wstring al;
        if (a.alerts & kAlert10) al += L"10";
        if (a.alerts & kAlert5) al += std::wstring(al.empty() ? L"" : L",") + L"5";
        if (a.alerts & kAlert0) al += std::wstring(al.empty() ? L"" : L",") + L"0";
        if (al.empty()) al = L"n"; // sem avisos
        fwprintf(f, L"%04d-%02d-%02d %02d:%02d d%d a%ls %ls\n", a.when.wYear, a.when.wMonth, a.when.wDay, a.when.wHour, a.when.wMinute, a.dur, al.c_str(), a.title.c_str());
    }
    fclose(f);
}
static LONGLONG Minutes(const SYSTEMTIME& t) { FILETIME f; SystemTimeToFileTime(&t, &f); return (LONGLONG)(((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 600000000ull); }
static bool AgendaSoon(std::wstring* next) { // evento na próxima hora?
    SYSTEMTIME now; GetLocalTime(&now);
    LONGLONG m = Minutes(now), best = LLONG_MAX;
    static std::vector<AgendaItem> cache; static ULONGLONG at = 0; // relê o arquivo no máximo a cada 10 s
    if (!at || GetTickCount64() - at > 10000) { cache = LoadAgenda(); at = GetTickCount64(); }
    for (auto& a : cache) {
        LONGLONG d = Minutes(a.when) - m;
        if (d >= 0 && d <= 60 && d < best) { best = d; if (next) { wchar_t b[16]; swprintf(b, 16, L"%02d:%02d ", a.when.wHour, a.when.wMinute); *next = b + a.title; } }
    }
    return best != LLONG_MAX;
}
static void OpenAgendaWindow();
static void CheckReminders();

// ---------- Roubo de Tempo (%LOCALAPPDATA%\DesignThumbs\roubo-de-tempo.csv: data;hora;segundos) ----------

struct Theft { int y, m, d, h, mi, sec; };
static std::vector<Theft> LoadThefts() {
    std::vector<Theft> r;
    FILE* f = _wfopen(DataPath(L"roubo-de-tempo.csv").c_str(), L"rt");
    if (!f) return r;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        Theft t = {};
        int ss = 0;
        if (sscanf(line, "%d-%d-%d;%d:%d:%d;%d", &t.y, &t.m, &t.d, &t.h, &t.mi, &ss, &t.sec) == 7) r.push_back(t);
    }
    fclose(f);
    return r;
}
static void ToggleTheft() {
    if (!g_clock.theft) {
        g_clock.theft = true; g_clock.theftStart = GetTickCount64(); GetLocalTime(&g_clock.theftWall);
        return;
    }
    g_clock.theft = false;
    int sec = (int)((GetTickCount64() - g_clock.theftStart) / 1000);
    if (sec < 1) return;
    std::wstring p = DataPath(L"roubo-de-tempo.csv");
    bool fresh = GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (FILE* f = _wfopen(p.c_str(), L"at")) {
        if (fresh) fprintf(f, "data;inicio;segundos\n");
        const SYSTEMTIME& w = g_clock.theftWall;
        fprintf(f, "%04d-%02d-%02d;%02d:%02d:%02d;%d\n", w.wYear, w.wMonth, w.wDay, w.wHour, w.wMinute, w.wSecond, sec);
        fclose(f);
    }
}
// Histórico do roubo de tempo (protótipo 4 · Relógio): cartão com o total de hoje, barras dos últimos dias,
// as interrupções mais recentes e os botões Exportar CSV e Ver semana.
struct TheftView {
    HWND wnd = nullptr;
    ui::Surface surf;
    bool week = false;
    int hot = 0; // 1 exportar, 2 ver semana
    D2D1_RECT_F btn[3] = {};
};
static TheftView g_tv;
static int DaySum(const std::vector<Theft>& all, const SYSTEMTIME& d, int* count = nullptr) {
    int s = 0, n = 0;
    for (auto& t : all) if (t.y == d.wYear && t.m == d.wMonth && t.d == d.wDay) { s += t.sec; ++n; }
    if (count) *count = n;
    return s;
}
static SYSTEMTIME DaysAgo(int k) {
    SYSTEMTIME now; GetLocalTime(&now);
    FILETIME ft; SystemTimeToFileTime(&now, &ft);
    ULONGLONG v = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - (ULONGLONG)k * 864000000000ull;
    FILETIME f2 = {(DWORD)v, (DWORD)(v >> 32)}; SYSTEMTIME d; FileTimeToSystemTime(&f2, &d);
    return d;
}
static std::wstring Clock2(int sec) { wchar_t b[24]; if (sec >= 3600) swprintf(b, 24, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60); else swprintf(b, 24, L"%02d:%02d", sec / 60, sec % 60); return b; }
static int TheftRows() {
    if (g_tv.week) return 7;
    SYSTEMTIME now; GetLocalTime(&now);
    int n = 0; DaySum(LoadThefts(), now, &n);
    return n == 0 ? 1 : n <= 4 ? n : 4;
}
static float TheftHeight() { return 106 + TheftRows() * 30 + 14 + 34 + 20; }
static void TheftExport() {
    std::wstring src = DataPath(L"roubo-de-tempo.csv");
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) { MessageBoxW(g_tv.wnd, T(L"Ainda não há registros para exportar.", L"There are no records to export yet.", L"Todavía no hay registros para exportar."), L"Vizu", MB_ICONINFORMATION); return; }
    wchar_t file[MAX_PATH] = L"roubo-de-tempo.csv";
    OPENFILENAMEW of = {sizeof(of)};
    of.hwndOwner = g_tv.wnd; of.lpstrFile = file; of.nMaxFile = MAX_PATH; of.lpstrDefExt = L"csv";
    of.lpstrFilter = L"CSV (*.csv)\0*.csv\0";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&of) && !CopyFileW(src.c_str(), file, FALSE))
        MessageBoxW(g_tv.wnd, T(L"Não foi possível salvar o arquivo.", L"Couldn't save the file.", L"No se pudo guardar el archivo."), L"Vizu", MB_ICONWARNING);
}
static void PaintTheft(HWND h) {
    g_tv.surf.hwnd = h;
    if (!g_tv.surf.Begin(g_d2d)) return;
    float s = WndScale(h);
    ID2D1RenderTarget* rt = g_tv.surf.rt;
    D2D1_SIZE_F sz = rt->GetSize();
    const ui::Theme& th = g_theme;
    bool classic = th.classic;
    D2D1_COLOR_F bg = classic ? ui::Rgba(0x4A4F57) : th.tint, ink = classic ? ui::Rgba(0xFFFFFF) : th.text, ink2 = classic ? ui::Rgba(0xCDD2DA) : th.text2,
                 track = classic ? ui::Rgba(0x5C6472) : th.track, sep = classic ? ui::Rgba(0x5C6472) : th.stroke, red = ui::Rgba(0xFF3B5C);
    rt->Clear(bg);
    ui::Painter p(rt, g_dw, th, s);
    if (!classic) p.Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 12 * s, th.stroke);
    auto R = [&](float x0, float y0, float x1, float y1) { return D2D1::RectF(x0 * s, y0 * s, x1 * s, y1 * s); };
    float W = sz.width / s, L = 20, Rt = W - 20;
    auto all = LoadThefts();
    SYSTEMTIME today; GetLocalTime(&today);
    int n = 0, total = DaySum(all, today, &n);
    int weekTotal = 0; for (int k = 0; k < 7; ++k) weekTotal += DaySum(all, DaysAgo(k));
    wchar_t head[96];
    if (g_tv.week) swprintf(head, 96, T(L"Últimos 7 dias · total perdido", L"Last 7 days · total lost", L"Últimos 7 días · total perdido"));
    else swprintf(head, 96, n == 1 ? T(L"Total perdido hoje · %d interrupção", L"Total lost today · %d interruption", L"Total perdido hoy · %d interrupción") : T(L"Total perdido hoje · %d interrupções", L"Total lost today · %d interruptions", L"Total perdido hoy · %d interrupciones"), n);
    p.Text(head, R(L, 18, Rt - 110, 36), 12, ink2);
    // total grande: número em 40 px, unidade em 18 px
    int big = g_tv.week ? weekTotal : total;
    std::wstring parts[4];
    if (big >= 3600) { parts[0] = std::to_wstring(big / 3600); parts[1] = L" h "; wchar_t b[8]; swprintf(b, 8, L"%02d", big / 60 % 60); parts[2] = b; parts[3] = L" min"; }
    else { parts[0] = std::to_wstring(big / 60); parts[1] = L" min "; parts[2] = std::to_wstring(big % 60); parts[3] = L" s"; }
    float x = L;
    for (int i = 0; i < 4; ++i) {
        bool num = i % 2 == 0;
        float sz2 = num ? 40.0f : 18.0f;
        float w = p.Measure(parts[i], sz2, num ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL) / s;
        p.Text(parts[i], R(x, num ? 36 : 52, x + w + 2, num ? 84 : 80), sz2, num ? ink : ink2, num ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        x += w;
    }
    // barras dos últimos 5 dias (hoje à direita, em vermelho)
    int days[5], mx = 1;
    for (int k = 0; k < 5; ++k) { days[k] = DaySum(all, DaysAgo(4 - k)); mx = max(mx, days[k]); }
    for (int k = 0; k < 5; ++k) {
        float bx = Rt - (5 - k) * 20 + 6, hh = max(4.0f, 56.0f * days[k] / mx);
        p.Fill(R(bx, 82 - hh, bx + 14, 82), 4 * s, k == 4 ? red : track);
    }
    p.Fill(R(L, 96, Rt, 97), 0, sep);
    // lista: interrupções de hoje (as 3 mais recentes + o resto somado) ou os 7 dias
    float y = 104;
    auto row = [&](const std::wstring& a, const std::wstring& b, bool muted) {
        D2D1_COLOR_F c = muted ? ink2 : ink;
        p.Text(a, R(L + 4, y, Rt, y + 30), 13, c);
        if (!muted) p.Text(L"—", R(L, y, Rt, y + 30), 13, ink2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        p.Text(b, R(L, y, Rt - 4, y + 30), 13, c, muted ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 30;
    };
    if (g_tv.week) {
        static const wchar_t* wdPt[7] = {L"dom", L"seg", L"ter", L"qua", L"qui", L"sex", L"sáb"}, *wdEn[7] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"}, *wdEs[7] = {L"dom", L"lun", L"mar", L"mié", L"jue", L"vie", L"sáb"};
        for (int k = 0; k < 7; ++k) {
            SYSTEMTIME d = DaysAgo(k);
            int sm = DaySum(all, d);
            wchar_t a[32]; swprintf(a, 32, L"%ls %02d/%02d", k == 0 ? T(L"hoje", L"today", L"hoy") : T(wdPt[d.wDayOfWeek], wdEn[d.wDayOfWeek], wdEs[d.wDayOfWeek]), d.wDay, d.wMonth);
            row(a, sm ? Clock2(sm) : L"—", sm == 0);
        }
    } else if (n == 0) row(T(L"Nenhum roubo de tempo hoje.", L"No time theft today.", L"Ningún robo de tiempo hoy."), L"", true);
    else {
        int shown = 0, rest = 0;
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->y != today.wYear || it->m != today.wMonth || it->d != today.wDay) continue;
            if (shown < (n <= 4 ? 4 : 3)) { wchar_t a[16]; swprintf(a, 16, L"%02d:%02d", it->h, it->mi); row(a, Clock2(it->sec), false); ++shown; }
            else rest += it->sec;
        }
        if (n > 4) { wchar_t a[32]; swprintf(a, 32, T(L"+ %d mais", L"+ %d more", L"+ %d más"), n - 3); row(a, Clock2(rest), true); }
    }
    // botões
    y += 14;
    float bw = (Rt - L - 8) / 2;
    const wchar_t* labels[3] = {nullptr, T(L"Exportar CSV", L"Export CSV", L"Exportar CSV"), g_tv.week ? T(L"Ver hoje", L"Show today", L"Ver hoy") : T(L"Ver semana", L"Show week", L"Ver semana")};
    for (int i = 1; i <= 2; ++i) {
        float bx = L + (i - 1) * (bw + 8);
        D2D1_RECT_F r = R(bx, y, bx + bw, y + 34);
        g_tv.btn[i] = r;
        if (g_tv.hot == i) p.Fill(r, 17 * s, classic ? ui::Rgba(0xFFFFFF, 0.10f) : th.panelHot);
        p.Stroke(r, 17 * s, classic ? ui::Rgba(0x9AA1AD) : th.stroke);
        p.Text(labels[i], r, 12.5f, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    g_tv.surf.End();
}
static void TheftResize() {
    if (!g_tv.wnd) return;
    float s = WndScale(g_tv.wnd);
    RECT r; GetWindowRect(g_tv.wnd, &r);
    SetWindowPos(g_tv.wnd, nullptr, 0, 0, r.right - r.left, (int)(TheftHeight() * s), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_tv.wnd, nullptr, FALSE);
}
static LRESULT CALLBACK TheftProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintTheft(h); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g_tv.surf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_NCHITTEST: { LRESULT r = DefWindowProcW(h, msg, wp, lp); if (r != HTCLIENT) return r; POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &pt); return pt.y < 96 * WndScale(h) ? HTCAPTION : HTCLIENT; }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = ui::Hit(g_tv.btn[1], pt) ? 1 : ui::Hit(g_tv.btn[2], pt) ? 2 : 0;
        if (hot != g_tv.hot) { g_tv.hot = hot; TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: g_tv.hot = 0; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, g_tv.hot ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_LBUTTONUP:
        if (g_tv.hot == 1) TheftExport();
        else if (g_tv.hot == 2) { g_tv.week = !g_tv.week; TheftResize(); }
        return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE && !IsWindowEnabled(h)) return 0; if (LOWORD(wp) == WA_INACTIVE && (HWND)lp != nullptr && GetWindow((HWND)lp, GW_OWNER) != h) DestroyWindow(h); return 0;
    case WM_DESTROY: g_tv.surf.Release(); g_tv.wnd = nullptr; g_tv.hot = 0; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void ShowTheftHistory() {
    if (g_tv.wnd) { SetForegroundWindow(g_tv.wnd); return; }
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = TheftProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"VizuTheft"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc); reg = true; }
    g_tv.week = false;
    float s = GetDpiForSystem() / 96.0f;
    int w = (int)(380 * s), hgt = (int)(TheftHeight() * s);
    RECT cr = {}; if (g_clock.wnd) GetWindowRect(g_clock.wnd, &cr);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(g_clock.wnd ? g_clock.wnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = max((int)mi.rcWork.left + 8, min((int)mi.rcWork.right - w - 8, (int)cr.left));
    int y = cr.top - hgt - 8 >= mi.rcWork.top ? cr.top - hgt - 8 : min((int)mi.rcWork.bottom - hgt - 8, (int)cr.bottom + 8);
    g_tv.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"VizuTheft", T(L"Roubo de tempo", L"Time theft", L"Robo de tiempo"), WS_POPUP, x, y, w, hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_tv.wnd) return;
    BOOL round = 2; DwmSetWindowAttribute(g_tv.wnd, 33, &round, sizeof(round));
    ShowWindow(g_tv.wnd, SW_SHOW); SetForegroundWindow(g_tv.wnd); SetFocus(g_tv.wnd);
}

// ---------- Pomodoro ----------

static void PomoStart(int phase) {
    int mins = kPomoTimes[min(2ul, (unsigned long)g_clock.pomoTimes)][phase - 1];
    g_clock.phase = phase; g_clock.paused = false;
    g_clock.phaseLen = (ULONGLONG)mins * 60000; g_clock.phaseEnd = GetTickCount64() + g_clock.phaseLen;
}
static void PomoStop() { g_clock.phase = 0; g_clock.paused = false; g_clock.done = 0; }
static void PomoTick() {
    if (!g_clock.phase || g_clock.paused || ClockLeft() > 0) return;
    MessageBeep(MB_ICONASTERISK);
    if (g_clock.phase == 1) { ++g_clock.done; PomoStart(g_clock.done % 4 == 0 ? 3 : 2); }
    else PomoStart(1);
}
static void PomoPauseResume() {
    if (!g_clock.phase) { g_clock.done = 0; PomoStart(1); return; }
    if (g_clock.paused) { g_clock.paused = false; g_clock.phaseEnd = GetTickCount64() + g_clock.pausedLeft; }
    else { g_clock.pausedLeft = ClockLeft(); g_clock.paused = true; }
}

// Barra de tarefas horizontal (para anexar o relógio). false se estiver na vertical ou não for encontrada.
static bool TaskbarRect(RECT* r) {
    APPBARDATA ab = {sizeof(ab)};
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &ab)) return false;
    *r = ab.rc;
    return (r->right - r->left) > (r->bottom - r->top) && (r->bottom - r->top) >= 24;
}
static bool TaskbarDark() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

// ---------- desenho ----------

static void ClockRender() {
    HWND w = g_clock.wnd;
    if (!w || !g_clock.on) return;
    ui::Theme th = g_theme;
    float s = GetDpiForSystem() / 96.0f * (float)g_clock.scalePct / 100.0f;
    bool docked = g_clock.docked && TaskbarRect(&g_clock.tb);
    if (docked) { // cabe na altura da barra de tarefas e segue a cor dela
        s = (g_clock.tb.bottom - g_clock.tb.top) * 0.82f / 64.0f;
        bool dk = TaskbarDark();
        th.dark = dk;
        th.text = dk ? ui::Rgba(0xFFFFFF, 0.95f) : ui::Rgba(0x1D1D1F);
        th.text2 = dk ? ui::Rgba(0xFFFFFF, 0.6f) : ui::Rgba(0x000000, 0.55f);
        th.icon = th.text;
    }
    g_clock.s = s;
    float H = 64 * s, M = docked ? 0 : 14 * s, fpx = (float)g_clock.fontPx;
    bool pomo = g_clock.phase != 0;
    UINT pc = kPomoPairs[min(2ul, (unsigned long)g_clock.pomoColors)][g_clock.phase == 1 ? 0 : 1];

    // largura fixa dos dígitos (nada pula quando o ":" pisca)
    float w2 = fpx * 1.2f * s, wc = fpx * 0.36f * s;
    {
        IDWriteTextFormat* f = nullptr;
        if (SUCCEEDED(g_dw->CreateTextFormat(g_clock.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fpx * s, L"pt-br", &f))) {
            auto measure = [&](const wchar_t* t) { IDWriteTextLayout* l = nullptr; DWRITE_TEXT_METRICS m = {}; if (SUCCEEDED(g_dw->CreateTextLayout(t, (UINT32)wcslen(t), f, 4000, 400, &l))) { l->GetMetrics(&m); l->Release(); } return m.widthIncludingTrailingWhitespace; };
            float a = measure(L"00"), b = measure(L"88"), c = measure(L":");
            if (a > 0) w2 = max(a, b) + 2 * s;
            if (c > 0) wc = c + 2 * s;
            f->Release();
        }
    }
    float timeW = 2 * w2 + wc, chip = 40 * s, gap = 6 * s, theftW = g_clock.theft ? 104 * s : chip;
    bool showTri = pomo || g_clock.agendaOn;
    float Wc = 14 * s + (showTri ? 20 * s + 12 * s : 0) + timeW + 12 * s + (g_clock.theftOn ? theftW + gap : 0) + (g_clock.pomoOn ? chip + gap : 0) + chip + (g_clock.ocr ? gap + chip : 0) + 10 * s;
    UINT W = (UINT)(Wc + 2 * M), Ht = (UINT)(H + 2 * M);
    fmt::Canvas cv(W, Ht);
    if (!cv.rt) return;
    ID2D1RenderTarget* rt = cv.rt;
    rt->Clear(D2D1::ColorF(0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ui::Painter p(rt, g_dw, th, s);
    D2D1_RECT_F card = D2D1::RectF(M, M, M + Wc, M + H);
    float rad = 22 * s;
    if (!docked) p.Shadow(card, rad);
    D2D1_COLOR_F base = docked ? ui::Rgba(0x000000, 0.006f) // quase invisível: a barra de tarefas aparece por trás (e o clique funciona)
                       : th.classic ? ui::Rgba(0x4A4F57) : th.neon ? ui::Rgba(0x0E0E10, 0.94f) : th.dark ? ui::Rgba(0x161618, 0.94f) : ui::Rgba(0xFAFAFC, 0.95f);
    p.Fill(card, rad, base);
    if (pomo) {
        p.Fill(card, rad, ui::Rgba(pc, 0.15f));
        if (g_clock.phase == 1 && !g_clock.paused)
            for (int i = 3; i >= 1; --i) { float g = i * 2.2f * s; p.Stroke(D2D1::RectF(card.left - g, card.top - g, card.right + g, card.bottom + g), rad + g, ui::Rgba(pc, 0.08f * (4 - i)), 1.6f * s); }
        p.Stroke(card, rad, ui::Rgba(pc, 0.9f), 1 * s);
        // barra de progresso embaixo, recortada pelo cartão
        float prog = 1.0f - (float)ClockLeft() / (float)max(1ull, g_clock.phaseLen);
        ID2D1RoundedRectangleGeometry* clip = nullptr; ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
        if (SUCCEEDED(fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(card, rad, rad), &clip))) {
            ID2D1Layer* layer = nullptr;
            if (SUCCEEDED(rt->CreateLayer(&layer))) {
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
                p.Fill(D2D1::RectF(card.left, card.bottom - 3 * s, card.left + (card.right - card.left) * prog, card.bottom), 0, ui::Rgba(pc));
                rt->PopLayer(); layer->Release();
            }
            clip->Release();
        }
        fac->Release();
    } else if (!docked) p.Stroke(card, rad, th.neon ? ui::Rgba(ui::NeonColor(), 0.42f) : th.stroke, 1 * s);

    float x = card.left + 14 * s, cy = (card.top + card.bottom) / 2;
    D2D1_COLOR_F hotBg = th.dark ? ui::Rgba(0xFFFFFF, 0.14f) : ui::Rgba(0x000000, 0.09f), chipBg = th.classic ? ui::Rgba(0x23272E) : th.dark ? ui::Rgba(0xFFFFFF, 0.07f) : ui::Rgba(0x000000, 0.05f);
    if (th.classic) hotBg = ui::Rgba(0x5C6472);
    // 1) ícone da esquerda: agenda (triângulo) ou fase do Pomodoro
    g_clock.rTri = D2D1::RectF(0, 0, 0, 0);
    if (showTri) {
        g_clock.rTri = D2D1::RectF(x - 6 * s, cy - 16 * s, x + 26 * s, cy + 16 * s);
        if (g_clock.hot == kClkTri) p.Fill(g_clock.rTri, 10 * s, hotBg);
        if (pomo) p.FeatherIcon(g_clock.phase == 1 ? "target" : "coffee", D2D1::RectF(x, cy - 10 * s, x + 20 * s, cy + 10 * s), 19 * s, ui::Rgba(pc));
        else p.FeatherIcon("alert-triangle", D2D1::RectF(x, cy - 10 * s, x + 20 * s, cy + 10 * s), 19 * s, AgendaSoon(nullptr) ? ui::Rgba(0xFF3B30) : th.text2);
        x += 20 * s + 12 * s;
    }

    // 2) hora, ou contagem do Pomodoro
    SYSTEMTIME now; GetLocalTime(&now);
    int a, b;
    if (pomo) { ULONGLONG left = (ClockLeft() + 999) / 1000; a = (int)(left / 60); b = (int)(left % 60); }
    else { a = now.wHour; b = now.wMinute; }
    D2D1_COLOR_F dig = th.neon ? ui::Rgba(ui::NeonColor()) : th.text;
    float ty = cy, tsz = fpx;
    if (pomo) { // rótulo pequeno acima
        const wchar_t* lab = g_clock.phase == 1 ? T(L"FOCO", L"FOCUS", L"ENFOQUE") : g_clock.phase == 2 ? T(L"PAUSA", L"BREAK", L"PAUSA") : T(L"PAUSA LONGA", L"LONG BREAK", L"PAUSA LARGA");
        wchar_t lb[48]; swprintf(lb, 48, g_clock.phase == 1 ? L"%ls · %d/4%ls" : L"%ls%ls%ls", lab, g_clock.phase == 1 ? g_clock.done % 4 + 1 : 0, g_clock.paused ? L"  ‖" : L"");
        if (g_clock.phase != 1) swprintf(lb, 48, L"%ls%ls", lab, g_clock.paused ? L"  ‖" : L"");
        p.Text(lb, D2D1::RectF(x, card.top + 8 * s, x + timeW, card.top + 20 * s), 9.5f, ui::Rgba(pc), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        ty = cy + 6 * s; tsz = fpx * 0.82f;
    }
    {
        IDWriteTextFormat* f = nullptr;
        if (SUCCEEDED(g_dw->CreateTextFormat(g_clock.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, tsz * s, L"pt-br", &f))) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ID2D1SolidColorBrush* br = nullptr; rt->CreateSolidColorBrush(dig, &br);
            wchar_t t1[4], t2[4]; swprintf(t1, 4, L"%02d", a % 100); swprintf(t2, 4, L"%02d", b);
            float hh = H;
            rt->DrawText(t1, 2, f, D2D1::RectF(x, ty - hh / 2, x + w2, ty + hh / 2), br);
            D2D1_COLOR_F cc = dig; cc.a = (now.wSecond % 2 == 0 || pomo) ? 0.55f : 0.18f; // ":" pisca a cada segundo
            br->SetColor(cc);
            rt->DrawText(L":", 1, f, D2D1::RectF(x + w2, ty - hh / 2 - 2 * s, x + w2 + wc, ty + hh / 2 - 2 * s), br);
            br->SetColor(dig);
            rt->DrawText(t2, 2, f, D2D1::RectF(x + w2 + wc, ty - hh / 2, x + timeW, ty + hh / 2), br);
            br->Release(); f->Release();
        }
    }
    x += timeW + 12 * s;

    // 3) três botões: caveira (roubo de tempo), Pomodoro/pausa, atalho/parar
    auto chipRect = [&](float wdt) { D2D1_RECT_F r = D2D1::RectF(x, cy - chip / 2, x + wdt, cy + chip / 2); x += wdt + gap; return r; };
    for (auto& r : g_clock.rChip) r = D2D1::RectF(0, 0, 0, 0);
    if (g_clock.theftOn) g_clock.rChip[0] = chipRect(theftW);
    if (!g_clock.theftOn) {}
    else if (g_clock.theft) {
        D2D1_RECT_F r = g_clock.rChip[0];
        for (int i = 3; i >= 1; --i) { float g = i * 1.8f * s; p.Stroke(D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g), chip / 2 + g, ui::Rgba(0xFF3B5C, 0.09f * (4 - i)), 1.4f * s); }
        p.Fill(r, chip / 2, ui::Rgba(0xFF3B5C, g_clock.hot == kClkChip0 ? 0.26f : 0.16f));
        p.Stroke(r, chip / 2, ui::Rgba(0xFF3B5C), 1 * s);
        p.FeatherIcon("skull", D2D1::RectF(r.left + 8 * s, r.top, r.left + 30 * s, r.bottom), 19 * s, ui::Rgba(0xFF3B5C));
        int sec = (int)((GetTickCount64() - g_clock.theftStart) / 1000);
        wchar_t tb[16]; if (sec >= 3600) swprintf(tb, 16, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60); else swprintf(tb, 16, L"%02d:%02d", sec / 60, sec % 60);
        p.Text(tb, D2D1::RectF(r.left + 30 * s, r.top, r.right - 10 * s, r.bottom), 13, ui::Rgba(0xFF7A8F), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        p.Fill(g_clock.rChip[0], chip / 2, g_clock.hot == kClkChip0 ? hotBg : chipBg);
        p.FeatherIcon("skull", g_clock.rChip[0], 18 * s, th.icon);
    }
    if (g_clock.pomoOn) g_clock.rChip[1] = chipRect(chip);
    g_clock.rChip[2] = chipRect(chip);
    if (pomo) {
        p.Fill(g_clock.rChip[1], chip / 2, ui::Rgba(pc, g_clock.hot == kClkChip1 ? 0.8f : 1.0f));
        UINT on = (pc >> 16 & 255) * 3 + (pc >> 8 & 255) * 6 + (pc & 255) > 1700 ? 0x0B0B0C : 0xFFFFFF; // texto legível sobre a cor
        p.FeatherIcon(g_clock.paused ? "play" : "pause", g_clock.rChip[1], 16 * s, ui::Rgba(on));
        p.Fill(g_clock.rChip[2], chip / 2, g_clock.hot == kClkChip2 ? hotBg : chipBg);
        p.FeatherIcon("square", g_clock.rChip[2], 16 * s, th.icon);
    } else {
        if (g_clock.pomoOn) { p.Fill(g_clock.rChip[1], chip / 2, g_clock.hot == kClkChip1 ? hotBg : chipBg); p.FeatherIcon("timer", g_clock.rChip[1], 18 * s, th.icon); }
        p.Fill(g_clock.rChip[2], chip / 2, g_clock.hot == kClkChip2 ? hotBg : chipBg);
        p.FeatherIcon("message-circle", g_clock.rChip[2], 18 * s, th.icon);
    }
    if (g_clock.ocr) { // ler texto da tela (OCR)
        g_clock.rChip[3] = chipRect(chip);
        p.Fill(g_clock.rChip[3], chip / 2, g_clock.hot == kClkChip3 ? hotBg : chipBg);
        p.FeatherIcon("scan-text", g_clock.rChip[3], 18 * s, th.icon);
    }

    HBITMAP bmp = nullptr;
    if (FAILED(cv.Finish(&bmp)) || !bmp) return;
    RECT wr; GetWindowRect(w, &wr);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT pos = {wr.left, wr.top}, zero = {0, 0};
    if (docked) { // trilho da barra de tarefas: centralizado na altura, dentro da largura
        RECT& t = g_clock.tb;
        pos.y = t.top + ((t.bottom - t.top) - (LONG)Ht) / 2;
        pos.x = max(t.left + 4, min(t.right - (LONG)W - 4, wr.left));
    }
    SIZE size = {(LONG)W, (LONG)Ht};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(w, screen, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteDC(mem); ReleaseDC(nullptr, screen); DeleteObject(bmp);
}

static int ClockHit(POINT pt) {
    auto in = [&](const D2D1_RECT_F& r) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; };
    if (in(g_clock.rTri)) return kClkTri;
    for (int i = 0; i < 4; ++i) if (in(g_clock.rChip[i])) return kClkChip0 + i;
    return -1;
}

static void ClockDefaultPos() { // canto inferior direito, acima da barra de tarefas
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wr; GetWindowRect(g_clock.wnd, &wr);
    SetWindowPos(g_clock.wnd, nullptr, mi.rcWork.right - (wr.right - wr.left) - 8, mi.rcWork.bottom - (wr.bottom - wr.top) - 8, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool PickFont(HWND owner, std::wstring* face, DWORD* px) {
    LOGFONTW lf = {};
    wcsncpy(lf.lfFaceName, face->c_str(), LF_FACESIZE - 1);
    HDC dc = GetDC(nullptr);
    lf.lfHeight = px ? -MulDiv((int)*px, GetDeviceCaps(dc, LOGPIXELSY), 96) : -16;
    lf.lfWeight = FW_SEMIBOLD;
    CHOOSEFONTW cf = {sizeof(cf)};
    cf.hwndOwner = owner; cf.lpLogFont = &lf; cf.hDC = dc;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | (px ? 0 : CF_NOSIZESEL) | CF_NOVERTFONTS;
    bool ok = ChooseFontW(&cf) != FALSE;
    if (ok) {
        *face = lf.lfFaceName;
        if (px) *px = max(18ul, min(96ul, (unsigned long)MulDiv(abs(lf.lfHeight), 96, GetDeviceCaps(dc, LOGPIXELSY))));
    }
    ReleaseDC(nullptr, dc);
    return ok;
}

static void ClockSave() {
    WriteDword(L"ClockOn", g_clock.on); WriteStr(L"ClockFont", g_clock.font); WriteDword(L"ClockSize", g_clock.fontPx);
    WriteDword(L"PomoTimes", g_clock.pomoTimes); WriteDword(L"PomoColors", g_clock.pomoColors); WriteStr(L"ClockApp", g_clock.app);
}

static void ClockMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, T(L"Agenda…", L"Agenda…", L"Agenda…"));
    AppendMenuW(m, MF_STRING, 2, T(L"Histórico do roubo de tempo…", L"Time theft history…", L"Historial del robo de tiempo…"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, g_clock.phase ? T(L"Parar Pomodoro", L"Stop Pomodoro", L"Detener Pomodoro") : T(L"Iniciar Pomodoro", L"Start Pomodoro", L"Iniciar Pomodoro"));
    if (g_clock.phase) AppendMenuW(m, MF_STRING, 4, T(L"Pular fase", L"Skip phase", L"Saltar fase"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, T(L"Fonte do relógio…", L"Clock font…", L"Fuente del reloj…"));
    AppendMenuW(m, MF_STRING, 6, T(L"Aplicativo do botão…", L"Button app…", L"Aplicación del botón…"));
    AppendMenuW(m, MF_STRING | (g_clock.docked ? MF_CHECKED : 0), 10, T(L"Anexar à barra de tarefas", L"Attach to the taskbar", L"Anclar a la barra de tareas"));
    HMENU sz = CreatePopupMenu();
    for (int i = 0; i < 3; ++i) { static const int pc[3] = {100, 90, 70}; wchar_t b[8]; swprintf(b, 8, L"%d%%", pc[i]); AppendMenuW(sz, MF_STRING | ((int)g_clock.scalePct == pc[i] ? MF_CHECKED : 0), 20 + i, b); }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, T(L"Tamanho", L"Size", L"Tamaño"));
    if (!g_clock.docked) AppendMenuW(m, MF_STRING, 7, T(L"Voltar ao canto", L"Back to the corner", L"Volver a la esquina"));
    AppendMenuW(m, MF_STRING, 8, T(L"Configurações", L"Settings", L"Configuración"));
    AppendMenuW(m, MF_STRING, 9, T(L"Ocultar relógio", L"Hide clock", L"Ocultar reloj"));
    SetForegroundWindow(g_clock.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_clock.wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
    case 1: OpenAgendaWindow(); break;
    case 2: ShowTheftHistory(); break;
    case 3: if (g_clock.phase) PomoStop(); else PomoPauseResume(); break;
    case 4: if (g_clock.phase == 1) { ++g_clock.done; PomoStart(g_clock.done % 4 == 0 ? 3 : 2); } else PomoStart(1); break;
    case 5: if (PickFont(g_clock.wnd, &g_clock.font, &g_clock.fontPx)) ClockSave(); break;
    case 6: {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of = {sizeof(of)};
        of.hwndOwner = g_clock.wnd; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.lpstrFilter = L"Aplicativos (*.exe)\0*.exe\0Todos (*.*)\0*.*\0";
        of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&of)) { g_clock.app = file; ClockSave(); }
        else if (MessageBoxW(g_clock.wnd, T(L"Voltar a abrir o ChatGPT no botão?", L"Go back to opening ChatGPT from the button?", L"¿Volver a abrir ChatGPT con el botón?"), L"Vizu", MB_YESNO | MB_ICONQUESTION) == IDYES) { g_clock.app = L"https://chatgpt.com"; ClockSave(); }
        break;
    }
    case 7: ClockDefaultPos(); break;
    case 10: { void ClockDock(bool); ClockDock(!g_clock.docked); return; }
    case 20: case 21: case 22: { static const int pc[3] = {100, 90, 70}; g_clock.scalePct = pc[c - 20]; WriteDword(L"ClockScale", g_clock.scalePct); break; }
    case 8: OpenSettings(); break;
    case 9: { void ClockEnable(bool); ClockEnable(false); return; }
    }
    ClockRender();
}

static LRESULT CALLBACK ClockProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        PomoTick();
        SYSTEMTIME t; GetLocalTime(&t);
        if (t.wSecond != g_clock.lastSec) {
            g_clock.lastSec = t.wSecond;
            if (g_clock.docked) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // fica por cima da barra
            ClockRender();
            if (g_clock.agendaOn) CheckReminders();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = ClockHit(pt);
        if (hot != g_clock.hot) { g_clock.hot = hot; ClockRender(); }
        TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, h, 0}; TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE: g_clock.hot = -1; ClockRender(); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_clock.hot >= 0 ? IDC_HAND : IDC_SIZEALL)); return TRUE;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = ClockHit(pt);
        if (hit < 0) { ReleaseCapture(); SendMessage(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0; } // arrastar
        if (hit == kClkTri) {
            if (g_clock.agendaOn) OpenAgendaWindow();
        } else if (hit == kClkChip0) ToggleTheft();
        else if (hit == kClkChip1) PomoPauseResume();
        else if (hit == kClkChip2) {
            if (g_clock.phase) PomoStop();
            else ShellExecuteW(nullptr, L"open", g_clock.app.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (hit == kClkChip3) { StartOcr(); return 0; }
        ClockRender();
        return 0;
    }
    case WM_RBUTTONUP: { POINT pt; GetCursorPos(&pt); ClockMenu(pt); return 0; }
    case WM_MOVING:
        if (g_clock.docked && TaskbarRect(&g_clock.tb)) {
            RECT* r = (RECT*)lp; RECT& t = g_clock.tb;
            LONG ww = r->right - r->left, hh = r->bottom - r->top;
            r->top = t.top + ((t.bottom - t.top) - hh) / 2; r->bottom = r->top + hh;
            if (r->left < t.left + 4) { r->left = t.left + 4; r->right = r->left + ww; }
            if (r->right > t.right - 4) { r->right = t.right - 4; r->left = r->right - ww; }
            return TRUE;
        }
        break;
    case WM_EXITSIZEMOVE: {
        RECT r; GetWindowRect(h, &r);
        if (g_clock.docked) WriteDword(L"DockX", (DWORD)r.left);
        else { WriteDword(L"ClockX", (DWORD)r.left); WriteDword(L"ClockY", (DWORD)r.top); }
        return 0;
    }
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: ClockRender(); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ClockDock(bool on);
static void SetTrayClock(bool docking);
void ClockEnable(bool on) {
    g_clock.on = on;
    WriteDword(L"ClockOn", on);
    if (!on) {
        if (g_clock.theft) ToggleTheft(); // não perde o tempo que estava contando
        if (g_clock.docked) SetTrayClock(false);
        if (g_clock.wnd) { KillTimer(g_clock.wnd, 1); DestroyWindow(g_clock.wnd); g_clock.wnd = nullptr; }
        return;
    }
    if (g_clock.wnd) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = ClockProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsClock";
        RegisterClassW(&wc); reg = true;
    }
    g_clock.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, L"DesignThumbsClock", L"Vizu", WS_POPUP,
                                  0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    ClockRender();
    DWORD x = ReadDword(L"ClockX", 0x80000000), y = ReadDword(L"ClockY", 0x80000000);
    POINT pt = {(LONG)x, (LONG)y};
    if (x == 0x80000000 || !MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) ClockDefaultPos();
    else SetWindowPos(g_clock.wnd, nullptr, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_clock.wnd, SW_SHOWNOACTIVATE);
    if (g_clock.docked) ClockDock(true);
    ClockRender();
    SetTimer(g_clock.wnd, 1, 200, nullptr);
}

// Relógio do Windows na bandeja ("Mostrar hora e data na bandeja do sistema", Windows 11): desligado enquanto o
// DesktopClock está anexado à barra, para não ficarem dois relógios; volta ao desanexar (só se fomos nós que desligamos).
static void SetTrayClock(bool docking) {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    DWORD cur = 1, sz = sizeof(cur);
    bool exists = RegGetValueW(HKEY_CURRENT_USER, key, L"ShowSystrayDateTimeValue", RRF_RT_REG_DWORD, nullptr, &cur, &sz) == ERROR_SUCCESS;
    if (!exists) cur = 1;
    DWORD v;
    if (docking) {
        if (cur == 0) return; // já estava desligado pelo usuário
        v = 0; WriteDword(L"TrayClockHidden", 1);
    } else {
        if (!ReadDword(L"TrayClockHidden", 0)) return;
        v = 1; WriteDword(L"TrayClockHidden", 0);
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"ShowSystrayDateTimeValue", REG_DWORD, &v, sizeof(v));
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 1000, &r);
}

void ClockDock(bool on) {
    RECT t;
    if (on && !TaskbarRect(&t)) { MessageBoxW(g_clock.wnd, T(L"A barra de tarefas precisa estar na horizontal (embaixo ou em cima).", L"The taskbar must be horizontal (bottom or top).", L"La barra de tareas debe estar en horizontal."), L"Vizu", MB_OK | MB_ICONINFORMATION); return; }
    g_clock.docked = on;
    WriteDword(L"ClockDocked", on);
    SetTrayClock(on);
    if (!g_clock.wnd) return;
    if (on) {
        DWORD dx = ReadDword(L"DockX", 0x80000000);
        int x = dx == 0x80000000 ? t.left + 8 : (int)dx;
        SetWindowPos(g_clock.wnd, HWND_TOPMOST, x, t.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        DWORD x = ReadDword(L"ClockX", 0x80000000), y = ReadDword(L"ClockY", 0x80000000);
        POINT pt = {(LONG)x, (LONG)y};
        if (x == 0x80000000 || !MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) { ClockRender(); ClockDefaultPos(); }
        else SetWindowPos(g_clock.wnd, nullptr, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ClockRender();
}

static void ClockLoad() {
    g_clock.font = ReadStr(L"ClockFont", L"Bahnschrift");
    g_clock.fontPx = max(18ul, min(96ul, (unsigned long)ReadDword(L"ClockSize", 38)));
    g_clock.pomoTimes = min(2ul, (unsigned long)ReadDword(L"PomoTimes", 0));
    g_clock.pomoColors = min(2ul, (unsigned long)ReadDword(L"PomoColors", 0));
    g_clock.app = ReadStr(L"ClockApp", L"https://chatgpt.com");
    g_clock.ocr = ReadDword(L"OcrOn", 1) != 0;
    g_clock.theftOn = ReadDword(L"ClockTheft", 1) != 0;
    g_clock.pomoOn = ReadDword(L"ClockPomo", 1) != 0;
    g_clock.agendaOn = ReadDword(L"ClockAgenda", 1) != 0;
    { DWORD sc = ReadDword(L"ClockScale", 100); g_clock.scalePct = sc == 90 || sc == 70 ? sc : 100; }
    g_clock.docked = ReadDword(L"ClockDocked", 0) != 0;
    if (g_lite) { g_clock.ocr = false; return; } // Vizu Lite: sem relógio nem OCR
    if (ReadDword(L"ClockOn", 0)) ClockEnable(true); // desligado até o usuário ativar
}
