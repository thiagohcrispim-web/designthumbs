// Hearts.inc.h - easter egg: corações sobem e somem pela tela por ~6 segundos (clique em "Ste" nas Configurações).
// Janela em camadas, transparente a cliques, desenhada com Direct2D num DIB e atualizada a ~60 quadros/s.

struct Heart { float x, y, size, t0, life, drift, sway, phase, rot; D2D1_COLOR_F col; };
struct HeartsFx {
    HWND wnd = nullptr;
    RECT area{};
    ULONGLONG start = 0;
    std::vector<Heart> hearts;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1PathGeometry* shape = nullptr;
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old = nullptr;
};
static HeartsFx g_hearts;
static const float kHeartsSeconds = 6.0f;

static void HeartsStop() {
    if (g_hearts.wnd) { KillTimer(g_hearts.wnd, 1); DestroyWindow(g_hearts.wnd); g_hearts.wnd = nullptr; }
    if (g_hearts.shape) { g_hearts.shape->Release(); g_hearts.shape = nullptr; }
    if (g_hearts.rt) { g_hearts.rt->Release(); g_hearts.rt = nullptr; }
    if (g_hearts.mem) { SelectObject(g_hearts.mem, g_hearts.old); DeleteDC(g_hearts.mem); g_hearts.mem = nullptr; }
    if (g_hearts.dib) { DeleteObject(g_hearts.dib); g_hearts.dib = nullptr; }
    g_hearts.hearts.clear();
}

static float EaseOutBack(float t) { const float c1 = 1.70158f, c3 = c1 + 1; float u = t - 1; return 1 + c3 * u * u * u + c1 * u * u; }
static float Smooth(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

static void HeartsFrame() {
    if (!g_hearts.rt) return;
    float now = (GetTickCount64() - g_hearts.start) / 1000.0f;
    if (now > kHeartsSeconds) { HeartsStop(); return; }
    int W = g_hearts.area.right - g_hearts.area.left, H = g_hearts.area.bottom - g_hearts.area.top;
    RECT rc = {0, 0, W, H};
    ID2D1DCRenderTarget* rt = g_hearts.rt;
    rt->BindDC(g_hearts.mem, &rc);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0));
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0), &br);
    for (auto& h : g_hearts.hearts) {
        float t = (now - h.t0) / h.life;
        if (t <= 0 || t >= 1) continue;
        float pop = EaseOutBack(Smooth(t / 0.22f));                 // aparece com um leve "pulo"
        float alpha = Smooth(t / 0.12f) * (1 - Smooth((t - 0.45f) / 0.55f)); // entra rápido, some devagar
        float sc = h.size / 24.0f * (0.35f + 0.65f * pop) * (1 + 0.06f * sinf(now * 6 + h.phase));
        float x = h.x + sinf(now * h.sway + h.phase) * 14.0f, y = h.y - h.drift * Smooth(t);
        float ang = sinf(now * 1.7f + h.phase) * 12.0f + h.rot;
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Rotation(ang) * D2D1::Matrix3x2F::Translation(x, y);
        // brilho suave por trás
        D2D1_COLOR_F g = h.col; g.a = alpha * 0.16f;
        br->SetColor(g);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(1.45f, 1.45f) * m);
        rt->FillGeometry(g_hearts.shape, br);
        D2D1_COLOR_F c = h.col; c.a = alpha * 0.92f;
        br->SetColor(c);
        rt->SetTransform(m);
        rt->FillGeometry(g_hearts.shape, br);
        // reflexo no alto
        D2D1_COLOR_F hi = D2D1::ColorF(1, 1, 1, alpha * 0.28f);
        br->SetColor(hi);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(-4.5f, -4.0f), 2.6f, 1.8f), br);
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    br->Release();
    rt->EndDraw();
    HDC screen = GetDC(nullptr);
    POINT pos = {g_hearts.area.left, g_hearts.area.top}, zero = {0, 0};
    SIZE size = {W, H};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hearts.wnd, screen, &pos, &size, g_hearts.mem, &zero, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK HeartsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) { HeartsFrame(); return 0; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

static void StartHearts(HWND owner) {
    HeartsStop();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    g_hearts.area = mi.rcMonitor;
    int W = mi.rcMonitor.right - mi.rcMonitor.left, H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    float dpi = GetDpiForWindow(owner) / 96.0f;
    // forma do coração (Feather "heart"), centrada na origem
    ID2D1Factory* fac = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &fac))) return;
    g_hearts.shape = ui::SvgPath(fac, "M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z", 1, -12, -12, true);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    fac->CreateDCRenderTarget(&rp, &g_hearts.rt);
    fac->Release();
    if (!g_hearts.rt || !g_hearts.shape) { HeartsStop(); return; }
    HDC screen = GetDC(nullptr);
    g_hearts.mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    g_hearts.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!g_hearts.dib) { HeartsStop(); return; }
    g_hearts.old = SelectObject(g_hearts.mem, g_hearts.dib);
    // magenta puxado para o vermelho
    static const UINT pal[] = {0xE0115F, 0xD81B60, 0xF0386B, 0xC2185B, 0xFF4D6D, 0xE91E63, 0xB0103F};
    UINT32 seed = (UINT32)GetTickCount64() | 1;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed % 10000) / 10000.0f; };
    for (int i = 0; i < 70; ++i) {
        Heart h;
        h.x = W * (0.04f + 0.92f * rnd());
        h.y = H * (0.25f + 0.70f * rnd());
        h.size = (16 + 34 * rnd() * rnd() + 8 * rnd()) * dpi;
        h.t0 = 3.6f * rnd();
        h.life = 2.0f + 1.2f * rnd();
        if (h.t0 + h.life > kHeartsSeconds - 0.1f) h.life = kHeartsSeconds - 0.1f - h.t0;
        (void)i;
        h.drift = (70 + 150 * rnd()) * dpi;
        h.sway = 1.2f + 1.6f * rnd();
        h.phase = 6.28f * rnd();
        h.rot = -10 + 20 * rnd();
        h.col = ui::Rgba(pal[(int)(rnd() * 7) % 7]);
        g_hearts.hearts.push_back(h);
    }
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = HeartsProc; wc.hInstance = g_inst; wc.lpszClassName = L"DesignThumbsHearts"; RegisterClassW(&wc); reg = true; }
    g_hearts.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DesignThumbsHearts", L"", WS_POPUP,
                                   mi.rcMonitor.left, mi.rcMonitor.top, W, H, nullptr, nullptr, g_inst, nullptr);
    g_hearts.start = GetTickCount64();
    HeartsFrame();
    ShowWindow(g_hearts.wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hearts.wnd, 1, 15, nullptr);
}
