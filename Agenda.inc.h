// Agenda.inc.h - janela da agenda do DesktopClock (dia, semana, novo evento) e avisos no canto (10 e 5 min antes).
// Incluído no Vizu.cpp depois do Clock.inc.h. Tudo desenhado em Direct2D, seguindo o tema escolhido.

#include <set>

static D2D1_COLOR_F AccentOf(const ui::Theme& th) {
    return th.classic ? ui::Rgba(0xFF6633) : th.neon ? ui::Rgba(ui::NeonColor()) : g_cfg.themeMode == ui::kThemeWindows ? th.accent : th.dark ? ui::Rgba(0xE6E6E8) : ui::Rgba(0x1D1D1F);
}
static D2D1_COLOR_F OnAccentOf(const ui::Theme& th) {
    return th.classic ? ui::Rgba(0x000000) : th.neon || (th.dark && g_cfg.themeMode != ui::kThemeWindows) ? ui::Rgba(0x0E0E10) : ui::Rgba(0xFFFFFF);
}
static SYSTEMTIME AddDays(SYSTEMTIME t, int d) {
    FILETIME f; SystemTimeToFileTime(&t, &f);
    ULONGLONG u = ((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) + (LONGLONG)d * 864000000000LL;
    f.dwHighDateTime = (DWORD)(u >> 32); f.dwLowDateTime = (DWORD)u;
    FileTimeToSystemTime(&f, &t);
    return t;
}
static bool SameDay(const SYSTEMTIME& a, const SYSTEMTIME& b) { return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay; }
static int DayOfWeek(SYSTEMTIME t) { t.wHour = 12; FILETIME f; SystemTimeToFileTime(&t, &f); FileTimeToSystemTime(&f, &t); return t.wDayOfWeek; } // 0 = domingo

// ---------- janela da agenda ----------

struct AgendaUi {
    HWND wnd = nullptr;
    ui::Surface surf;
    SYSTEMTIME day{};
    std::vector<std::pair<int, D2D1_RECT_F>> hits;
    int hot = 0, field = 0;          // campo com foco: 0 título, 1 dia, 2 hora, -1 nenhum
    std::wstring f[3];
    int dur = 30, alerts = kAlert10 | kAlert5;
    int editIdx = -1;                // evento em edição (índice na lista do arquivo)
    std::wstring msg;                // erro do formulário
};
static AgendaUi g_ag;
enum { kAgPrev = 1, kAgToday, kAgNext, kAgClose, kAgWeek0 = 10, kAgField0 = 20, kAgDur0 = 30, kAgAl0 = 40, kAgSave = 50, kAgDelete, kAgCancel, kAgEvent0 = 100 };

static const wchar_t* MonthName(int m) {
    static const wchar_t* pt[] = {L"janeiro", L"fevereiro", L"março", L"abril", L"maio", L"junho", L"julho", L"agosto", L"setembro", L"outubro", L"novembro", L"dezembro"};
    static const wchar_t* en[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September", L"October", L"November", L"December"};
    static const wchar_t* es[] = {L"enero", L"febrero", L"marzo", L"abril", L"mayo", L"junio", L"julio", L"agosto", L"septiembre", L"octubre", L"noviembre", L"diciembre"};
    int l = i18n::Current();
    return (l == 1 ? en : l == 2 ? es : pt)[(m + 11) % 12];
}
static const wchar_t* WeekName(int d, bool shortName) {
    static const wchar_t* pt[] = {L"Domingo", L"Segunda-feira", L"Terça-feira", L"Quarta-feira", L"Quinta-feira", L"Sexta-feira", L"Sábado"};
    static const wchar_t* en[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
    static const wchar_t* es[] = {L"Domingo", L"Lunes", L"Martes", L"Miércoles", L"Jueves", L"Viernes", L"Sábado"};
    static const wchar_t* ptS[] = {L"DOM", L"SEG", L"TER", L"QUA", L"QUI", L"SEX", L"SÁB"};
    static const wchar_t* enS[] = {L"SUN", L"MON", L"TUE", L"WED", L"THU", L"FRI", L"SAT"};
    static const wchar_t* esS[] = {L"DOM", L"LUN", L"MAR", L"MIÉ", L"JUE", L"VIE", L"SÁB"};
    int l = i18n::Current();
    return shortName ? (l == 1 ? enS : l == 2 ? esS : ptS)[d] : (l == 1 ? en : l == 2 ? es : pt)[d];
}

static void AgResetForm() {
    g_ag.editIdx = -1; g_ag.msg.clear();
    wchar_t b[16];
    swprintf(b, 16, L"%02d/%02d", g_ag.day.wDay, g_ag.day.wMonth); g_ag.f[1] = b;
    SYSTEMTIME now; GetLocalTime(&now);
    int h = now.wHour + 1; if (h > 23) h = 23;
    swprintf(b, 16, L"%02d:00", h); g_ag.f[2] = b;
    g_ag.f[0].clear(); g_ag.dur = 30; g_ag.alerts = kAlert10 | kAlert5; g_ag.field = 0;
}

static void PaintAgenda(HWND h) {
    g_ag.surf.hwnd = h;
    if (!g_ag.surf.Begin(g_d2d)) return;
    ID2D1RenderTarget* rt = g_ag.surf.rt;
    const ui::Theme& th = g_theme;
    float s = WndScale(h);
    D2D1_SIZE_F sz = rt->GetSize();
    float W = sz.width, H = sz.height;
    rt->Clear(th.tint);
    ui::Backdrop(rt, th, W, H);
    ui::Painter p(rt, g_dw, th, s);
    D2D1_COLOR_F acc = AccentOf(th), onAcc = OnAccentOf(th);
    D2D1_COLOR_F sep = th.neon ? ui::Rgba(ui::NeonColor(), 0.14f) : th.dark ? ui::Rgba(0xFFFFFF, 0.07f) : ui::Rgba(0x000000, 0.07f);
    D2D1_COLOR_F field = th.dark ? ui::Rgba(0xFFFFFF, 0.06f) : ui::Rgba(0x000000, 0.045f);
    D2D1_COLOR_F hov = th.dark ? ui::Rgba(0xFFFFFF, 0.08f) : ui::Rgba(0x000000, 0.05f);
    g_ag.hits.clear();
    auto hit = [&](int id, D2D1_RECT_F r) { g_ag.hits.push_back({id, r}); return g_ag.hot == id; };
    SYSTEMTIME today; GetLocalTime(&today);

    // cabeçalho
    float x = 26 * s, y = 22 * s;
    p.Text(T(L"AGENDA", L"AGENDA", L"AGENDA"), D2D1::RectF(x, y, x + 300 * s, y + 16 * s), 11, th.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    wchar_t title[96];
    int dow = DayOfWeek(g_ag.day);
    if (i18n::Current() == 1) swprintf(title, 96, L"%ls, %ls %d", WeekName(dow, false), MonthName(g_ag.day.wMonth), g_ag.day.wDay);
    else swprintf(title, 96, L"%ls, %d de %ls", WeekName(dow, false), g_ag.day.wDay, MonthName(g_ag.day.wMonth));
    p.Text(title, D2D1::RectF(x, y + 16 * s, W - 260 * s, y + 52 * s), 24, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float bx = W - 26 * s;
    {
        D2D1_RECT_F r = D2D1::RectF(bx - 34 * s, y + 10 * s, bx, y + 44 * s);
        if (hit(kAgClose, r)) p.Fill(r, 17 * s, hov);
        p.FeatherIcon("x", r, 16 * s, th.text2);
        bx -= 48 * s;
        r = D2D1::RectF(bx - 34 * s, y + 10 * s, bx, y + 44 * s);
        p.CircleButton(r, L"", hit(kAgNext, r)); bx -= 42 * s;
        r = D2D1::RectF(bx - 66 * s, y + 10 * s, bx, y + 44 * s);
        p.Pill(r, T(L"Hoje", L"Today", L"Hoy"), hit(kAgToday, r)); bx -= 74 * s;
        r = D2D1::RectF(bx - 34 * s, y + 10 * s, bx, y + 44 * s);
        p.CircleButton(r, L"", hit(kAgPrev, r));
    }
    y += 64 * s;

    auto items = LoadAgenda();
    // semana (segunda a domingo)
    SYSTEMTIME mon = AddDays(g_ag.day, -((dow + 6) % 7));
    float cw = (W - 52 * s - 6 * 8 * s) / 7;
    for (int i = 0; i < 7; ++i) {
        SYSTEMTIME d = AddDays(mon, i);
        D2D1_RECT_F r = D2D1::RectF(x + i * (cw + 8 * s), y, x + i * (cw + 8 * s) + cw, y + 62 * s);
        bool sel = SameDay(d, g_ag.day), isToday = SameDay(d, today), hv = hit(kAgWeek0 + i, r);
        if (sel) p.Fill(r, 14 * s, acc); else if (hv) p.Fill(r, 14 * s, hov);
        D2D1_COLOR_F tc = sel ? onAcc : th.text, t2 = sel ? onAcc : th.text2;
        if (sel) t2.a = 0.8f;
        p.Text(WeekName(DayOfWeek(d), true), D2D1::RectF(r.left, r.top + 6 * s, r.right, r.top + 20 * s), 10.5f, t2, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        p.Text(std::to_wstring(d.wDay), D2D1::RectF(r.left, r.top + 20 * s, r.right, r.top + 44 * s), 16, isToday && !sel ? acc : tc, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        bool has = false; for (auto& a : items) if (SameDay(a.when, d)) has = true;
        if (has) { float cx = (r.left + r.right) / 2; p.Fill(D2D1::RectF(cx - 2.5f * s, r.top + 48 * s, cx + 2.5f * s, r.top + 53 * s), 2.5f * s, sel ? onAcc : acc); }
    }
    y += 76 * s;
    p.Fill(D2D1::RectF(x, y, W - 26 * s, y + 1 * s), 0, sep);
    y += 14 * s;

    // linha do tempo (à esquerda) e formulário (à direita)
    float formW = 300 * s, fx = W - formW, tlR = fx - 18 * s, top = y + 6 * s, bottom = H - 24 * s;
    std::vector<int> dayIdx;
    for (int i = 0; i < (int)items.size(); ++i) if (SameDay(items[i].when, g_ag.day)) dayIdx.push_back(i);
    std::sort(dayIdx.begin(), dayIdx.end(), [&](int a, int b) { return Minutes(items[a].when) < Minutes(items[b].when); });
    int h0 = 8, h1 = 19;
    for (int i : dayIdx) { h0 = min(h0, (int)items[i].when.wHour); h1 = max(h1, (int)min(24, (int)items[i].when.wHour + (items[i].when.wMinute + items[i].dur + 59) / 60)); }
    if (SameDay(g_ag.day, today)) { h0 = min(h0, (int)today.wHour); h1 = max(h1, min(24, (int)today.wHour + 1)); }
    float hourH = (bottom - top) / (float)(h1 - h0);
    for (int hh = h0; hh < h1; ++hh) {
        float yy = top + (hh - h0) * hourH;
        p.Fill(D2D1::RectF(x, yy, tlR, yy + 1 * s), 0, sep);
        wchar_t b[8]; swprintf(b, 8, L"%02d:00", hh);
        p.Text(b, D2D1::RectF(x, yy + 2 * s, x + 50 * s, yy + 16 * s), 10, th.text2);
    }
    LONGLONG nowM = Minutes(today);
    int nextIdx = -1;
    for (int i : dayIdx) if (Minutes(items[i].when) >= nowM) { nextIdx = i; break; }
    float ex = x + 52 * s;
    // eventos que se sobrepõem (na tela) ficam lado a lado, em colunas
    std::vector<float> ys0(items.size()), ys1(items.size());
    std::vector<int> col(items.size(), 0), ncol(items.size(), 1);
    for (int i : dayIdx) {
        const AgendaItem& a = items[i];
        ys0[i] = top + ((a.when.wHour - h0) * 60 + a.when.wMinute) / 60.0f * hourH;
        ys1[i] = min(bottom, max(ys0[i] + 26 * s, ys0[i] + a.dur / 60.0f * hourH - 2 * s));
    }
    {
        std::vector<int> cluster; float clusterEnd = -1;
        std::vector<float> colEnd;
        auto flush = [&]() { int n = (int)colEnd.size(); for (int j : cluster) ncol[j] = max(1, n); cluster.clear(); colEnd.clear(); };
        for (int i : dayIdx) {
            if (!cluster.empty() && ys0[i] >= clusterEnd) flush();
            int c = 0;
            while (c < (int)colEnd.size() && colEnd[c] > ys0[i]) ++c;
            if (c == (int)colEnd.size()) colEnd.push_back(ys1[i]); else colEnd[c] = ys1[i];
            col[i] = c; cluster.push_back(i); clusterEnd = max(clusterEnd, ys1[i]);
        }
        flush();
    }
    for (int i : dayIdx) {
        const AgendaItem& a = items[i];
        float y0 = ys0[i], y1 = ys1[i];
        float cwid = (tlR - ex - (ncol[i] - 1) * 4 * s) / ncol[i];
        D2D1_RECT_F r = D2D1::RectF(ex + col[i] * (cwid + 4 * s), y0 + 1 * s, ex + col[i] * (cwid + 4 * s) + cwid, y1);
        bool past = Minutes(a.when) + a.dur < nowM, next = i == nextIdx, editing = i == g_ag.editIdx, hv = hit(kAgEvent0 + i, r);
        D2D1_COLOR_F bg = next || editing ? (th.neon ? ui::Rgba(ui::NeonColor(), 0.14f) : th.dark ? ui::Rgba(0xFFFFFF, 0.10f) : ui::Rgba(0x000000, 0.07f)) : field;
        if (hv) bg = th.dark ? ui::Rgba(0xFFFFFF, 0.12f) : ui::Rgba(0x000000, 0.08f);
        if (next && th.neon) p.Glow(r, 10 * s);
        p.Fill(r, 10 * s, bg);
        D2D1_COLOR_F bar = next || editing ? acc : (th.dark ? ui::Rgba(0xFFFFFF, 0.25f) : ui::Rgba(0x000000, 0.25f));
        p.Fill(D2D1::RectF(r.left, r.top + 4 * s, r.left + 3 * s, r.bottom - 4 * s), 1.5f * s, bar);
        D2D1_COLOR_F tc = th.text, t2 = th.text2;
        if (past) { tc.a *= 0.5f; t2.a *= 0.5f; }
        wchar_t when[48];
        LONGLONG endM = Minutes(a.when) + a.dur; SYSTEMTIME e = a.when; e.wHour = (WORD)((a.when.wHour * 60 + a.when.wMinute + a.dur) / 60 % 24); e.wMinute = (WORD)((a.when.wMinute + a.dur) % 60); (void)endM;
        swprintf(when, 48, L"%02d:%02d – %02d:%02d", a.when.wHour, a.when.wMinute, e.wHour, e.wMinute);
        bool tall = r.bottom - r.top > 40 * s;
        p.Text(a.title, D2D1::RectF(r.left + 12 * s, r.top + 4 * s, r.right - (r.right - r.left > 260 * s ? 112 : 8) * s, r.top + (tall ? 22 : r.bottom - r.top - 4) * s), 13, tc, next ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (tall) p.Text(when, D2D1::RectF(r.left + 12 * s, r.top + 22 * s, r.right - 12 * s, r.top + 38 * s), 11, t2);
        else if (r.right - r.left > 260 * s) p.Text(when, D2D1::RectF(r.right - 110 * s, r.top, r.right - 10 * s, r.bottom), 11, t2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (next) {
            LONGLONG left = Minutes(a.when) - nowM;
            wchar_t b[32];
            if (left < 60) swprintf(b, 32, T(L"EM %d MIN", L"IN %d MIN", L"EN %d MIN"), (int)left); else swprintf(b, 32, T(L"EM %d H", L"IN %d H", L"EN %d H"), (int)(left / 60));
            if (tall) p.Text(b, D2D1::RectF(r.right - 100 * s, r.top + 4 * s, r.right - 10 * s, r.top + 22 * s), 10.5f, acc, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
    if (dayIdx.empty())
        p.Text(T(L"Nenhum evento neste dia.", L"No events on this day.", L"Ningún evento este día."), D2D1::RectF(ex, top + 20 * s, tlR, top + 44 * s), 13, th.text2);
    if (SameDay(g_ag.day, today)) { // agora
        float yy = top + ((today.wHour - h0) * 60 + today.wMinute) / 60.0f * hourH;
        D2D1_COLOR_F red = ui::Rgba(0xFF3B30);
        p.Fill(D2D1::RectF(ex - 6 * s, yy - 4.5f * s, ex + 3 * s, yy + 4.5f * s), 4.5f * s, red);
        p.Fill(D2D1::RectF(ex, yy - 0.75f * s, tlR, yy + 0.75f * s), 0, red);
    }

    // formulário
    p.Fill(D2D1::RectF(fx, top - 6 * s, fx + 1 * s, bottom), 0, sep);
    float fx0 = fx + 22 * s, fx1 = W - 22 * s, fy = top + 4 * s;
    p.Text(g_ag.editIdx >= 0 ? T(L"EDITAR EVENTO", L"EDIT EVENT", L"EDITAR EVENTO") : T(L"NOVO EVENTO", L"NEW EVENT", L"NUEVO EVENTO"),
           D2D1::RectF(fx0, fy, fx1, fy + 16 * s), 11, th.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fy += 26 * s;
    bool caretOn = (GetTickCount() / 530) % 2 == 0;
    auto input = [&](int idx, const wchar_t* label, D2D1_RECT_F r) {
        p.Text(label, D2D1::RectF(r.left, r.top - 18 * s, r.right, r.top - 2 * s), 11.5f, th.text2);
        bool focus = g_ag.field == idx, hv = hit(kAgField0 + idx, r);
        p.Fill(r, 12 * s, hv && !focus ? hov : field);
        if (focus) p.Stroke(r, 12 * s, acc, 1.2f * s);
        std::wstring t = g_ag.f[idx];
        float tw = p.Measure(t, 13.5f);
        D2D1_RECT_F tr = D2D1::RectF(r.left + 12 * s, r.top, r.right - 10 * s, r.bottom);
        p.Text(t, tr, 13.5f, th.text);
        if (focus && caretOn) { float cx = min(tr.right, tr.left + tw + 1 * s); p.Fill(D2D1::RectF(cx, r.top + 10 * s, cx + 1.4f * s, r.bottom - 10 * s), 0, acc); }
    };
    fy += 18 * s;
    input(0, T(L"Título", L"Title", L"Título"), D2D1::RectF(fx0, fy, fx1, fy + 38 * s));
    fy += 38 * s + 30 * s;
    float half = (fx1 - fx0 - 10 * s) / 2;
    input(1, T(L"Dia (dd/mm)", L"Day (dd/mm)", L"Día (dd/mm)"), D2D1::RectF(fx0, fy, fx0 + half, fy + 38 * s));
    input(2, T(L"Hora", L"Time", L"Hora"), D2D1::RectF(fx0 + half + 10 * s, fy, fx1, fy + 38 * s));
    fy += 38 * s + 30 * s;
    {
        p.Text(T(L"Duração", L"Duration", L"Duración"), D2D1::RectF(fx0, fy - 18 * s, fx1, fy - 2 * s), 11.5f, th.text2);
        D2D1_RECT_F r = D2D1::RectF(fx0, fy, fx1, fy + 34 * s), parts[3];
        std::wstring l[3] = {L"15 min", L"30 min", L"1 h"};
        int sel = g_ag.dur == 15 ? 0 : g_ag.dur == 60 ? 2 : 1;
        int hv = g_ag.hot >= kAgDur0 && g_ag.hot <= kAgDur0 + 2 ? g_ag.hot - kAgDur0 : -1;
        p.Segmented(r, l, 3, sel, hv, parts);
        for (int i = 0; i < 3; ++i) hit(kAgDur0 + i, parts[i]);
    }
    fy += 34 * s + 30 * s;
    {
        p.Text(T(L"Avisar antes", L"Remind me", L"Avisar antes"), D2D1::RectF(fx0, fy - 18 * s, fx1, fy - 2 * s), 11.5f, th.text2);
        const wchar_t* l[3] = {L"10 min", L"5 min", T(L"Na hora", L"On time", L"A la hora")};
        int bits[3] = {kAlert10, kAlert5, kAlert0};
        float cx = fx0;
        for (int i = 0; i < 3; ++i) {
            float wdt = p.Measure(l[i], 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 26 * s;
            D2D1_RECT_F r = D2D1::RectF(cx, fy, cx + wdt, fy + 30 * s);
            bool on = (g_ag.alerts & bits[i]) != 0, hv = hit(kAgAl0 + i, r);
            if (on) p.Fill(r, 15 * s, acc);
            else { if (hv) p.Fill(r, 15 * s, hov); p.Stroke(r, 15 * s, th.neon ? ui::Rgba(ui::NeonColor(), 0.5f) : th.dark ? ui::Rgba(0xFFFFFF, 0.16f) : ui::Rgba(0x000000, 0.12f), 1 * s); }
            p.Text(l[i], r, 12.5f, on ? onAcc : th.text2, on ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            cx += wdt + 6 * s;
        }
    }
    fy += 30 * s + 22 * s;
    {
        D2D1_RECT_F r = D2D1::RectF(fx0, fy, fx1, fy + 40 * s);
        p.Pill(r, g_ag.editIdx >= 0 ? T(L"Salvar alterações", L"Save changes", L"Guardar cambios") : T(L"Adicionar à agenda", L"Add to agenda", L"Añadir a la agenda"), hit(kAgSave, r), true);
        fy += 48 * s;
        if (g_ag.editIdx >= 0) {
            float hw = (fx1 - fx0 - 8 * s) / 2;
            D2D1_RECT_F d = D2D1::RectF(fx0, fy, fx0 + hw, fy + 34 * s), c = D2D1::RectF(fx0 + hw + 8 * s, fy, fx1, fy + 34 * s);
            p.Pill(d, T(L"Excluir", L"Delete", L"Eliminar"), hit(kAgDelete, d));
            p.Pill(c, T(L"Cancelar", L"Cancel", L"Cancelar"), hit(kAgCancel, c));
            fy += 42 * s;
        }
    }
    if (!g_ag.msg.empty()) { p.Text(g_ag.msg, D2D1::RectF(fx0, fy, fx1, fy + 18 * s), 11.5f, ui::Rgba(0xFF453A), DWRITE_FONT_WEIGHT_SEMI_BOLD); fy += 22 * s; }
    p.Paragraph(T(L"Clique num evento para editar ou excluir. Tab passa de campo, Enter salva.", L"Click an event to edit or delete it. Tab moves between fields, Enter saves.",
                  L"Haz clic en un evento para editarlo o eliminarlo. Tab cambia de campo, Enter guarda."), fx0, fy, fx1 - fx0, 11.5f, th.text2);
    g_ag.surf.End();
}

static void AgSave() {
    std::wstring t = g_ag.f[0];
    while (!t.empty() && t.back() == L' ') t.pop_back();
    while (!t.empty() && t[0] == L' ') t.erase(0, 1);
    int d = 0, m = 0, hh = -1, mi = -1;
    swscanf(g_ag.f[1].c_str(), L"%d/%d", &d, &m);
    swscanf(g_ag.f[2].c_str(), L"%d:%d", &hh, &mi);
    if (t.empty()) { g_ag.msg = T(L"Escreva um título.", L"Type a title.", L"Escribe un título."); g_ag.field = 0; return; }
    if (d < 1 || d > 31 || m < 1 || m > 12) { g_ag.msg = T(L"Dia inválido (use dd/mm).", L"Invalid day (use dd/mm).", L"Día inválido (usa dd/mm)."); g_ag.field = 1; return; }
    if (hh < 0 || hh > 23 || mi < 0 || mi > 59) { g_ag.msg = T(L"Hora inválida (use hh:mm).", L"Invalid time (use hh:mm).", L"Hora inválida (usa hh:mm)."); g_ag.field = 2; return; }
    AgendaItem a;
    a.when = {}; a.when.wYear = g_ag.day.wYear; a.when.wMonth = (WORD)m; a.when.wDay = (WORD)d; a.when.wHour = (WORD)hh; a.when.wMinute = (WORD)mi;
    FILETIME ft;
    if (!SystemTimeToFileTime(&a.when, &ft)) { g_ag.msg = T(L"Essa data não existe.", L"That date doesn't exist.", L"Esa fecha no existe."); g_ag.field = 1; return; }
    a.title = t; a.dur = g_ag.dur; a.alerts = g_ag.alerts;
    auto items = LoadAgenda();
    if (g_ag.editIdx >= 0 && g_ag.editIdx < (int)items.size()) items[g_ag.editIdx] = a; else items.push_back(a);
    if (!SaveAgenda(items)) { g_ag.msg = T(L"Não foi possível salvar a agenda. Verifique o disco e tente de novo.", L"Couldn't save the agenda. Check the disk and try again.", L"No se pudo guardar la agenda. Revisa el disco e inténtalo de nuevo."); return; } // o evento não some da tela
    g_ag.day = a.when;
    AgResetForm();
    ClockRender();
}

static void AgAction(int id) {
    SYSTEMTIME today; GetLocalTime(&today);
    if (id == kAgClose) { DestroyWindow(g_ag.wnd); return; }
    if (id == kAgPrev) g_ag.day = AddDays(g_ag.day, -1);
    else if (id == kAgNext) g_ag.day = AddDays(g_ag.day, 1);
    else if (id == kAgToday) g_ag.day = today;
    else if (id >= kAgWeek0 && id < kAgWeek0 + 7) { int dow = DayOfWeek(g_ag.day); g_ag.day = AddDays(AddDays(g_ag.day, -((dow + 6) % 7)), id - kAgWeek0); }
    else if (id >= kAgField0 && id <= kAgField0 + 2) { g_ag.field = id - kAgField0; return; }
    else if (id >= kAgDur0 && id <= kAgDur0 + 2) { static const int d[3] = {15, 30, 60}; g_ag.dur = d[id - kAgDur0]; return; }
    else if (id >= kAgAl0 && id <= kAgAl0 + 2) { static const int b[3] = {kAlert10, kAlert5, kAlert0}; g_ag.alerts ^= b[id - kAgAl0]; return; }
    else if (id == kAgSave) { AgSave(); return; }
    else if (id == kAgCancel) { AgResetForm(); return; }
    else if (id == kAgDelete) {
        auto items = LoadAgenda();
        if (g_ag.editIdx >= 0 && g_ag.editIdx < (int)items.size()) { items.erase(items.begin() + g_ag.editIdx); SaveAgenda(items); ClockRender(); }
        AgResetForm(); return;
    } else if (id >= kAgEvent0) {
        auto items = LoadAgenda();
        int i = id - kAgEvent0;
        if (i < (int)items.size()) {
            const AgendaItem& a = items[i];
            g_ag.editIdx = i; g_ag.msg.clear(); g_ag.f[0] = a.title;
            wchar_t b[16];
            swprintf(b, 16, L"%02d/%02d", a.when.wDay, a.when.wMonth); g_ag.f[1] = b;
            swprintf(b, 16, L"%02d:%02d", a.when.wHour, a.when.wMinute); g_ag.f[2] = b;
            g_ag.dur = a.dur <= 15 ? 15 : a.dur >= 60 ? 60 : 30; g_ag.alerts = a.alerts; g_ag.field = 0;
        }
        return;
    }
    if (g_ag.editIdx < 0) { wchar_t b[16]; swprintf(b, 16, L"%02d/%02d", g_ag.day.wDay, g_ag.day.wMonth); g_ag.f[1] = b; }
}

static void AgChar(wchar_t c) {
    if (g_ag.field < 0) return;
    std::wstring& f = g_ag.f[g_ag.field];
    g_ag.msg.clear();
    if (c == 8) { if (!f.empty()) f.pop_back(); return; }
    if (c == 9) { g_ag.field = (g_ag.field + ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 1)) % 3; return; }
    if (c == 13) { AgSave(); return; }
    if (c == 22) { // Ctrl+V
        if (OpenClipboard(g_ag.wnd)) {
            if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) { const wchar_t* t = (const wchar_t*)GlobalLock(hd); if (t) { for (const wchar_t* q = t; *q && f.size() < 80; ++q) if (*q >= 32) f += *q; GlobalUnlock(hd); } }
            CloseClipboard();
        }
        return;
    }
    if (c < 32) return;
    if (g_ag.field == 0) { if (f.size() < 80) f += c; return; }
    // dia e hora: só dígitos; o separador entra sozinho
    wchar_t sepc = g_ag.field == 1 ? L'/' : L':';
    if (c == sepc && f.size() == 2) { f += c; return; }
    if (!iswdigit(c) || f.size() >= 5) return;
    if (f.size() == 2) f += sepc;
    f += c;
}

static LRESULT CALLBACK AgendaProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        bool onButton = false;
        for (auto& e : g_ag.hits) if (ui::Hit(e.second, pt)) onButton = true;
        return pt.y < 78 * WndScale(h) && !onButton ? HTCAPTION : HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = 0;
        for (auto& e : g_ag.hits) if (ui::Hit(e.second, pt)) hot = e.first;
        if (hot != g_ag.hot) { g_ag.hot = hot; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { bool txt = g_ag.hot >= kAgField0 && g_ag.hot <= kAgField0 + 2; SetCursor(LoadCursor(nullptr, txt ? IDC_IBEAM : g_ag.hot ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_MOUSELEAVE: if (g_ag.hot) { g_ag.hot = 0; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int id = 0;
        for (auto& e : g_ag.hits) if (ui::Hit(e.second, pt)) id = e.first;
        if (id) AgAction(id); else g_ag.field = -1;
        if (g_ag.wnd) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CHAR: AgChar((wchar_t)wp); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { if (g_ag.editIdx >= 0) { AgResetForm(); InvalidateRect(h, nullptr, FALSE); } else DestroyWindow(h); }
        return 0;
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0; // cursor piscando e "agora"
    case WM_SIZE: g_ag.surf.Resize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintAgenda(h); EndPaint(h, &ps); return 0; }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_ag.surf.Release(); g_ag.hits.clear(); g_ag.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenAgendaWindow() {
    if (g_ag.wnd) { ShowWindow(g_ag.wnd, SW_RESTORE); SetForegroundWindow(g_ag.wnd); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = AgendaProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsAgenda";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIcon(g_inst, MAKEINTRESOURCE(1));
        RegisterClassW(&wc); reg = true;
    }
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpix = 96, dpiy = 96;
    GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpix, &dpiy);
    float s = dpix / 96.0f;
    int w = (int)(880 * s), h = min((int)(700 * s), (int)(mi.rcWork.bottom - mi.rcWork.top - 40));
    GetLocalTime(&g_ag.day);
    g_ag.hot = 0;
    AgResetForm();
    g_ag.wnd = CreateWindowExW(WS_EX_APPWINDOW, L"DesignThumbsAgenda", T(L"Agenda — Vizu", L"Agenda — Vizu", L"Agenda — Vizu"), WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                               mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h,
                               nullptr, nullptr, g_inst, nullptr);
    ui::EnableGlass(g_ag.wnd, g_theme.dark);
    ShowWindow(g_ag.wnd, SW_SHOW);
    SetForegroundWindow(g_ag.wnd);
    SetTimer(g_ag.wnd, 1, 530, nullptr);
}

// ---------- avisos (10 e 5 min antes) ----------

struct Reminder {
    HWND wnd = nullptr;
    AgendaItem item;
    int level = 10;          // 10, 5, 0 (na hora); 10 = discreto e some sozinho
    ULONGLONG shownAt = 0;
    int hot = -1;
    D2D1_RECT_F r[3]{};      // 0 primário, 1 secundário, 2 fechar
    float s = 1;
};
static Reminder g_rem;
static std::set<std::wstring> g_remFired;
static std::vector<std::pair<LONGLONG, AgendaItem>> g_remSnooze; // minuto em que avisa de novo

static void ReminderRender() {
    HWND w = g_rem.wnd;
    if (!w) return;
    const ui::Theme& th = g_theme;
    float s = g_rem.s, W = 400 * s, H = 118 * s, M = 16 * s;
    UINT CW = (UINT)(W + 2 * M), CH = (UINT)(H + 2 * M);
    fmt::Canvas cv(CW, CH);
    if (!cv.rt) return;
    ID2D1RenderTarget* rt = cv.rt;
    rt->Clear(D2D1::ColorF(0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ui::Painter p(rt, g_dw, th, s);
    D2D1_COLOR_F acc = AccentOf(th), onAcc = OnAccentOf(th);
    bool urgent = g_rem.level <= 5;
    D2D1_RECT_F card = D2D1::RectF(M, M, M + W, M + H);
    float rad = 22 * s;
    p.Shadow(card, rad);
    if (urgent && th.neon) p.Glow(card, rad);
    p.Fill(card, rad, th.classic ? ui::Rgba(0x4A4F57) : th.neon ? ui::Rgba(0x0E0E10, 0.97f) : th.dark ? ui::Rgba(0x161618, 0.98f) : ui::Rgba(0xFAFAFC, 0.98f));
    p.Stroke(card, rad, urgent ? acc : th.classic ? ui::Rgba(0x5C6472) : th.neon ? ui::Rgba(ui::NeonColor(), 0.42f) : th.stroke, (urgent ? 1.3f : 1) * s);
    float x = card.left + 18 * s, y = card.top + 16 * s;
    D2D1_RECT_F ic = D2D1::RectF(x, y, x + 42 * s, y + 42 * s);
    p.Fill(ic, 21 * s, urgent ? acc : (th.classic ? ui::Rgba(0x23272E) : th.neon ? ui::Rgba(ui::NeonColor(), 0.14f) : th.dark ? ui::Rgba(0xFFFFFF, 0.08f) : ui::Rgba(0x000000, 0.06f)));
    p.FeatherIcon(urgent ? "timer" : "calendar", ic, 20 * s, urgent ? onAcc : acc);
    float tx = x + 56 * s;
    SYSTEMTIME now; GetLocalTime(&now);
    LONGLONG left = Minutes(g_rem.item.when) - Minutes(now);
    wchar_t lab[64];
    if (left <= 0) swprintf(lab, 64, T(L"AGORA · %02d:%02d", L"NOW · %02d:%02d", L"AHORA · %02d:%02d"), g_rem.item.when.wHour, g_rem.item.when.wMinute);
    else if (left == 1) swprintf(lab, 64, T(L"EM 1 MINUTO · %02d:%02d", L"IN 1 MINUTE · %02d:%02d", L"EN 1 MINUTO · %02d:%02d"), g_rem.item.when.wHour, g_rem.item.when.wMinute);
    else swprintf(lab, 64, T(L"EM %d MINUTOS · %02d:%02d", L"IN %d MINUTES · %02d:%02d", L"EN %d MINUTOS · %02d:%02d"), (int)left, g_rem.item.when.wHour, g_rem.item.when.wMinute);
    p.Text(lab, D2D1::RectF(tx, y, card.right - 44 * s, y + 16 * s), 10.5f, acc, DWRITE_FONT_WEIGHT_BOLD);
    p.Text(g_rem.item.title, D2D1::RectF(tx, y + 18 * s, card.right - 20 * s, y + 42 * s), 15.5f, th.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_rem.r[2] = D2D1::RectF(card.right - 40 * s, card.top + 10 * s, card.right - 14 * s, card.top + 36 * s);
    if (g_rem.hot == 2) p.Fill(g_rem.r[2], 13 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.1f) : ui::Rgba(0x000000, 0.06f));
    p.FeatherIcon("x", g_rem.r[2], 14 * s, th.text2);
    float by = y + 52 * s;
    const wchar_t* b1 = urgent ? L"Ok" : T(L"Dispensar", L"Dismiss", L"Descartar");
    const wchar_t* b2 = urgent ? T(L"Adiar 5 min", L"Snooze 5 min", L"Posponer 5 min") : T(L"Abrir agenda", L"Open agenda", L"Abrir agenda");
    float w1 = p.Measure(b1, 12.5f, DWRITE_FONT_WEIGHT_BOLD) + 30 * s, w2 = p.Measure(b2, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 30 * s;
    g_rem.r[0] = D2D1::RectF(tx, by, tx + w1, by + 30 * s);
    g_rem.r[1] = D2D1::RectF(tx + w1 + 8 * s, by, tx + w1 + 8 * s + w2, by + 30 * s);
    if (urgent) p.Pill(g_rem.r[0], b1, g_rem.hot == 0, true); else p.Pill(g_rem.r[0], b1, g_rem.hot == 0);
    p.Pill(g_rem.r[1], b2, g_rem.hot == 1);
    if (!urgent) { // tempo até sumir (20 s)
        float f = 1.0f - min(1.0f, (GetTickCount64() - g_rem.shownAt) / 20000.0f);
        ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        if (SUCCEEDED(fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(card, rad, rad), &clip))) {
            ID2D1Layer* layer = nullptr;
            if (SUCCEEDED(rt->CreateLayer(&layer))) {
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
                p.Fill(D2D1::RectF(card.left, card.bottom - 3 * s, card.left + (card.right - card.left) * f, card.bottom), 0, acc);
                rt->PopLayer(); layer->Release();
            }
            clip->Release();
        }
        fac->Release();
    }
    HBITMAP bmp = nullptr;
    if (FAILED(cv.Finish(&bmp)) || !bmp) return;
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    float t = min(1.0f, (GetTickCount64() - g_rem.shownAt) / 220.0f), e = 1 - (1 - t) * (1 - t) * (1 - t); // desliza para cima
    POINT pos = {mi.rcWork.right - (LONG)CW - (LONG)(4 * s), mi.rcWork.bottom - (LONG)CH - (LONG)(4 * s) + (LONG)((1 - e) * 40 * s)}, zero = {0, 0};
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    SIZE size = {(LONG)CW, (LONG)CH};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(255 * e), AC_SRC_ALPHA};
    UpdateLayeredWindow(w, screen, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteDC(mem); ReleaseDC(nullptr, screen); DeleteObject(bmp);
}

static void ReminderClose() { if (g_rem.wnd) { HWND w = g_rem.wnd; g_rem.wnd = nullptr; DestroyWindow(w); } }

static LRESULT CALLBACK ReminderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        ULONGLONG age = GetTickCount64() - g_rem.shownAt;
        if (g_rem.level > 5 && age > 20000 && g_rem.hot < 0) { ReminderClose(); return 0; }
        ReminderRender();
        if (age > 260) { KillTimer(h, 1); SetTimer(h, 1, 250, nullptr); } // depois da animação, só atualiza a barra
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hot = -1;
        for (int i = 0; i < 3; ++i) if (ui::Hit(g_rem.r[i], pt)) hot = i;
        if (hot != g_rem.hot) { g_rem.hot = hot; ReminderRender(); }
        TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, h, 0}; TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE: g_rem.hot = -1; ReminderRender(); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_rem.hot >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE;
    case WM_LBUTTONUP: {
        int hit = g_rem.hot;
        bool urgent = g_rem.level <= 5;
        if (hit == 1 && urgent) { // adiar 5 min
            SYSTEMTIME now; GetLocalTime(&now);
            g_remSnooze.push_back({Minutes(now) + 5, g_rem.item});
            ReminderClose();
        } else if (hit == 1) { ReminderClose(); OpenAgendaWindow(); }
        else if (hit == 0 || hit == 2) ReminderClose();
        return 0;
    }
    case WM_DESTROY: if (g_rem.wnd == h) g_rem.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowReminder(const AgendaItem& a, int level) {
    ReminderClose();
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = ReminderProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsReminder"; RegisterClassW(&wc); reg = true; }
    g_rem.item = a; g_rem.level = level; g_rem.shownAt = GetTickCount64(); g_rem.hot = -1;
    g_rem.s = GetDpiForSystem() / 96.0f;
    g_rem.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, L"DesignThumbsReminder", L"Vizu", WS_POPUP,
                                0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    ReminderRender();
    ShowWindow(g_rem.wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_rem.wnd, 1, 15, nullptr);
    MessageBeep(MB_ICONASTERISK);
}

// Chamado a cada segundo pelo relógio: dispara os avisos de 10 min, 5 min e na hora (cada um uma vez).
static void CheckReminders() {
    static ULONGLONG lastLoad = 0;
    static std::vector<AgendaItem> cache;
    if (GetTickCount64() - lastLoad > 10000 || lastLoad == 0) { cache = LoadAgenda(); lastLoad = GetTickCount64(); }
    if (g_ag.wnd) cache = LoadAgenda(), lastLoad = GetTickCount64(); // a agenda pode ter acabado de mudar
    SYSTEMTIME now; GetLocalTime(&now);
    LONGLONG m = Minutes(now);
    for (size_t i = 0; i < g_remSnooze.size(); ++i)
        if (m >= g_remSnooze[i].first) { AgendaItem a = g_remSnooze[i].second; g_remSnooze.erase(g_remSnooze.begin() + i); ShowReminder(a, 5); return; }
    for (auto& a : cache) {
        LONGLONG start = Minutes(a.when);
        const int levels[3] = {10, 5, 0}, bits[3] = {kAlert10, kAlert5, kAlert0};
        for (int k = 2; k >= 0; --k) { // o mais urgente primeiro
            if (!(a.alerts & bits[k])) continue;
            LONGLONG at = start - levels[k];
            if (m < at || m > at + 1) continue; // janela de 2 minutos
            wchar_t key[160]; swprintf(key, 160, L"%lld|%d|%ls", (long long)start, levels[k], a.title.c_str());
            if (g_remFired.count(key)) continue;
            g_remFired.insert(key);
            for (int j = k + 1; j < 3; ++j) { wchar_t k2[160]; swprintf(k2, 160, L"%lld|%d|%ls", (long long)start, levels[j], a.title.c_str()); g_remFired.insert(k2); }
            ShowReminder(a, levels[k]);
            return;
        }
    }
}
