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
    void Paint() {
        surface.hwnd = hwnd;
        if (!surface.Begin(factory)) return;
        auto* rt = surface.rt;
        rt->Clear(theme.tint);
        float s = Scale(), w = rt->GetSize().width, h = rt->GetSize().height;
        ui::Painter p(rt, write, theme, s);
        auto rect = [&](float x, float y, float right, float bottom) { return D2D1::RectF(x*s, y*s, right*s, bottom*s); };
        float width = w / s;
        hits.clear();
        auto hit = [&](int id, D2D1_RECT_F r) {
            hits.push_back({id,r});
            return hot == id;
        };
        ui::Brand(rt, theme, 32*s, 30*s, 38*s);
        p.Text(L"v9.8.4", rect(width-130,32,width-32,62), 13, theme.text2, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (page == 0) {
            p.Text(installed ? T(L"Seu Vizu, atualizado.",L"Your Vizu, updated.",L"Tu Vizu, actualizado.") : T(L"Veja seus arquivos de outro jeito.",L"See your files in a new way.",L"Mira tus archivos de otra forma."), rect(32,105,width-32,147), 25, theme.text, DWRITE_FONT_WEIGHT_BOLD);
            p.Paragraph(T(L"Miniaturas, prévias e ferramentas para o seu dia a dia.",L"Thumbnails, previews and tools for your everyday work.",L"Miniaturas, vistas previas y herramientas para tu día a día."), 32*s,157*s,w-64*s,13,theme.text2);
            struct Ed { const wchar_t* name; const wchar_t* line1; const wchar_t* line2; const wchar_t* size; };
            Ed eds[3] = {
                {L"Vizu Lite", T(L"Prévia com Espaço",L"Space preview",L"Vista con Espacio"), T(L"Miniaturas no Explorer",L"Explorer thumbnails",L"Miniaturas en el Explorador"), L"~2 MB"},
                {L"Vizu", T(L"Tudo do Lite, mais",L"All of Lite, plus",L"Todo lo de Lite, más"), T(L"converter, PDF, OCR, relógio",L"convert, PDF, OCR, clock",L"convertir, PDF, OCR, reloj"), L"~2 MB"},
                {L"Vizu Ultra", T(L"Tudo do Vizu, mais",L"All of Vizu, plus",L"Todo lo de Vizu, más"), T(L"CDR/SVG em SVG e EPS vetorial",L"CDR/SVG to vector SVG and EPS",L"CDR/SVG a SVG y EPS vectorial"), T(L"+ ~100 MB baixados",L"+ ~100 MB download",L"+ ~100 MB descarga")},
            };
            float card=(width-88)/3;
            for (int i=0;i<3;++i) {
                float x=32+i*(card+12);
                auto box=rect(x,212,x+card,402);
                bool sel=edition==i, hov=hit(10+i,box);
                p.Glass(box,14*s,hov);
                if(sel) p.Stroke(box,14*s,theme.accent,2*s);
                auto dot=rect(x+16,228,x+36,248);
                p.Fill(dot,10*s,sel?theme.accent:theme.track);
                p.Stroke(dot,10*s,sel?theme.accent:theme.text2);
                if(sel) p.Icon(L"\uE73E",dot,12,theme.onAccent);
                p.Text(eds[i].name,rect(x+16,262,x+card-12,292),16,theme.text,DWRITE_FONT_WEIGHT_BOLD);
                p.Paragraph(eds[i].line1,(x+16)*s,298*s,(card-28)*s,12,theme.text2);
                p.Paragraph(eds[i].line2,(x+16)*s,318*s,(card-28)*s,12,theme.text);
                p.Text(eds[i].size,rect(x+16,368,x+card-12,392),11,theme.text2);
            }
            p.Text(edition==2?T(L"O Ultra baixa o motor vetorial (Inkscape) uma única vez, com verificação de integridade.",L"Ultra downloads the vector engine (Inkscape) once, with an integrity check.",L"Ultra descarga el motor vectorial (Inkscape) una sola vez, con verificación de integridad."):
                   edition==0?T(L"O Lite não tem menu circular, conversões, OCR nem relógio.",L"Lite has no radial menu, conversions, OCR or clock.",L"Lite no tiene menú circular, conversiones, OCR ni reloj."):
                   T(L"Você pode trocar de edição depois, abrindo o instalador de novo.",L"You can switch editions later by running the installer again.",L"Puedes cambiar de edición después, abriendo el instalador de nuevo."),rect(32,420,width-32,450),11.5f,theme.text2);
            p.Pill(rect(width-225,493,width-32,539),installed?T(L"Atualizar / reinstalar",L"Update / reinstall",L"Actualizar / reinstalar"):T(L"Instalar Vizu",L"Install Vizu",L"Instalar Vizu"),hit(2,rect(width-225,493,width-32,539)),true);
            p.Pill(rect(32,493,132,539),T(L"Cancelar",L"Cancel",L"Cancelar"),hit(3,rect(32,493,132,539)));
            if(installed) p.Text(T(L"Desinstalar",L"Uninstall",L"Desinstalar"),rect(148,493,252,539),12,hot==4?theme.danger:theme.text2);
            if(installed) hit(4,rect(148,493,252,539));
        } else {
            const wchar_t* title=page==1?T(L"Instalando o Vizu…",L"Installing Vizu…",L"Instalando Vizu…"):success?T(L"Tudo pronto para começar.",L"You're ready to go.",L"Todo listo para comenzar."):T(L"Não foi possível concluir.",L"Couldn't finish installation.",L"No se pudo completar.");
            p.Text(title,rect(32,130,width-32,178),25,theme.text,DWRITE_FONT_WEIGHT_BOLD);
            if(page==1) {
                int value; std::wstring text;
                { std::lock_guard<std::mutex> lock(mutex); value=percent; text=phase; }
                p.Paragraph(text,32*s,202*s,w-64*s,14,theme.text2);
                auto bar=rect(32,293,width-32,303);
                p.Fill(bar,5*s,theme.track);
                p.Fill(rect(32,293,32+(width-64)*value/100.0f,303),5*s,theme.accent);
                p.Text(std::to_wstring(value)+L"%",rect(32,319,width-32,350),13,theme.text2,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_TEXT_ALIGNMENT_TRAILING);
                p.Text(T(L"Estamos preparando tudo para você.",L"We're getting everything ready for you.",L"Estamos preparando todo para ti."),rect(32,461,width-32,490),12,theme.text2);
                p.Pill(rect(32,493,132,539),T(L"Cancelar",L"Cancel",L"Cancelar"),hit(3,rect(32,493,132,539)));
            } else {
                p.Paragraph(success?T(L"Selecione um arquivo no Explorer e aperte Espaço para visualizar. Segure Alt para acessar as ferramentas.",L"Select a file in Explorer and press Space to preview. Hold Alt to access tools.",L"Selecciona un archivo en el Explorador y pulsa Espacio para verlo. Mantén Alt para acceder a las herramientas."):error.substr(0,500),32*s,204*s,w-64*s,14,theme.text2);
                if(success) p.Text(edition==2?T(L"Vizu Ultra instalado, com o conversor vetorial.",L"Vizu Ultra installed, with the vector converter.",L"Vizu Ultra instalado, con el conversor vectorial."):edition==0?T(L"Vizu Lite instalado.",L"Vizu Lite installed.",L"Vizu Lite instalado."):T(L"Vizu instalado.",L"Vizu installed.",L"Vizu instalado."),rect(32,373,width-32,410),13,theme.text);
                p.Pill(rect(width-195,493,width-32,539),T(L"Concluir",L"Finish",L"Finalizar"),hit(2,rect(width-195,493,width-32,539)),true);
            }
        }
        p.Text(T(L"Sem administrador • Suas configurações são preservadas",L"No administrator needed • Your settings are preserved",L"Sin administrador • Se conservan tus ajustes"),rect(32,550,width-32,574),10.5f,theme.text2);
        if(GetFocus()==hwnd) for(const auto& area:hits) if(area.first==focus)
            p.Stroke(D2D1::RectF(area.second.left-3*s,area.second.top-3*s,area.second.right+3*s,area.second.bottom+3*s),14*s,theme.accent);
        p.Stroke(D2D1::RectF(0,0,w,h),12*s,theme.stroke);
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
            if(self->page==0 && (wp==VK_LEFT||wp==VK_RIGHT)){self->edition=(self->edition+(wp==VK_LEFT?2:1))%3;InvalidateRect(h,nullptr,FALSE);return 0;}
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
        int width=(int)(660*s),height=(int)(590*s);
        hwnd=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"Vizu — Instalação",WS_POPUP|WS_MINIMIZEBOX,(monitor.rcWork.left+monitor.rcWork.right-width)/2,(monitor.rcWork.top+monitor.rcWork.bottom-height)/2,width,height,nullptr,nullptr,instance,this);
        if(hwnd){float actual=Scale();if(fabs(actual-s)>0.01f){width=(int)(660*actual);height=(int)(590*actual);SetWindowPos(hwnd,nullptr,(monitor.rcWork.left+monitor.rcWork.right-width)/2,(monitor.rcWork.top+monitor.rcWork.bottom-height)/2,width,height,SWP_NOZORDER);}BOOL round=2;DwmSetWindowAttribute(hwnd,33,&round,sizeof(round));ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);SetFocus(hwnd);MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}}
        if(worker.joinable())worker.join();surface.Release();if(write)write->Release();if(factory)factory->Release();
        return hwnd?result:1;
    }
};
} // namespace installui
