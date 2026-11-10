// OcrUi.inc.h - "Ler texto da tela": congela a tela, o usuário arrasta sobre o texto, o OCR do Windows lê,
// o texto vai para a área de transferência e um cartão oferece tradução para PT / ES / EN (Google Tradutor,
// com o MyMemory de reserva). Incluído no Vizu.cpp depois do Clock.inc.h.

#include "Ocr.h"
#include <shellscalingapi.h>

static const UINT WM_OCR_TRANSLATED = WM_APP + 60;

// ---------- tradução ----------
static std::string ToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &r[0], n, nullptr, nullptr);
    return r;
}
static std::wstring FromUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n);
    return r;
}
static std::wstring UrlEncode(const std::wstring& w) {
    std::string u = ToUtf8(w);
    std::wstring r;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (wchar_t)c;
        else { wchar_t b[4]; swprintf(b, 4, L"%%%02X", c); r += b; }
    }
    return r;
}
// Lê uma string JSON começando nas aspas em j[i]; avança i para depois dela.
static std::string JsonStr(const std::string& j, size_t& i) {
    std::string out;
    if (i >= j.size() || j[i] != '"') return out;
    ++i;
    while (i < j.size() && j[i] != '"') {
        char c = j[i++];
        if (c != '\\' || i >= j.size()) { out += c; continue; }
        char e = j[i++];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = (unsigned)strtoul(j.substr(i, 4).c_str(), nullptr, 16); i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= j.size() && j[i] == '\\' && j[i + 1] == 'u') {
                unsigned lo = (unsigned)strtoul(j.substr(i + 2, 4).c_str(), nullptr, 16); i += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            std::wstring w;
            if (cp >= 0x10000) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
            else w += (wchar_t)cp;
            out += ToUtf8(w);
            break;
        }
        default: out += e;
        }
    }
    ++i;
    return out;
}
// Google Tradutor (acesso gratuito, não oficial). Resposta: [[["tradução","original",...],...],null,"en",...]
static bool TranslateGoogle(const std::wstring& text, const wchar_t* to, std::wstring* out, std::wstring* from) {
    std::wstring path = std::wstring(L"/translate_a/single?client=gtx&sl=auto&tl=") + to + L"&dt=t&q=" + UrlEncode(text);
    std::string j = upd::HttpsGet(L"translate.googleapis.com", path.c_str());
    if (j.size() < 4 || j[0] != '[') return false;
    std::string res;
    int depth = 0, elem3 = -1; // elem3: posição do item dentro do array de nível 3
    int top = 0;               // índice do elemento no nível 1
    for (size_t i = 0; i < j.size();) {
        char c = j[i];
        if (c == '"') {
            size_t at = i;
            std::string s = JsonStr(j, i);
            if (depth == 3 && elem3 == 0 && top == 0) res += s;
            if (depth == 1 && top == 2 && from) *from = FromUtf8(s);
            if (depth == 3) elem3++;
            (void)at;
            continue;
        }
        if (c == '[') { depth++; if (depth == 3) elem3 = 0; }
        else if (c == ']') depth--;
        else if (c == ',') { if (depth == 1) top++; else if (depth == 3) { if (elem3 == 0) elem3 = 1; } }
        else if (depth == 3 && c != ' ' && elem3 == 0) elem3 = 1; // primeiro item não é string
        ++i;
    }
    if (res.empty()) return false;
    *out = FromUtf8(res);
    return true;
}
// Reserva: MyMemory (gratuito, com limite diário).
static bool TranslateMyMemory(const std::wstring& text, const std::wstring& src, const wchar_t* to, std::wstring* out) {
    std::wstring s = src.empty() ? L"en" : src.substr(0, 2);
    if (s == to) return false;
    std::wstring path = L"/get?q=" + UrlEncode(text.substr(0, 480)) + L"&langpair=" + s + L"%7C" + to;
    std::string j = upd::HttpsGet(L"api.mymemory.translated.net", path.c_str());
    size_t p = j.find("\"translatedText\"");
    if (p == std::string::npos) return false;
    p = j.find('"', j.find(':', p + 16) + 1);
    if (p == std::string::npos) return false;
    std::string r = JsonStr(j, p);
    if (r.empty()) return false;
    *out = FromUtf8(r);
    return true;
}

// ---------- cartão do resultado ----------
struct OcrCard {
    HWND wnd = nullptr;
    std::wstring original, shown, lang, status;
    int state = 0;            // 0 copiado, 1 traduzindo, 2 traduzido, 3 erro
    std::wstring target;      // pt / es / en
    bool joined = false;
    int hot = -1;
    D2D1_RECT_F r[8]{};       // 0 PT, 1 ES, 2 EN, 3 juntar, 4 abrir no Google, 5 fechar, 6 copiar original
    POINT anchor{};           // canto da seleção
    float s = 1;
    ULONGLONG lastHover = 0;
};
static OcrCard g_card;
enum { kCardPt, kCardEs, kCardEn, kCardJoin, kCardWeb, kCardClose, kCardOrig };

static std::wstring LangName(const std::wstring& tag) {
    std::wstring t = tag.substr(0, 2);
    if (t == L"pt") return T(L"português", L"Portuguese", L"portugués");
    if (t == L"en") return T(L"inglês", L"English", L"inglés");
    if (t == L"es") return T(L"espanhol", L"Spanish", L"español");
    if (t == L"fr") return T(L"francês", L"French", L"francés");
    if (t == L"de") return T(L"alemão", L"German", L"alemán");
    if (t == L"it") return T(L"italiano", L"Italian", L"italiano");
    return tag;
}
static std::wstring JoinLines(const std::wstring& t) {
    std::wstring r;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'\r') continue;
        if (t[i] == L'\n') { if (!r.empty() && r.back() == L'-') r.pop_back(); else if (!r.empty() && r.back() != L' ') r += L' '; continue; }
        r += t[i];
    }
    return r;
}

static void CardRender() {
    HWND w = g_card.wnd;
    if (!w) return;
    const ui::Theme& th = g_theme;
    float s = g_card.s, W = 440 * s, pad = 16 * s, M = 14 * s;
    D2D1_COLOR_F accent = th.neon ? ui::Rgba(ui::NeonColor()) : g_cfg.themeMode == ui::kThemeWindows ? th.accent : th.dark ? ui::Rgba(0xE6E6E8) : ui::Rgba(0x1D1D1F);
    D2D1_COLOR_F onAccent = th.neon || (th.dark && g_cfg.themeMode != ui::kThemeWindows) ? ui::Rgba(0x0E0E10) : ui::Rgba(0xFFFFFF);
    std::wstring disp = g_card.shown.size() > 700 ? g_card.shown.substr(0, 700) + L"…" : g_card.shown;
    // mede o texto
    float textW = W - 2 * pad - 24 * s, textH = 20 * s;
    {
        fmt::Canvas probe(8, 8);
        if (probe.rt) { ui::Painter pp(probe.rt, g_dw, th, s); textH = min(pp.Paragraph(disp, 0, 0, textW, 13, th.text, false), 190 * s); }
    }
    float H = pad + 20 * s + 10 * s + textH + 24 * s + 12 * s + 32 * s + (g_card.state == 2 ? 22 * s : 0) + pad;
    UINT CW = (UINT)(W + 2 * M), CH = (UINT)(H + 2 * M);
    fmt::Canvas cv(CW, CH);
    if (!cv.rt) return;
    ID2D1RenderTarget* rt = cv.rt;
    rt->Clear(D2D1::ColorF(0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ui::Painter p(rt, g_dw, th, s);
    D2D1_RECT_F card = D2D1::RectF(M, M, M + W, M + H);
    float rad = 22 * s;
    p.Shadow(card, rad);
    p.Fill(card, rad, th.neon ? ui::Rgba(0x0E0E10, 0.96f) : th.dark ? ui::Rgba(0x161618, 0.97f) : ui::Rgba(0xFAFAFC, 0.98f));
    p.Stroke(card, rad, th.neon ? ui::Rgba(ui::NeonColor(), 0.42f) : th.stroke, 1 * s);
    float x = card.left + pad, y = card.top + pad;
    D2D1_COLOR_F ok = th.dark ? ui::Rgba(0x30D158) : ui::Rgba(0x248A3D), bad = ui::Rgba(0xFF453A);
    // cabeçalho
    std::wstring st = g_card.status;
    D2D1_COLOR_F stc = g_card.state == 3 ? bad : g_card.state == 1 ? th.text2 : ok;
    if (g_card.state != 1) p.FeatherIcon(g_card.state == 3 ? "alert-circle" : "check", D2D1::RectF(x, y, x + 18 * s, y + 20 * s), 16 * s, stc);
    p.Text(st, D2D1::RectF(x + (g_card.state == 1 ? 0 : 24 * s), y, card.right - pad - 30 * s, y + 20 * s), 12.5f, stc, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_card.r[kCardClose] = D2D1::RectF(card.right - pad - 24 * s, y - 2 * s, card.right - pad, y + 22 * s);
    if (g_card.hot == kCardClose) p.Fill(g_card.r[kCardClose], 12 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.10f) : ui::Rgba(0x000000, 0.06f));
    p.FeatherIcon("x", g_card.r[kCardClose], 15 * s, th.text2);
    y += 30 * s;
    // texto
    D2D1_RECT_F box = D2D1::RectF(x, y, card.right - pad, y + textH + 24 * s);
    p.Fill(box, 14 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.05f) : ui::Rgba(0x000000, 0.04f));
    {
        ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
        ID2D1RectangleGeometry* clip = nullptr;
        if (SUCCEEDED(fac->CreateRectangleGeometry(D2D1::RectF(box.left, box.top + 10 * s, box.right, box.bottom - 10 * s), &clip))) {
            ID2D1Layer* layer = nullptr;
            if (SUCCEEDED(rt->CreateLayer(&layer))) {
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
                p.Paragraph(disp, box.left + 12 * s, box.top + 12 * s, textW, 13, th.text);
                rt->PopLayer(); layer->Release();
            }
            clip->Release();
        }
        fac->Release();
    }
    y = box.bottom + 12 * s;
    // botões
    p.Text(T(L"Traduzir:", L"Translate:", L"Traducir:"), D2D1::RectF(x, y, x + 64 * s, y + 32 * s), 12, th.text2);
    const wchar_t* langs[3] = {L"PT", L"ES", L"EN"};
    const wchar_t* tags[3] = {L"pt", L"es", L"en"};
    float bx = x + 66 * s;
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F b = D2D1::RectF(bx, y, bx + 46 * s, y + 32 * s);
        g_card.r[kCardPt + i] = b;
        bool sel = g_card.state >= 1 && g_card.target == tags[i];
        bool same = g_card.lang.substr(0, 2) == tags[i] && !sel;
        if (sel) p.Fill(b, 16 * s, accent);
        else { if (g_card.hot == kCardPt + i) p.Fill(b, 16 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.08f) : ui::Rgba(0x000000, 0.05f)); p.Stroke(b, 16 * s, th.neon ? ui::Rgba(ui::NeonColor(), 0.5f) : th.dark ? ui::Rgba(0xFFFFFF, 0.16f) : ui::Rgba(0x000000, 0.12f), 1 * s); }
        D2D1_COLOR_F tc = sel ? onAccent : th.text; if (same) tc.a = 0.45f;
        p.Text(langs[i], b, 13, tc, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        bx += 54 * s;
    }
    float rx = card.right - pad;
    for (int k : {kCardWeb, kCardJoin}) {
        D2D1_RECT_F b = D2D1::RectF(rx - 32 * s, y, rx, y + 32 * s);
        g_card.r[k] = b;
        bool on = k == kCardJoin && g_card.joined;
        if (on) p.Fill(b, 16 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.14f) : ui::Rgba(0x000000, 0.08f));
        else if (g_card.hot == k) p.Fill(b, 16 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.08f) : ui::Rgba(0x000000, 0.05f));
        p.Stroke(b, 16 * s, th.dark ? ui::Rgba(0xFFFFFF, 0.16f) : ui::Rgba(0x000000, 0.12f), 1 * s);
        p.FeatherIcon(k == kCardWeb ? "external-link" : "align-justify", b, 15 * s, th.text);
        rx -= 40 * s;
    }
    y += 32 * s;
    g_card.r[kCardOrig] = D2D1::RectF(0, 0, 0, 0);
    if (g_card.state == 2) {
        D2D1_RECT_F o = D2D1::RectF(x, y + 6 * s, x + 160 * s, y + 22 * s);
        g_card.r[kCardOrig] = o;
        p.Text(T(L"Copiar o original", L"Copy the original", L"Copiar el original"), o, 11.5f, g_card.hot == kCardOrig ? th.text : th.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    HBITMAP bmp = nullptr;
    if (FAILED(cv.Finish(&bmp)) || !bmp) return;
    // posição: abaixo da seleção, dentro da área de trabalho do monitor
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(g_card.anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int px = max((int)mi.rcWork.left, min((int)mi.rcWork.right - (int)CW, (int)g_card.anchor.x - (int)M));
    int py = max((int)mi.rcWork.top, min((int)mi.rcWork.bottom - (int)CH, (int)g_card.anchor.y));
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT pos = {px, py}, zero = {0, 0};
    SIZE size = {(LONG)CW, (LONG)CH};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(w, screen, &pos, &size, mem, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteDC(mem); ReleaseDC(nullptr, screen); DeleteObject(bmp);
}

struct TransResult { std::wstring text, from, to; bool ok; };

static void CardTranslate(const wchar_t* to) {
    if (g_card.state == 1) return;
    g_card.state = 1; g_card.target = to;
    g_card.status = T(L"Traduzindo…", L"Translating…", L"Traduciendo…");
    CardRender();
    std::wstring text = g_card.joined ? JoinLines(g_card.original) : g_card.original, lang = g_card.lang, tgt = to;
    HWND w = g_card.wnd;
    std::thread([text, lang, tgt, w] {
        auto* r = new TransResult{L"", L"", tgt, false};
        std::wstring t = text.size() > 4500 ? text.substr(0, 4500) : text;
        r->ok = TranslateGoogle(t, tgt.c_str(), &r->text, &r->from) || TranslateMyMemory(t, lang, tgt.c_str(), &r->text);
        if (!PostMessage(w, WM_OCR_TRANSLATED, 0, (LPARAM)r)) delete r;
    }).detach();
}

static void CardClose() { if (g_card.wnd) { DestroyWindow(g_card.wnd); g_card.wnd = nullptr; } }

static int CardHit(POINT pt) {
    for (int i = 0; i < 7; ++i) {
        const D2D1_RECT_F& r = g_card.r[i];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) return i;
    }
    return -1;
}

static LRESULT CALLBACK CardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_card.lastHover = GetTickCount64();
        int hot = CardHit(pt);
        if (hot != g_card.hot) { g_card.hot = hot; CardRender(); }
        TRACKMOUSEEVENT te = {sizeof(te), TME_LEAVE, h, 0}; TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE: g_card.hot = -1; g_card.lastHover = GetTickCount64(); CardRender(); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, g_card.hot >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = CardHit(pt);
        if (hit < 0) { ReleaseCapture(); SendMessage(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0; }
        static const wchar_t* tags[3] = {L"pt", L"es", L"en"};
        if (hit <= kCardEn) CardTranslate(tags[hit]);
        else if (hit == kCardJoin) {
            g_card.joined = !g_card.joined;
            if (g_card.state != 2) { g_card.shown = g_card.joined ? JoinLines(g_card.original) : g_card.original; CopyText(g_card.shown); }
            CardRender();
        } else if (hit == kCardWeb) {
            std::wstring t = g_card.joined ? JoinLines(g_card.original) : g_card.original;
            std::wstring url = L"https://translate.google.com/?sl=auto&tl=" + (g_card.target.empty() ? std::wstring(L"pt") : g_card.target) + L"&op=translate&text=" + UrlEncode(t.substr(0, 4000));
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (hit == kCardClose) CardClose();
        else if (hit == kCardOrig) {
            CopyText(g_card.joined ? JoinLines(g_card.original) : g_card.original);
            g_card.status = T(L"Original copiado", L"Original copied", L"Original copiado"); g_card.state = 0;
            g_card.shown = g_card.joined ? JoinLines(g_card.original) : g_card.original;
            CardRender();
        }
        return 0;
    }
    case WM_OCR_TRANSLATED: {
        auto* r = (TransResult*)lp;
        if (r->to == g_card.target) {
            if (r->ok) {
                g_card.state = 2; g_card.shown = r->text; CopyText(r->text);
                std::wstring from = r->from.empty() ? g_card.lang : r->from;
                g_card.status = std::wstring(T(L"Tradução copiada · ", L"Translation copied · ", L"Traducción copiada · ")) + LangName(from) + L" \u203A " + LangName(r->to);
            } else {
                g_card.state = 3;
                g_card.status = T(L"Não deu para traduzir agora (sem internet?)", L"Couldn't translate now (offline?)", L"No se pudo traducir ahora (¿sin internet?)");
            }
            CardRender();
        }
        delete r;
        return 0;
    }
    case WM_TIMER: // some sozinho após 15 s sem o mouse em cima
        if (g_card.hot < 0 && g_card.state != 1 && GetTickCount64() - g_card.lastHover > 15000) {
            POINT pt; GetCursorPos(&pt); RECT wr; GetWindowRect(h, &wr);
            if (!PtInRect(&wr, pt)) CardClose();
        }
        return 0;
    case WM_DESTROY: if (g_card.wnd == h) g_card.wnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowCard(const std::wstring& text, const std::wstring& lang, POINT anchor) {
    CardClose();
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = CardProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsOcrCard"; RegisterClassW(&wc); reg = true; }
    g_card = OcrCard();
    g_card.original = text; g_card.lang = lang; g_card.anchor = anchor;
    g_card.joined = ReadDword(L"OcrJoin", 1) != 0;
    g_card.shown = g_card.joined ? JoinLines(text) : text;
    g_card.status = std::wstring(T(L"Copiado", L"Copied", L"Copiado")) + (lang.empty() ? L"" : L" · " + LangName(lang));
    UINT dpix = 96, dpiy = 96;
    GetDpiForMonitor(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpix, &dpiy);
    g_card.s = dpix / 96.0f;
    g_card.lastHover = GetTickCount64();
    g_card.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, L"DesignThumbsOcrCard", L"OCR", WS_POPUP,
                                 anchor.x, anchor.y, 10, 10, nullptr, nullptr, g_inst, nullptr);
    CardRender();
    ShowWindow(g_card.wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_card.wnd, 1, 1000, nullptr);
}

// ---------- seleção na tela congelada ----------
struct OcrSel {
    HWND wnd = nullptr;
    HBITMAP shot = nullptr, dim = nullptr;
    BYTE* shotBits = nullptr;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    bool drag = false;
    POINT a{}, b{};
    COLORREF accent = RGB(255, 255, 255);
};
static OcrSel g_sel;

static void SelClose() {
    if (g_sel.wnd) { HWND w = g_sel.wnd; g_sel.wnd = nullptr; DestroyWindow(w); }
    if (g_sel.shot) DeleteObject(g_sel.shot);
    if (g_sel.dim) DeleteObject(g_sel.dim);
    g_sel.shot = g_sel.dim = nullptr; g_sel.shotBits = nullptr;
}

static RECT SelRect() {
    RECT r = {min(g_sel.a.x, g_sel.b.x), min(g_sel.a.y, g_sel.b.y), max(g_sel.a.x, g_sel.b.x), max(g_sel.a.y, g_sel.b.y)};
    return r;
}

static void SelFinish() {
    RECT r = SelRect();
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 6 || h < 6 || !g_sel.shotBits) { SelClose(); return; }
    // recorte (ampliado quando o texto é pequeno: o OCR acerta mais)
    int k = (h < 120 || w < 300) ? 3 : (h < 300 ? 2 : 1);
    while (k > 1 && (w * k > 4000 || h * k > 4000)) --k;
    int ow = w * k, oh = h * k;
    std::vector<BYTE> px((size_t)ow * oh * 4);
    {
        HDC scr = GetDC(nullptr), src = CreateCompatibleDC(scr), dst = CreateCompatibleDC(scr);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = ow; bi.bmiHeader.biHeight = -oh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP out = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ o1 = SelectObject(src, g_sel.shot), o2 = SelectObject(dst, out);
        SetStretchBltMode(dst, HALFTONE); SetBrushOrgEx(dst, 0, 0, nullptr);
        StretchBlt(dst, 0, 0, ow, oh, src, r.left, r.top, w, h, SRCCOPY);
        GdiFlush();
        memcpy(px.data(), bits, px.size());
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        SelectObject(src, o1); SelectObject(dst, o2);
        DeleteObject(out); DeleteDC(src); DeleteDC(dst); ReleaseDC(nullptr, scr);
    }
    POINT anchor = {g_sel.vx + r.left, g_sel.vy + r.bottom + 8};
    SelClose();
    std::wstring text, lang;
    bool keep = ReadDword(L"OcrJoin", 1) == 0;
    if (!ocr::Recognize(px.data(), ow, oh, ow * 4, true, &text, &lang)) {
        MessageBoxW(nullptr, T(L"O leitor de texto do Windows não está disponível neste PC.\n\nEle vem no Windows 10 e 11; confira em Configurações › Hora e idioma › Idioma se há um idioma com \"Reconhecimento óptico de caracteres\".",
                               L"The Windows text reader isn't available on this PC.\n\nIt ships with Windows 10 and 11; check Settings › Time & language › Language for a language with \"Optical character recognition\".",
                               L"El lector de texto de Windows no está disponible en este PC.\n\nViene con Windows 10 y 11; revisa en Configuración › Hora e idioma › Idioma si hay un idioma con \"Reconocimiento óptico de caracteres\"."),
                    L"Vizu", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    (void)keep;
    if (text.empty()) {
        ShowToast(T(L"Nenhum texto encontrado", L"No text found", L"No se encontró texto"), T(L"Tente selecionar uma área maior ou mais nítida.", L"Try a larger or sharper area.", L"Prueba un área más grande o más nítida."), 4000, 2);
        return;
    }
    std::wstring copy = ReadDword(L"OcrJoin", 1) ? JoinLines(text) : text;
    CopyText(copy);
    ShowCard(text, lang, anchor);
}

static LRESULT CALLBACK SelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc), src = CreateCompatibleDC(dc);
        HBITMAP back = CreateCompatibleBitmap(dc, g_sel.vw, g_sel.vh);
        HGDIOBJ ob = SelectObject(mem, back), os = SelectObject(src, g_sel.dim);
        BitBlt(mem, 0, 0, g_sel.vw, g_sel.vh, src, 0, 0, SRCCOPY);
        RECT r = SelRect();
        if (g_sel.drag && r.right > r.left && r.bottom > r.top) {
            SelectObject(src, g_sel.shot);
            BitBlt(mem, r.left, r.top, r.right - r.left, r.bottom - r.top, src, r.left, r.top, SRCCOPY);
            HPEN pen = CreatePen(PS_SOLID, 2, g_sel.accent);
            HGDIOBJ op = SelectObject(mem, pen), obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
            SelectObject(mem, op); SelectObject(mem, obr); DeleteObject(pen);
            wchar_t sz[32]; swprintf(sz, 32, L" %d × %d ", (int)(r.right - r.left), (int)(r.bottom - r.top));
            SetBkColor(mem, g_sel.accent); SetTextColor(mem, GetRValue(g_sel.accent) + GetGValue(g_sel.accent) + GetBValue(g_sel.accent) > 450 ? RGB(14, 14, 16) : RGB(255, 255, 255));
            TextOutW(mem, r.left, max(0L, r.top - 20), sz, (int)wcslen(sz));
        }
        // dica no alto da tela principal
        const wchar_t* hint = T(L"   Arraste sobre o texto  ·  Esc cancela   ", L"   Drag over the text  ·  Esc cancels   ", L"   Arrastra sobre el texto  ·  Esc cancela   ");
        SetBkColor(mem, RGB(20, 20, 22)); SetTextColor(mem, RGB(232, 232, 234));
        SIZE ts; GetTextExtentPoint32W(mem, hint, (int)wcslen(hint), &ts);
        TextOutW(mem, -g_sel.vx + GetSystemMetrics(SM_CXSCREEN) / 2 - ts.cx / 2, -g_sel.vy + 24, hint, (int)wcslen(hint));
        BitBlt(dc, 0, 0, g_sel.vw, g_sel.vh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); SelectObject(src, os);
        DeleteObject(back); DeleteDC(mem); DeleteDC(src);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_CROSS)); return TRUE;
    case WM_LBUTTONDOWN:
        g_sel.drag = true; g_sel.a = g_sel.b = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (g_sel.drag) { g_sel.b = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (g_sel.drag) { g_sel.drag = false; ReleaseCapture(); g_sel.b = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; SelFinish(); }
        return 0;
    case WM_RBUTTONUP: SelClose(); return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) SelClose(); return 0;
    case WM_KILLFOCUS: if (!g_sel.drag) { /* continua aberto: alguns PCs tiram o foco ao abrir */ } return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void StartOcr() {
    if (g_sel.wnd) return;
    CardClose();
    g_sel = OcrSel();
    g_sel.vx = GetSystemMetrics(SM_XVIRTUALSCREEN); g_sel.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_sel.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN); g_sel.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_sel.vw <= 0 || g_sel.vh <= 0) return;
    // esconde o relógio e o cartão da captura
    bool clockVis = g_clock.wnd && IsWindowVisible(g_clock.wnd);
    if (clockVis) ShowWindow(g_clock.wnd, SW_HIDE);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = g_sel.vw; bi.bmiHeader.biHeight = -g_sel.vh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; void* dbits = nullptr;
    g_sel.shot = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_sel.dim = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &dbits, nullptr, 0);
    if (g_sel.shot && g_sel.dim) {
        HGDIOBJ o = SelectObject(mem, g_sel.shot);
        BitBlt(mem, 0, 0, g_sel.vw, g_sel.vh, scr, g_sel.vx, g_sel.vy, SRCCOPY | CAPTUREBLT);
        SelectObject(mem, o);
        GdiFlush();
        g_sel.shotBits = (BYTE*)bits;
        size_t n = (size_t)g_sel.vw * g_sel.vh * 4;
        const BYTE* s0 = (const BYTE*)bits; BYTE* d0 = (BYTE*)dbits;
        for (size_t i = 0; i < n; ++i) d0[i] = (BYTE)(s0[i] * 52 / 100); // escurece ~48%
    }
    DeleteDC(mem); ReleaseDC(nullptr, scr);
    if (clockVis) ShowWindow(g_clock.wnd, SW_SHOWNOACTIVATE);
    if (!g_sel.shotBits) { SelClose(); return; }
    const ui::Theme& th = g_theme;
    UINT acc = th.neon ? ui::NeonColor() : g_cfg.themeMode == ui::kThemeWindows ? ui::SystemAccent() : 0xFFFFFF;
    g_sel.accent = RGB(acc >> 16 & 255, acc >> 8 & 255, acc & 255);
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = SelProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsOcrSel"; wc.hCursor = LoadCursor(nullptr, IDC_CROSS); RegisterClassW(&wc); reg = true; }
    g_sel.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DesignThumbsOcrSel", L"OCR", WS_POPUP, g_sel.vx, g_sel.vy, g_sel.vw, g_sel.vh, nullptr, nullptr, g_inst, nullptr);
    ShowWindow(g_sel.wnd, SW_SHOW);
    SetForegroundWindow(g_sel.wnd);
    SetFocus(g_sel.wnd);
}
