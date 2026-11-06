// Clock.inc.h - DesktopClock integrado ao DesignPeek: relógio na área de trabalho, agenda, Pomodoro e Roubo de Tempo.
// Incluído dentro do DesignPeek.cpp (usa g_theme, g_dw, g_inst, T(), ReadDword...). Desligado até o usuário ativar.

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
static const UINT kPomoPairs[4][2] = {{0x34C759, 0xFF3B30}, {0x0A84FF, 0x8E8E93}, {0xB6FF3B, 0xA855F7}, {0xFF9F0A, 0x5AC8FA}};
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
    D2D1_RECT_F rTri{}, rChip[3]{};
    float s = 1;
    bool reminderOpen = false;
};
static ClockState g_clock;
enum { kClkTri = 0, kClkChip0 = 1, kClkChip1, kClkChip2 };

static ULONGLONG ClockLeft() {
    if (!g_clock.phase) return 0;
    if (g_clock.paused) return g_clock.pausedLeft;
    ULONGLONG now = GetTickCount64();
    return g_clock.phaseEnd > now ? g_clock.phaseEnd - now : 0;
}

// ---------- agenda (%LOCALAPPDATA%\DesignThumbs\agenda.txt: "2026-11-06 15:30 Reunião") ----------

struct AgendaItem { SYSTEMTIME when; std::wstring title; };
static std::vector<AgendaItem> LoadAgenda() {
    std::vector<AgendaItem> r;
    FILE* f = _wfopen(DataPath(L"agenda.txt").c_str(), L"rt, ccs=UTF-8");
    if (!f) return r;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        AgendaItem a = {};
        int y, mo, d, h, mi, n = 0;
        if (swscanf(line, L"%d-%d-%d %d:%d %n", &y, &mo, &d, &h, &mi, &n) == 5 && n > 0) {
            a.when.wYear = (WORD)y; a.when.wMonth = (WORD)mo; a.when.wDay = (WORD)d; a.when.wHour = (WORD)h; a.when.wMinute = (WORD)mi;
            a.title = line + n;
            while (!a.title.empty() && (a.title.back() == L'\n' || a.title.back() == L'\r')) a.title.pop_back();
            r.push_back(a);
        }
    }
    fclose(f);
    return r;
}
static void SaveAgenda(const std::vector<AgendaItem>& v) {
    FILE* f = _wfopen(DataPath(L"agenda.txt").c_str(), L"wt, ccs=UTF-8");
    if (!f) return;
    for (auto& a : v) fwprintf(f, L"%04d-%02d-%02d %02d:%02d %ls\n", a.when.wYear, a.when.wMonth, a.when.wDay, a.when.wHour, a.when.wMinute, a.title.c_str());
    fclose(f);
}
static LONGLONG Minutes(const SYSTEMTIME& t) { FILETIME f; SystemTimeToFileTime(&t, &f); return (LONGLONG)(((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 600000000ull); }
static void OpenAgendaFile() {
    std::wstring p = DataPath(L"agenda.txt");
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SYSTEMTIME t; GetLocalTime(&t);
        AgendaItem ex = {t, T(L"Exemplo: apague esta linha. Formato: AAAA-MM-DD HH:MM Título", L"Example: delete this line. Format: YYYY-MM-DD HH:MM Title", L"Ejemplo: borre esta línea. Formato: AAAA-MM-DD HH:MM Título")};
        ex.when.wDay = 1; ex.when.wMonth = 1; ex.when.wYear = 2000;
        SaveAgenda({ex});
    }
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
static bool AgendaSoon(std::wstring* next) { // evento na próxima hora?
    SYSTEMTIME now; GetLocalTime(&now);
    LONGLONG m = Minutes(now), best = LLONG_MAX;
    for (auto& a : LoadAgenda()) {
        LONGLONG d = Minutes(a.when) - m;
        if (d >= 0 && d <= 60 && d < best) { best = d; if (next) { wchar_t b[16]; swprintf(b, 16, L"%02d:%02d ", a.when.wHour, a.when.wMinute); *next = b + a.title; } }
    }
    return best != LLONG_MAX;
}
static void CheckReminders() {
    if (g_clock.reminderOpen) return;
    SYSTEMTIME now; GetLocalTime(&now);
    LONGLONG m = Minutes(now);
    auto items = LoadAgenda();
    for (size_t i = 0; i < items.size(); ++i) {
        LONGLONG d = Minutes(items[i].when);
        if (d > m || d < m - 5) continue; // venceu agora (tolerância de 5 min)
        g_clock.reminderOpen = true;
        wchar_t when[32]; swprintf(when, 32, L"%02d:%02d", items[i].when.wHour, items[i].when.wMinute);
        TASKDIALOG_BUTTON b[] = {{100, T(L"Dispensar", L"Dismiss", L"Descartar")}, {101, T(L"+5 min", L"+5 min", L"+5 min")}};
        TASKDIALOGCONFIG tc = {sizeof(tc)};
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION; tc.pszWindowTitle = L"DesktopClock";
        tc.pszMainIcon = TD_INFORMATION_ICON; tc.pszMainInstruction = items[i].title.c_str(); tc.pszContent = when;
        tc.pButtons = b; tc.cButtons = 2; tc.nDefaultButton = 100;
        int pressed = 100;
        MessageBeep(MB_ICONASTERISK);
        if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) MessageBoxW(nullptr, items[i].title.c_str(), L"DesktopClock", MB_OK | MB_TOPMOST);
        auto cur = LoadAgenda(); // o arquivo pode ter mudado enquanto a caixa estava aberta
        for (size_t k = 0; k < cur.size(); ++k)
            if (Minutes(cur[k].when) == d && cur[k].title == items[i].title) {
                if (pressed == 101) { // adia 5 minutos a partir de agora
                    FILETIME f; SYSTEMTIME n2; GetLocalTime(&n2); SystemTimeToFileTime(&n2, &f);
                    ULONGLONG u = ((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) + 5ull * 600000000ull;
                    f.dwHighDateTime = (DWORD)(u >> 32); f.dwLowDateTime = (DWORD)u; FileTimeToSystemTime(&f, &cur[k].when);
                    cur[k].when.wSecond = 0;
                } else cur.erase(cur.begin() + k);
                break;
            }
        SaveAgenda(cur);
        g_clock.reminderOpen = false;
        return;
    }
}

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
static std::wstring Dur(int sec) {
    wchar_t b[32];
    if (sec >= 3600) swprintf(b, 32, L"%d h %02d min", sec / 3600, sec / 60 % 60);
    else swprintf(b, 32, L"%d min %02d s", sec / 60, sec % 60);
    return b;
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
static void ShowTheftHistory() {
    SYSTEMTIME now; GetLocalTime(&now);
    auto all = LoadThefts();
    int total = 0, n = 0;
    std::wstring list;
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (it->y == now.wYear && it->m == now.wMonth && it->d == now.wDay) {
            total += it->sec; ++n;
            if (n <= 12) { wchar_t b[64]; swprintf(b, 64, L"%02d:%02d   —   %ls\n", it->h, it->mi, Dur(it->sec).c_str()); list += b; }
        }
    if (n > 12) { wchar_t b[64]; swprintf(b, 64, T(L"+ %d mais\n", L"+ %d more\n", L"+ %d más\n"), n - 12); list += b; }
    std::wstring week = T(L"\nÚltimos 7 dias:\n", L"\nLast 7 days:\n", L"\nÚltimos 7 días:\n");
    FILETIME ft; SystemTimeToFileTime(&now, &ft);
    ULONGLONG u = (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    for (int k = 6; k >= 0; --k) {
        ULONGLONG v = u - (ULONGLONG)k * 864000000000ull;
        FILETIME f2 = {(DWORD)v, (DWORD)(v >> 32)}; SYSTEMTIME d; FileTimeToSystemTime(&f2, &d);
        int s = 0; for (auto& t : all) if (t.y == d.wYear && t.m == d.wMonth && t.d == d.wDay) s += t.sec;
        wchar_t b[64]; swprintf(b, 64, L"%02d/%02d   %ls\n", d.wDay, d.wMonth, s ? Dur(s).c_str() : L"—"); week += b;
    }
    wchar_t head[128];
    swprintf(head, 128, T(L"Hoje: %ls perdidos em %d interrupções", L"Today: %ls lost in %d interruptions", L"Hoy: %ls perdidos en %d interrupciones"), Dur(total).c_str(), n);
    std::wstring content = (list.empty() ? std::wstring(T(L"Nenhum roubo de tempo hoje.\n", L"No time theft today.\n", L"Ningún robo de tiempo hoy.\n")) : list) + week;
    TASKDIALOG_BUTTON b[] = {{100, T(L"Abrir planilha (CSV)", L"Open spreadsheet (CSV)", L"Abrir hoja (CSV)")}};
    TASKDIALOGCONFIG tc = {sizeof(tc)};
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION; tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tc.pszWindowTitle = T(L"Roubo de tempo", L"Time theft", L"Robo de tiempo");
    tc.pszMainInstruction = head; tc.pszContent = content.c_str(); tc.pButtons = b; tc.cButtons = 1;
    int pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr)) && pressed == 100) {
        std::wstring p = DataPath(L"roubo-de-tempo.csv");
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
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

// ---------- desenho ----------

static void ClockRender() {
    HWND w = g_clock.wnd;
    if (!w || !g_clock.on) return;
    const ui::Theme& th = g_theme;
    float s = g_clock.s = GetDpiForSystem() / 96.0f;
    float H = 64 * s, M = 14 * s, fpx = (float)g_clock.fontPx;
    bool pomo = g_clock.phase != 0;
    UINT pc = kPomoPairs[min(3ul, (unsigned long)g_clock.pomoColors)][g_clock.phase == 1 ? 0 : 1];

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
    float Wc = 14 * s + 20 * s + 12 * s + timeW + 12 * s + theftW + gap + chip + gap + chip + 10 * s;
    UINT W = (UINT)(Wc + 2 * M), Ht = (UINT)(H + 2 * M);
    fmt::Canvas cv(W, Ht);
    if (!cv.rt) return;
    ID2D1RenderTarget* rt = cv.rt;
    rt->Clear(D2D1::ColorF(0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ui::Painter p(rt, g_dw, th, s);
    D2D1_RECT_F card = D2D1::RectF(M, M, M + Wc, M + H);
    float rad = 22 * s;
    p.Shadow(card, rad);
    D2D1_COLOR_F base = th.neon ? ui::Rgba(0x0E0E10, 0.94f) : th.dark ? ui::Rgba(0x161618, 0.94f) : ui::Rgba(0xFAFAFC, 0.95f);
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
    } else p.Stroke(card, rad, th.neon ? ui::Rgba(ui::NeonColor(), 0.42f) : th.stroke, 1 * s);

    float x = card.left + 14 * s, cy = (card.top + card.bottom) / 2;
    D2D1_COLOR_F hotBg = th.dark ? ui::Rgba(0xFFFFFF, 0.14f) : ui::Rgba(0x000000, 0.09f), chipBg = th.dark ? ui::Rgba(0xFFFFFF, 0.07f) : ui::Rgba(0x000000, 0.05f);
    // 1) ícone da esquerda: agenda (triângulo) ou fase do Pomodoro
    g_clock.rTri = D2D1::RectF(x - 6 * s, cy - 16 * s, x + 26 * s, cy + 16 * s);
    if (g_clock.hot == kClkTri) p.Fill(g_clock.rTri, 10 * s, hotBg);
    if (pomo) p.FeatherIcon(g_clock.phase == 1 ? "target" : "coffee", D2D1::RectF(x, cy - 10 * s, x + 20 * s, cy + 10 * s), 19 * s, ui::Rgba(pc));
    else p.FeatherIcon("alert-triangle", D2D1::RectF(x, cy - 10 * s, x + 20 * s, cy + 10 * s), 19 * s, AgendaSoon(nullptr) ? ui::Rgba(0xFF3B30) : th.text2);
    x += 20 * s + 12 * s;

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
    g_clock.rChip[0] = chipRect(theftW);
    if (g_clock.theft) {
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
        p.FeatherIcon("skull", g_clock.rChip[0], 18 * s, th.text);
    }
    g_clock.rChip[1] = chipRect(chip);
    g_clock.rChip[2] = chipRect(chip);
    if (pomo) {
        p.Fill(g_clock.rChip[1], chip / 2, ui::Rgba(pc, g_clock.hot == kClkChip1 ? 0.8f : 1.0f));
        UINT on = (pc >> 16 & 255) * 3 + (pc >> 8 & 255) * 6 + (pc & 255) > 1700 ? 0x0B0B0C : 0xFFFFFF; // texto legível sobre a cor
        p.FeatherIcon(g_clock.paused ? "play" : "pause", g_clock.rChip[1], 16 * s, ui::Rgba(on));
        p.Fill(g_clock.rChip[2], chip / 2, g_clock.hot == kClkChip2 ? hotBg : chipBg);
        p.FeatherIcon("square", g_clock.rChip[2], 16 * s, th.text);
    } else {
        p.Fill(g_clock.rChip[1], chip / 2, g_clock.hot == kClkChip1 ? hotBg : chipBg);
        p.FeatherIcon("timer", g_clock.rChip[1], 18 * s, th.text);
        p.Fill(g_clock.rChip[2], chip / 2, g_clock.hot == kClkChip2 ? hotBg : chipBg);
        p.FeatherIcon("message-circle", g_clock.rChip[2], 18 * s, th.text);
    }

    HBITMAP bmp = nullptr;
    if (FAILED(cv.Finish(&bmp)) || !bmp) return;
    RECT wr; GetWindowRect(w, &wr);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT pos = {wr.left, wr.top}, zero = {0, 0};
    SIZE size = {(LONG)W, (LONG)Ht};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(w, screen, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteDC(mem); ReleaseDC(nullptr, screen); DeleteObject(bmp);
}

static int ClockHit(POINT pt) {
    auto in = [&](const D2D1_RECT_F& r) { return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom; };
    if (in(g_clock.rTri)) return kClkTri;
    for (int i = 0; i < 3; ++i) if (in(g_clock.rChip[i])) return kClkChip0 + i;
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
    AppendMenuW(m, MF_STRING, 7, T(L"Voltar ao canto", L"Back to the corner", L"Volver a la esquina"));
    AppendMenuW(m, MF_STRING, 8, T(L"Configurações", L"Settings", L"Configuración"));
    AppendMenuW(m, MF_STRING, 9, T(L"Ocultar relógio", L"Hide clock", L"Ocultar reloj"));
    SetForegroundWindow(g_clock.wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_clock.wnd, nullptr);
    DestroyMenu(m);
    switch (c) {
    case 1: OpenAgendaFile(); break;
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
        else if (MessageBoxW(g_clock.wnd, T(L"Voltar a abrir o ChatGPT no botão?", L"Go back to opening ChatGPT from the button?", L"¿Volver a abrir ChatGPT con el botón?"), L"DesktopClock", MB_YESNO | MB_ICONQUESTION) == IDYES) { g_clock.app = L"https://chatgpt.com"; ClockSave(); }
        break;
    }
    case 7: ClockDefaultPos(); break;
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
            ClockRender();
            if (t.wSecond == 0 || t.wSecond == 30) CheckReminders();
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
            std::wstring next;
            if (!g_clock.phase && AgendaSoon(&next)) MessageBoxW(h, next.c_str(), T(L"Próximo evento", L"Next event", L"Próximo evento"), MB_OK | MB_ICONINFORMATION);
            else if (!g_clock.phase) OpenAgendaFile();
        } else if (hit == kClkChip0) ToggleTheft();
        else if (hit == kClkChip1) PomoPauseResume();
        else if (hit == kClkChip2) {
            if (g_clock.phase) PomoStop();
            else ShellExecuteW(nullptr, L"open", g_clock.app.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ClockRender();
        return 0;
    }
    case WM_RBUTTONUP: { POINT pt; GetCursorPos(&pt); ClockMenu(pt); return 0; }
    case WM_EXITSIZEMOVE: { RECT r; GetWindowRect(h, &r); WriteDword(L"ClockX", (DWORD)r.left); WriteDword(L"ClockY", (DWORD)r.top); return 0; }
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: ClockRender(); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ClockEnable(bool on) {
    g_clock.on = on;
    WriteDword(L"ClockOn", on);
    if (!on) {
        if (g_clock.theft) ToggleTheft(); // não perde o tempo que estava contando
        if (g_clock.wnd) { KillTimer(g_clock.wnd, 1); DestroyWindow(g_clock.wnd); g_clock.wnd = nullptr; }
        return;
    }
    if (g_clock.wnd) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = ClockProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsClock";
        RegisterClassW(&wc); reg = true;
    }
    g_clock.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, L"DesignThumbsClock", L"DesktopClock", WS_POPUP,
                                  0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    ClockRender();
    DWORD x = ReadDword(L"ClockX", 0x80000000), y = ReadDword(L"ClockY", 0x80000000);
    POINT pt = {(LONG)x, (LONG)y};
    if (x == 0x80000000 || !MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) ClockDefaultPos();
    else SetWindowPos(g_clock.wnd, nullptr, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_clock.wnd, SW_SHOWNOACTIVATE);
    ClockRender();
    SetTimer(g_clock.wnd, 1, 200, nullptr);
}

static void ClockLoad() {
    g_clock.font = ReadStr(L"ClockFont", L"Bahnschrift");
    g_clock.fontPx = max(18ul, min(96ul, (unsigned long)ReadDword(L"ClockSize", 38)));
    g_clock.pomoTimes = min(2ul, (unsigned long)ReadDword(L"PomoTimes", 0));
    g_clock.pomoColors = min(3ul, (unsigned long)ReadDword(L"PomoColors", 0));
    g_clock.app = ReadStr(L"ClockApp", L"https://chatgpt.com");
    if (ReadDword(L"ClockOn", 0)) ClockEnable(true); // desligado até o usuário ativar
}
