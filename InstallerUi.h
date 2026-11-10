// One branded window for component choice, overall progress and completion.
#pragma once
#include "Ui.h"
#include "I18n.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace installui {
using Progress = std::function<void(int, const std::wstring&)>;
using Work = std::function<bool(int, const Progress&, std::wstring&)>;
struct Window {
    HWND hwnd = nullptr;
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* write = nullptr;
    ui::Surface surface;
    ui::Theme theme;
    Work work;
    std::atomic<bool>* cancel = nullptr; // pedido de cancelamento do download
    std::thread worker;
    std::mutex mutex;
    int edition = 1; // 0 Lite, 1 Vizu, 2 Ultra
    bool installed = false, success = false, preview = false;
    int page = 0, hot = 0, focus = 2, result = 0, percent = 0;
    std::wstring phase, error;
    std::vector<std::pair<int, D2D1_RECT_F>> hits;
    static constexpr UINT Changed = WM_APP + 180, Finished = WM_APP + 181;
    float Scale() const { return GetDpiForWindow(hwnd) / 96.0f; }
    // Cores do protótipo aprovado do instalador (docs/design/prototipo/Inst-*-copia).
    static D2D1_COLOR_F C(UINT rgb, float a = 1) { return ui::Rgba(rgb, a); }
    // Separa "Baixando o motor vetorial · 42 MB de 99 MB · 3,1 MB/s · ~18 s".
    static std::vector<std::wstring> Parts(const std::wstring& text) {
        std::vector<std::wstring> out; size_t start = 0;
        for (;;) { size_t at = text.find(L" · ", start); out.push_back(text.substr(start, at - start)); if (at == std::wstring::npos) break; start = at + 3; }
        return out;
    }
    void Paint() {
        surface.hwnd = hwnd;
        if (!surface.Begin(factory)) return;
        auto* rt = surface.rt;
        float s = Scale(), w = rt->GetSize().width, h = rt->GetSize().height;
        // Fundo: degradê grafite do protótipo (170°).
        {
            D2D1_GRADIENT_STOP stops[3] = {{0, C(0x283039)}, {0.5f, C(0x1B2029)}, {1, C(0x030406)}};
            ID2D1GradientStopCollection* coll = nullptr; ID2D1LinearGradientBrush* bg = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, &coll)) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(w * 0.41f, 0), D2D1::Point2F(w * 0.59f, h)), coll, &bg)))
                rt->FillRectangle(D2D1::RectF(0, 0, w, h), bg);
            else rt->Clear(C(0x1B2029));
            if (bg) bg->Release(); if (coll) coll->Release();
        }
        ui::Theme body = theme, head = theme;
        body.text = C(0xF2F2F2); body.text2 = C(0xCDD2DA); body.accent = C(0xFF6633); body.onAccent = C(0x0A0B0D); body.neon = true;
        body.font = L"Inter"; head = body; head.font = L"Space Grotesk";
        ui::Painter p(rt, write, body, s), ph(rt, write, head, s);
        const D2D1_COLOR_F text = body.text, text2 = body.text2, dim = C(0xB5BAC2), accent = body.accent, line = C(0xFFFFFF, 0.12f);
        auto rect = [&](float x, float y, float right, float bottom) { return D2D1::RectF(x*s, y*s, right*s, bottom*s); };
        float width = w / s, L = 48, R = width - 48;
        hits.clear();
        auto hit = [&](int id, D2D1_RECT_F r) { hits.push_back({id,r}); return hot == id; };
        auto primary = [&](D2D1_RECT_F r, const std::wstring& label, int id) {
            bool hov = hit(id, r);
            p.Glow(r, 12*s);
            p.Fill(r, 12*s, accent);
            if (hov) p.Fill(r, 12*s, C(0xFFFFFF, 0.14f));
            p.Text(label, r, 15, body.onAccent, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        auto outline = [&](D2D1_RECT_F r, const std::wstring& label, int id) {
            bool hov = hit(id, r);
            if (hov) p.Fill(r, 12*s, C(0xFFFFFF, 0.08f));
            p.Stroke(r, 12*s, hov ? accent : text2);
            p.Text(label, r, 14, hov ? accent : text, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        auto keycap = [&](D2D1_RECT_F r, const wchar_t* label) {
            p.Fill(D2D1::RectF(r.left, r.bottom - 5*s, r.right, r.bottom), 4*s, C(0xFF6633, 0.18f));
            p.Stroke(r, 9*s, accent, 2*s);
            ph.TextSpaced(label, r, 15, accent, DWRITE_FONT_WEIGHT_BOLD, 0.1f);
        };
        bool done = page == 2;
        ui::Brand(rt, body, L*s, (done ? 40 : 44)*s, (done ? 60 : 44)*s);
        p.Text(L"9.8.6", rect(R-120, done ? 40 : 44, R, done ? 100 : 88), 13, text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (page == 0) {
            ph.Text(installed ? T(L"Atualize ou troque a versão:",L"Update or switch edition:",L"Actualiza o cambia la versión:") : T(L"Escolha uma versão:",L"Choose an edition:",L"Elige una versión:"), rect(L,112,R,152), 32, text, DWRITE_FONT_WEIGHT_BOLD);
            p.Text(T(L"Para trocar depois, rode o instalador de novo.",L"To switch later, run the installer again.",L"Para cambiar después, ejecuta el instalador de nuevo."), rect(L,158,R,182), 15, text2);
            struct Ed { const wchar_t* name; const wchar_t* desc; const wchar_t* size; int bar; };
            Ed eds[3] = {
                {L"Lite", T(L"Prévia com Espaço e miniaturas",L"Space preview and thumbnails",L"Vista con Espacio y miniaturas"), L"2 MB", 2},
                {L"Vizu", T(L"Converter, menu circular, PDF, OCR, relógio e agenda",L"Convert, radial menu, PDF, OCR, clock and calendar",L"Convertir, menú circular, PDF, OCR, reloj y agenda"), L"2 MB", 2},
                {L"Vizu Ultra", T(L"Tudo do Vizu, mais SVG e EPS vetoriais a partir de CDR",L"All of Vizu, plus vector SVG and EPS from CDR",L"Todo lo de Vizu, más SVG y EPS vectoriales desde CDR"), T(L"+ 100 MB de download",L"+ 100 MB download",L"+ 100 MB de descarga"), 100},
            };
            float top = 206, row = 76;
            for (int i = 0; i < 3; ++i) {
                float y = top + i*row;
                auto box = rect(L, y, R, y + row);
                bool sel = edition == i, hov = hit(10+i, box);
                if (hov) p.Fill(rect(L-12, y+4, R+12, y+row-4), 10*s, C(0xFFFFFF, 0.04f));
                p.Fill(rect(L, y, R, y+1), 0, line);
                auto dot = rect(L+2, y+29, L+20, y+47);
                if (sel) {
                    p.Glow(dot, 9*s);
                    p.Fill(dot, 9*s, accent);
                    p.Fill(rect(L+6, y+33, L+16, y+43), 5*s, C(0x0A0B0D));
                    p.Fill(rect(L+8, y+35, L+14, y+41), 3*s, accent);
                } else p.Stroke(dot, 9*s, hov ? text : text2, 1.5f*s);
                float right = R - 150;
                p.Text(eds[i].name, rect(L+38, y+16, right-16, y+40), 17, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                p.Text(eds[i].desc, rect(L+38, y+40, right-16, y+60), 14, text2);
                p.Text(eds[i].size, rect(right, y+20, R, y+38), 12, i == 2 ? text : text2);
                p.Fill(rect(right, y+44, R, y+48), 2*s, line);
                float fill = eds[i].bar >= 100 ? 150 : 4;
                if (eds[i].bar >= 100 && sel) p.Glow(rect(right, y+44, right+fill, y+48), 2*s);
                p.Fill(rect(right, y+44, right+fill, y+48), 2*s, i == 0 ? text2 : accent);
            }
            p.Fill(rect(L, top+3*row, R, top+3*row+1), 0, line);
            p.Text(edition==0?T(L"O Lite não tem menu circular, conversões, OCR nem relógio.",L"Lite has no radial menu, conversions, OCR or clock.",L"Lite no tiene menú circular, conversiones, OCR ni reloj."):
                   T(L"O Ultra baixa o motor vetorial uma única vez e confere a integridade antes de instalar.",L"Ultra downloads the vector engine once and checks its integrity before installing.",L"Ultra descarga el motor vectorial una sola vez y verifica su integridad antes de instalar."),
                   rect(L, top+3*row+22, R, top+3*row+44), 13, text2);
            auto cancel = rect(L-4, 548, L+80, 596);
            bool hc = hit(3, cancel);
            p.Text(T(L"Cancelar",L"Cancel",L"Cancelar"), cancel, 15, hc ? text : text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (installed) {
                auto un = rect(L+96, 548, L+200, 596);
                p.Text(T(L"Desinstalar",L"Uninstall",L"Desinstalar"), un, 15, hit(4, un) ? body.danger : text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            const wchar_t* go = installed ? T(L"Atualizar Vizu",L"Update Vizu",L"Actualizar Vizu") : T(L"Instalar Vizu",L"Install Vizu",L"Instalar Vizu");
            float bw = p.Measure(go, 15, DWRITE_FONT_WEIGHT_BOLD) / s + 68;
            primary(rect(R-bw, 548, R, 596), go, 2);
        } else if (page == 1) {
            int value; std::wstring phaseText;
            { std::lock_guard<std::mutex> lock(mutex); value = percent; phaseText = phase; }
            auto parts = Parts(phaseText);
            bool ultra = edition == 2, downloading = ultra && parts.size() > 1;
            ph.Text(downloading ? T(L"Baixando o motor vetorial",L"Downloading the vector engine",L"Descargando el motor vectorial") : T(L"Instalando o Vizu",L"Installing Vizu",L"Instalando Vizu"), rect(L,112,R,152), 32, text, DWRITE_FONT_WEIGHT_BOLD);
            p.Text(downloading ? T(L"Acontece só na primeira vez. As atualizações reaproveitam o motor.",L"This only happens once. Updates reuse the engine.",L"Solo ocurre la primera vez. Las actualizaciones reutilizan el motor.") : T(L"Estamos preparando tudo para você.",L"We're getting everything ready for you.",L"Estamos preparando todo para ti."), rect(L,158,R,182), 15, text2);
            auto card = rect(L, 208, R, 352);
            p.Fill(card, 14*s, C(0xFFFFFF, 0.05f)); p.Stroke(card, 14*s, C(0xFFFFFF, 0.10f));
            ph.Text(std::to_wstring(value) + L"%", rect(L+22, 226, R-22, 276), 40, text, DWRITE_FONT_WEIGHT_BOLD);
            if (downloading) p.Text(parts[1], rect(L+22, 240, R-22, 270), 14, text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            auto bar = rect(L+22, 290, R-22, 300);
            p.Fill(bar, 5*s, C(0xFFFFFF, 0.10f));
            if (value > 0) { auto f = rect(L+22, 290, L+22+(R-L-44)*value/100.0f, 300); p.Glow(f, 5*s); p.Fill(f, 5*s, accent); }
            if (downloading) {
                if (parts.size() > 2) p.Text(parts[2], rect(L+22, 314, R-22, 334), 14, text2);
                if (parts.size() > 3) p.Text(parts[3] + T(L" restantes",L" left",L" restantes"), rect(L+22, 314, R-22, 334), 14, text2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else p.Text(phaseText, rect(L+22, 314, R-22, 334), 14, text2);
            const wchar_t* stepsU[4] = {T(L"Preparando a instalação",L"Preparing installation",L"Preparando la instalación"), T(L"Baixando o motor vetorial",L"Downloading the vector engine",L"Descargando el motor vectorial"), T(L"Conferindo a integridade",L"Checking integrity",L"Verificando la integridad"), T(L"Instalando arquivos e miniaturas",L"Installing files and thumbnails",L"Instalando archivos y miniaturas")};
            const wchar_t* stepsV[3] = {T(L"Preparando a instalação",L"Preparing installation",L"Preparando la instalación"), T(L"Instalando arquivos e miniaturas",L"Installing files and thumbnails",L"Instalando archivos y miniaturas"), T(L"Configurando atalhos",L"Setting up shortcuts",L"Configurando accesos")};
            int count = ultra ? 4 : 3;
            int current = ultra ? (value < 10 ? 0 : downloading ? 1 : value < 65 ? 2 : 3) : (value < 65 ? 0 : value < 85 ? 1 : 2);
            for (int i = 0; i < count; ++i) {
                float y = 378 + i*30;
                auto ic = rect(L, y+4, L+16, y+20);
                const wchar_t* label = ultra ? stepsU[i] : stepsV[i];
                if (i < current) { p.Icon(L"", ic, 11, text2); p.Text(label, rect(L+28, y, R, y+24), 15, text2); }
                else if (i == current) { p.Stroke(ic, 8*s, accent, 1.5f*s); p.Fill(rect(L+6.5f, y+10.5f, L+9.5f, y+13.5f), 1.5f*s, accent); p.Text(label, rect(L+28, y, R, y+24), 15, C(0xFF8A5C), DWRITE_FONT_WEIGHT_SEMI_BOLD); }
                else { p.Stroke(ic, 8*s, dim, 1.5f*s); p.Text(label, rect(L+28, y, R, y+24), 15, dim); }
            }
            if (ultra) p.Text(T(L"Se a conexão cair, o download continua de onde parou.",L"If the connection drops, the download resumes where it stopped.",L"Si se cae la conexión, la descarga continúa donde se detuvo."), rect(L, 552, R-130, 592), 13, text2);
            outline(rect(R-110, 552, R, 592), T(L"Cancelar",L"Cancel",L"Cancelar"), 3);
        } else {
            if (success) {
                auto ring = rect(L, 168, L+64, 232);
                p.Fill(rect(L-8, 160, L+72, 240), 40*s, C(0xFF6633, 0.14f));
                p.Glow(ring, 32*s);
                p.Fill(ring, 32*s, accent);
                p.Feather("M20 6L9 17l-5-5", ring, 30*s, C(0x0A0B0D), 2.4f);
                ph.Paragraph(T(L"Tudo pronto. Agora é a sua vez de testar.",L"All set. Now it's your turn to try it.",L"Todo listo. Ahora te toca probarlo."), (L+84)*s, 164*s, (R-L-84)*s, 32, C(0xFFFFFF));
                struct Tip { const wchar_t* key; const wchar_t* lead; const wchar_t* title; const wchar_t* body; float cap; };
                Tip tips[2] = {
                    {T(L"ESPAÇO",L"SPACE",L"ESPACIO"), nullptr, T(L"Espaço",L"Space",L"Espacio"), T(L"Escolha uma imagem ou PDF no Explorer e aperte ESPAÇO para abrir a prévia. Aperte de novo para fechar.",L"Pick an image or PDF in Explorer and press SPACE to open the preview. Press it again to close.",L"Elige una imagen o PDF en el Explorador y pulsa ESPACIO para abrir la vista. Pulsa de nuevo para cerrar."), 180},
                    {L"ALT", T(L"Segure a tecla",L"Hold the key",L"Mantén la tecla"), L"Alt", T(L"Com o arquivo selecionado, escolha o formato no menu que aparece.",L"With the file selected, choose the format in the menu that appears.",L"Con el archivo seleccionado, elige el formato en el menú que aparece."), 72},
                };
                int n = edition == 0 ? 1 : 2; // o Lite não tem o menu do Alt
                for (int i = 0; i < n; ++i) {
                    float y = 262 + i*124;
                    auto card = rect(L, y, R, y+110);
                    p.Fill(card, 14*s, C(0xFFFFFF, 0.05f)); p.Stroke(card, 14*s, C(0xFFFFFF, 0.10f));
                    float kx = L+20;
                    if (tips[i].lead) { p.Text(tips[i].lead, rect(kx, y+34, kx+110, y+76), 14, accent); kx += 199 - tips[i].cap; }
                    keycap(rect(kx, y+34, kx+tips[i].cap, y+76), tips[i].key);
                    float tx = L+20+199+18;
                    ph.Text(tips[i].title, rect(tx, y+12, R-20, y+36), 18, text, DWRITE_FONT_WEIGHT_BOLD);
                    p.Paragraph(tips[i].body, tx*s, (y+38)*s, (R-20-tx)*s, 14, C(0xE3E6EB));
                }
            } else {
                ph.Text(T(L"Não foi possível concluir.",L"Couldn't finish installation.",L"No se pudo completar."), rect(L,168,R,212), 32, text, DWRITE_FONT_WEIGHT_BOLD);
                p.Paragraph(error.substr(0,500), L*s, 228*s, (R-L)*s, 15, text2);
            }
            const wchar_t* go = success ? T(L"Começar >",L"Get started >",L"Comenzar >") : T(L"Fechar",L"Close",L"Cerrar");
            float bw = p.Measure(go, 15, DWRITE_FONT_WEIGHT_BOLD) / s + 68;
            primary(rect(R-bw, 528, R, 576), go, 2);
        }
        if(GetFocus()==hwnd) for(const auto& area:hits) if(area.first==focus)
            p.Stroke(D2D1::RectF(area.second.left-3*s,area.second.top-3*s,area.second.right+3*s,area.second.bottom+3*s),14*s,accent);
        p.Stroke(D2D1::RectF(0,0,w,h),18*s,C(0xFFFFFF,0.08f));
        surface.End();
    }
    void Action(int id) {
        if(page==1) { if(id==3 && cancel) cancel->store(true); return; }
        if(page==2) { if(id==2) DestroyWindow(hwnd); return; }
        if(id>=10 && id<=12) { edition=id-10; InvalidateRect(hwnd,nullptr,FALSE); }
        if(id==3 || id==4) { result=id==4?3:0; DestroyWindow(hwnd); }
        if(id==2 && !preview) {
            page=1; percent=2; phase=T(L"Preparando a instalação…",L"Preparing installation…",L"Preparando la instalación…");
            InvalidateRect(hwnd,nullptr,FALSE);
            worker=std::thread([this] {
                CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
                try {
                    success=work(edition,[this](int value,const std::wstring& text) {
                        {std::lock_guard<std::mutex> lock(mutex);percent=value;phase=text;}
                        PostMessageW(hwnd,Changed,0,0);
                    },error);
                } catch(...) {success=false;error=T(L"A instalação foi interrompida. Tente novamente.",L"Installation interrupted. Please try again.",L"Instalación interrumpida. Inténtalo de nuevo.");}
                CoUninitialize(); PostMessageW(hwnd,Finished,0,0);
            });
        }
    }
    static LRESULT CALLBACK Proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Window*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {self=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self) return DefWindowProcW(h,msg,wp,lp);
        switch(msg) {
        case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(h,&ps);self->Paint();EndPaint(h,&ps);return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_SIZE:self->surface.Resize(LOWORD(lp),HIWORD(lp));return 0;
        case WM_DPICHANGED:{auto*r=reinterpret_cast<RECT*>(lp);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);return 0;}
        case WM_NCHITTEST:{POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(h,&pt);if(pt.y<90*self->Scale())return HTCAPTION;return HTCLIENT;}
        case WM_MOUSEMOVE:{POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};int id=0;for(auto&hit:self->hits)if(ui::Hit(hit.second,pt))id=hit.first;if(id!=self->hot){self->hot=id;InvalidateRect(h,nullptr,FALSE);}TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);return 0;}
        case WM_MOUSELEAVE:self->hot=0;InvalidateRect(h,nullptr,FALSE);return 0;
        case WM_SETCURSOR:if(LOWORD(lp)==HTCLIENT){SetCursor(LoadCursor(nullptr,self->hot?IDC_HAND:IDC_ARROW));return TRUE;}break;
        case WM_LBUTTONUP:{POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};for(auto&hit:self->hits)if(ui::Hit(hit.second,pt)){int id=hit.first;self->focus=id;self->Action(id);break;}return 0;}
        case WM_KEYDOWN:
            if(wp==VK_TAB){int count=self->installed?3:2;self->focus=self->page==2?2:2+(self->focus-2+(GetKeyState(VK_SHIFT)<0?count-1:1))%count;InvalidateRect(h,nullptr,FALSE);return 0;}
            if(self->page==0 && (wp==VK_LEFT||wp==VK_RIGHT||wp==VK_UP||wp==VK_DOWN)){self->edition=(self->edition+(wp==VK_LEFT||wp==VK_UP?2:1))%3;InvalidateRect(h,nullptr,FALSE);return 0;}
            if(wp==VK_SPACE){self->Action(self->focus);return 0;}
            if(wp==VK_RETURN){self->Action(2);return 0;}
            if(wp==VK_ESCAPE && (self->page!=1 || self->preview)){DestroyWindow(h);return 0;}break;
        case Changed:InvalidateRect(h,nullptr,FALSE);return 0;
        case Finished:if(self->worker.joinable())self->worker.join();self->page=2;self->result=self->success?2:1;self->focus=2;InvalidateRect(h,nullptr,FALSE);return 0;
        case WM_CLOSE:if(self->page!=1 || self->preview)DestroyWindow(h);return 0;
        case WM_DESTROY:PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(h,msg,wp,lp);
    }
    int Show(HINSTANCE instance) {
        if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory)) || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&write))))return 1;
        WNDCLASSW wc={};wc.lpfnWndProc=Proc;wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));wc.lpszClassName=L"VizuInstaller";RegisterClassW(&wc);
        POINT point;GetCursorPos(&point);MONITORINFO monitor={sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
        HDC dc=GetDC(nullptr);float s=GetDeviceCaps(dc,LOGPIXELSX)/96.0f;ReleaseDC(nullptr,dc);
        int width=(int)(700*s),height=(int)(640*s);
        hwnd=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"Vizu — Instalação",WS_POPUP|WS_MINIMIZEBOX,(monitor.rcWork.left+monitor.rcWork.right-width)/2,(monitor.rcWork.top+monitor.rcWork.bottom-height)/2,width,height,nullptr,nullptr,instance,this);
        if(hwnd){float actual=Scale();if(fabs(actual-s)>0.01f){width=(int)(700*actual);height=(int)(640*actual);SetWindowPos(hwnd,nullptr,(monitor.rcWork.left+monitor.rcWork.right-width)/2,(monitor.rcWork.top+monitor.rcWork.bottom-height)/2,width,height,SWP_NOZORDER);}BOOL round=2;DwmSetWindowAttribute(hwnd,33,&round,sizeof(round));ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);SetFocus(hwnd);MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}}
        if(worker.joinable())worker.join();surface.Release();if(write)write->Release();if(factory)factory->Release();
        return hwnd?result:1;
    }
};
} // namespace installui
