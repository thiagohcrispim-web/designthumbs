// Ocr.h - leitura de texto com o OCR nativo do Windows 10/11 (Windows.Media.Ocr), sem internet e sem instalar nada.
// As interfaces WinRT são declaradas aqui (o MinGW não traz windows.media.ocr.h); IIDs e ordem dos métodos
// conforme o Windows SDK (windows.media.ocr.h / windows.graphics.imaging.idl).
#pragma once
#include "Pdf.h"
#include <string>
#include <vector>

namespace ocr {

using pdfrt::IAsyncResult;

template <class T> struct IVectorView : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, T** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};

static const IID IID_ILanguage = {0xEA79A752, 0xF7C2, 0x4265, {0xB1, 0xBD, 0xC4, 0xDE, 0xC4, 0xE4, 0xF0, 0x80}};
struct ILanguage : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_LanguageTag(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NativeName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Script(HSTRING*) = 0;
};

struct BitmapPlaneDescription { INT32 StartIndex, Width, Height, Stride; };
static const IID IID_IBitmapBuffer = {0xA53E04C4, 0x399C, 0x438C, {0xB2, 0x8F, 0xA6, 0x3A, 0x6B, 0x83, 0xD1, 0xA1}};
struct IBitmapBuffer : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPlaneCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaneDescription(INT32 index, BitmapPlaneDescription*) = 0;
};
static const IID IID_IMemoryBuffer = {0xFBC4DD2A, 0x245B, 0x11E4, {0xAF, 0x98, 0x68, 0x94, 0x23, 0x26, 0x0C, 0xF8}};
struct IMemoryBuffer : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateReference(IInspectable** reference) = 0;
};
static const IID IID_IMemoryBufferByteAccess = {0x5B0D3235, 0x4DBA, 0x4D44, {0x86, 0x5E, 0x8F, 0x1D, 0x0E, 0x4F, 0xD0, 0x4D}};
struct IMemoryBufferByteAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** value, UINT32* capacity) = 0;
};
static const IID IID_IClosable = {0x30D5A829, 0x7FA4, 0x4026, {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};
struct IClosable : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

static const IID IID_ISoftwareBitmap = {0x689E0708, 0x7EEF, 0x483F, {0x96, 0x3F, 0xDA, 0x93, 0x88, 0x18, 0xE0, 0x73}};
struct ISoftwareBitmap : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BitmapPixelFormat(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapAlphaMode(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PixelWidth(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PixelHeight(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsReadOnly(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DpiX(DOUBLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DpiX(DOUBLE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DpiY(DOUBLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DpiY(DOUBLE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockBuffer(int mode, IInspectable** buffer) = 0; // 2 = escrita
};
static const IID IID_ISoftwareBitmapFactory = {0xC99FEB69, 0x2D62, 0x4D47, {0xA6, 0xB3, 0x4F, 0xDB, 0x6A, 0x07, 0xFD, 0xF8}};
struct ISoftwareBitmapFactory : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Create(int format, INT32 width, INT32 height, ISoftwareBitmap** value) = 0; // 87 = Bgra8
};

struct IOcrLine : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};
struct IOcrResult : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(IVectorView<IOcrLine>**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TextAngle(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING*) = 0;
};
struct IOcrEngine : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(ISoftwareBitmap* bitmap, IAsyncResult<IOcrResult>** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RecognizerLanguage(ILanguage**) = 0;
};
static const IID IID_IOcrEngineStatics = {0x5BFFA85A, 0x3384, 0x3540, {0x99, 0x40, 0x69, 0x91, 0x20, 0xD4, 0x28, 0xA8}};
struct IOcrEngineStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(IVectorView<ILanguage>**) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(ILanguage*, boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(ILanguage*, IOcrEngine**) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(IOcrEngine**) = 0;
};

inline std::wstring Str(HSTRING h) {
    UINT32 n = 0;
    const wchar_t* p = WindowsGetStringRawBuffer(h, &n);
    std::wstring r(p ? p : L"", n);
    WindowsDeleteString(h);
    return r;
}

template <class T> inline HRESULT Factory(const wchar_t* cls, const IID& iid, T** out) {
    HSTRING s = nullptr;
    HRESULT hr = WindowsCreateString(cls, (UINT32)wcslen(cls), &s);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(s, iid, (void**)out);
    WindowsDeleteString(s);
    return hr;
}

// Idiomas que o OCR do Windows sabe ler neste PC (nomes para exibir).
inline std::vector<std::wstring> Languages() {
    std::vector<std::wstring> r;
    IOcrEngineStatics* st = nullptr;
    if (FAILED(Factory(L"Windows.Media.Ocr.OcrEngine", IID_IOcrEngineStatics, &st))) return r;
    IVectorView<ILanguage>* v = nullptr;
    if (SUCCEEDED(st->get_AvailableRecognizerLanguages(&v)) && v) {
        UINT32 n = 0; v->get_Size(&n);
        for (UINT32 i = 0; i < n; ++i) {
            ILanguage* l = nullptr;
            if (SUCCEEDED(v->GetAt(i, &l)) && l) { HSTRING h = nullptr; if (SUCCEEDED(l->get_DisplayName(&h))) r.push_back(Str(h)); l->Release(); }
        }
        v->Release();
    }
    st->Release();
    return r;
}

// Lê o texto de uma imagem BGRA (de cima para baixo). keepLines: mantém as quebras de linha; senão junta numa linha.
// Retorna false se o OCR do Windows não estiver disponível (lang recebe a explicação).
inline bool Recognize(const BYTE* bgra, int w, int h, int stride, bool keepLines, std::wstring* text, std::wstring* langTag) {
    text->clear();
    IOcrEngineStatics* st = nullptr;
    if (FAILED(Factory(L"Windows.Media.Ocr.OcrEngine", IID_IOcrEngineStatics, &st))) return false;
    IOcrEngine* eng = nullptr;
    st->TryCreateFromUserProfileLanguages(&eng);
    st->Release();
    if (!eng) return false;
    if (langTag) { ILanguage* l = nullptr; if (SUCCEEDED(eng->get_RecognizerLanguage(&l)) && l) { HSTRING hs = nullptr; if (SUCCEEDED(l->get_LanguageTag(&hs))) *langTag = Str(hs); l->Release(); } }

    bool ok = false;
    ISoftwareBitmapFactory* bf = nullptr;
    ISoftwareBitmap* bmp = nullptr;
    if (SUCCEEDED(Factory(L"Windows.Graphics.Imaging.SoftwareBitmap", IID_ISoftwareBitmapFactory, &bf)) && SUCCEEDED(bf->Create(87, w, h, &bmp)) && bmp) {
        IInspectable* buf = nullptr;
        if (SUCCEEDED(bmp->LockBuffer(2, &buf)) && buf) {
            BitmapPlaneDescription pd = {0, w, h, w * 4};
            IBitmapBuffer* bb = nullptr;
            if (SUCCEEDED(buf->QueryInterface(IID_IBitmapBuffer, (void**)&bb))) { bb->GetPlaneDescription(0, &pd); bb->Release(); }
            IMemoryBuffer* mb = nullptr;
            if (SUCCEEDED(buf->QueryInterface(IID_IMemoryBuffer, (void**)&mb))) {
                IInspectable* ref = nullptr;
                if (SUCCEEDED(mb->CreateReference(&ref)) && ref) {
                    IMemoryBufferByteAccess* ba = nullptr;
                    BYTE* dst = nullptr; UINT32 cap = 0;
                    if (SUCCEEDED(ref->QueryInterface(IID_IMemoryBufferByteAccess, (void**)&ba)) && SUCCEEDED(ba->GetBuffer(&dst, &cap)) && dst) {
                        for (int y = 0; y < h; ++y) {
                            size_t off = (size_t)pd.StartIndex + (size_t)y * pd.Stride;
                            if (off + (size_t)w * 4 > cap) break;
                            memcpy(dst + off, bgra + (size_t)y * stride, (size_t)w * 4);
                        }
                        ok = true;
                    }
                    if (ba) ba->Release();
                    IClosable* c = nullptr;
                    if (SUCCEEDED(ref->QueryInterface(IID_IClosable, (void**)&c))) { c->Close(); c->Release(); }
                    ref->Release();
                }
                mb->Release();
            }
            IClosable* c = nullptr;
            if (SUCCEEDED(buf->QueryInterface(IID_IClosable, (void**)&c))) { c->Close(); c->Release(); }
            buf->Release();
        }
    }
    if (bf) bf->Release();
    if (ok) {
        ok = false;
        IAsyncResult<IOcrResult>* op = nullptr;
        IOcrResult* res = nullptr;
        if (SUCCEEDED(eng->RecognizeAsync(bmp, &op)) && op && SUCCEEDED(pdfrt::Wait(op, 15000)) && SUCCEEDED(op->GetResults(&res)) && res) {
            IVectorView<IOcrLine>* lines = nullptr;
            if (SUCCEEDED(res->get_Lines(&lines)) && lines) {
                UINT32 n = 0; lines->get_Size(&n);
                for (UINT32 i = 0; i < n; ++i) {
                    IOcrLine* ln = nullptr;
                    if (SUCCEEDED(lines->GetAt(i, &ln)) && ln) {
                        HSTRING hs = nullptr;
                        if (SUCCEEDED(ln->get_Text(&hs))) {
                            std::wstring t = Str(hs);
                            if (!text->empty()) {
                                // juntando: hífen no fim da linha une a palavra; senão, espaço
                                // hífen de quebra só une quando a palavra continua em minúscula ("infor-" + "mação")
                                bool joinWord = !keepLines && text->size() >= 2 && text->back() == L'-' && iswalpha((*text)[text->size() - 2]) && !t.empty() && iswlower(t[0]);
                                if (joinWord) text->pop_back();
                                else *text += keepLines ? L"\r\n" : L" ";
                            }
                            *text += t;
                        }
                        ln->Release();
                    }
                }
                lines->Release();
            }
            res->Release();
            ok = true;
        }
        if (op) op->Release();
    }
    if (bmp) bmp->Release();
    eng->Release();
    return ok;
}

} // namespace ocr
