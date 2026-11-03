// Pdf.h - páginas de PDF (e .ai, que é PDF por dentro) com o leitor de PDF nativo do Windows (Windows.Data.Pdf).
// As interfaces WinRT necessárias são declaradas aqui (o MinGW não traz windows.data.pdf.h); layout e IIDs
// conforme o Windows SDK.
#pragma once
#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <shcore.h>

namespace pdfrt {

struct Size { float Width, Height; };
struct Rect { float X, Y, Width, Height; };
struct Color { BYTE A, R, G, B; };

static const IID IID_IAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
struct IAsyncInfo : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;  // 0 iniciado, 1 concluído, 2 cancelado, 3 erro
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// IAsyncOperation<T*> / IAsyncAction: só usamos GetResults (mesma posição na vtable).
template <class T> struct IAsyncResult : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T** result) = 0;
};
struct IAsyncActionResult : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};

static const IID IID_IPdfPageRenderOptions = {0x3C98056F, 0xB7CF, 0x4C29, {0x9A, 0x04, 0x52, 0xD9, 0x02, 0x67, 0xF4, 0x25}};
struct IPdfPageRenderOptions : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(Rect*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(Rect) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(Color*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(Color) = 0;
};

static const IID IID_IPdfPage = {0x9DB4B0C8, 0x5320, 0x4CFC, {0xAD, 0x76, 0x49, 0x3F, 0xDA, 0xD0, 0xE5, 0x94}};
struct IPdfPage : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown* stream, IAsyncActionResult** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown* stream, IPdfPageRenderOptions* opt, IAsyncActionResult** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(Size*) = 0;
};

static const IID IID_IPdfDocument = {0xAC7EBEDD, 0x80FA, 0x4089, {0x84, 0x6E, 0x81, 0xB7, 0x7F, 0xF5, 0xA8, 0x6C}};
struct IPdfDocument : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IPdfPage** page) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean*) = 0;
};

static const IID IID_IPdfDocumentStatics = {0x433A0B5F, 0xC007, 0x4788, {0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99}};
struct IPdfDocumentStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown*, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown*, HSTRING, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown* stream, IAsyncResult<IPdfDocument>** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IUnknown*, HSTRING, void**) = 0;
};

static const IID IID_IRandomAccessStream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};

// Espera uma operação assíncrona do WinRT terminar (as do PDF rodam no pool de threads).
inline HRESULT Wait(IUnknown* op, DWORD timeoutMs = 15000) {
    IAsyncInfo* info = nullptr;
    HRESULT hr = op->QueryInterface(IID_IAsyncInfo, (void**)&info);
    if (FAILED(hr)) return hr;
    int status = 0;
    DWORD start = GetTickCount();
    while (SUCCEEDED(info->get_Status(&status)) && status == 0) {
        if (GetTickCount() - start > timeoutMs) { info->Cancel(); status = 2; break; }
        Sleep(4);
    }
    if (status == 3) info->get_ErrorCode(&hr); else hr = status == 1 ? S_OK : E_ABORT;
    info->Release();
    return hr;
}

inline HRESULT Activate(const wchar_t* cls, IInspectable** out) {
    HSTRING s = nullptr;
    HRESULT hr = WindowsCreateString(cls, (UINT32)wcslen(cls), &s);
    if (SUCCEEDED(hr)) hr = RoActivateInstance(s, out);
    WindowsDeleteString(s);
    return hr;
}

// Documento PDF aberto a partir de um IStream.
class Document {
    IPdfDocument* doc_ = nullptr;
public:
    ~Document() { if (doc_) doc_->Release(); }
    bool Open(IStream* s) {
        LARGE_INTEGER zero = {};
        s->Seek(zero, STREAM_SEEK_SET, nullptr);
        HSTRING cls = nullptr;
        IPdfDocumentStatics* st = nullptr;
        IUnknown* ras = nullptr;
        IAsyncResult<IPdfDocument>* op = nullptr;
        const wchar_t* name = L"Windows.Data.Pdf.PdfDocument";
        bool ok = SUCCEEDED(WindowsCreateString(name, (UINT32)wcslen(name), &cls)) &&
                  SUCCEEDED(RoGetActivationFactory(cls, IID_IPdfDocumentStatics, (void**)&st)) &&
                  SUCCEEDED(CreateRandomAccessStreamOverStream(s, BSOS_DEFAULT, IID_IRandomAccessStream, (void**)&ras)) &&
                  SUCCEEDED(st->LoadFromStreamAsync(ras, &op)) && SUCCEEDED(Wait(op)) && SUCCEEDED(op->GetResults(&doc_)) && doc_;
        if (op) op->Release(); if (ras) ras->Release(); if (st) st->Release();
        WindowsDeleteString(cls);
        return ok;
    }
    UINT32 Pages() { UINT32 n = 0; if (doc_) doc_->get_PageCount(&n); return n; }
    bool PageSize(UINT32 i, Size* sz) { // em DIPs (1/96 pol)
        IPdfPage* p = nullptr;
        bool ok = doc_ && SUCCEEDED(doc_->GetPage(i, &p)) && SUCCEEDED(p->get_Size(sz));
        if (p) p->Release();
        return ok;
    }
    // Renderiza a página como PNG (fundo branco) com o lado maior = cx; devolve um IStream com o PNG.
    IStream* RenderPng(UINT32 i, UINT cx) {
        IPdfPage* page = nullptr;
        IInspectable *optObj = nullptr, *memObj = nullptr;
        IPdfPageRenderOptions* opt = nullptr;
        IUnknown* mem = nullptr;
        IAsyncActionResult* op = nullptr;
        IStream* out = nullptr;
        Size sz = {};
        if (doc_ && SUCCEEDED(doc_->GetPage(i, &page)) && SUCCEEDED(page->get_Size(&sz)) && sz.Width > 0 && sz.Height > 0 &&
            SUCCEEDED(Activate(L"Windows.Data.Pdf.PdfPageRenderOptions", &optObj)) &&
            SUCCEEDED(optObj->QueryInterface(IID_IPdfPageRenderOptions, (void**)&opt)) &&
            SUCCEEDED(Activate(L"Windows.Storage.Streams.InMemoryRandomAccessStream", &memObj)) &&
            SUCCEEDED(memObj->QueryInterface(IID_IRandomAccessStream, (void**)&mem))) {
            bool wide = sz.Width >= sz.Height;
            opt->put_DestinationWidth(wide ? cx : (UINT)(cx * sz.Width / sz.Height + 1));
            opt->put_DestinationHeight(wide ? (UINT)(cx * sz.Height / sz.Width + 1) : cx);
            opt->put_BackgroundColor({255, 255, 255, 255});
            if (SUCCEEDED(page->RenderWithOptionsToStreamAsync(mem, opt, &op)) && SUCCEEDED(Wait(op)) && SUCCEEDED(op->GetResults()))
                CreateStreamOverRandomAccessStream(mem, IID_IStream, (void**)&out);
        }
        if (op) op->Release(); if (mem) mem->Release(); if (memObj) memObj->Release();
        if (opt) opt->Release(); if (optObj) optObj->Release(); if (page) page->Release();
        return out;
    }
};

} // namespace pdfrt
