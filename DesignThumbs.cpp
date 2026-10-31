// DesignThumbs - thumbnail provider leve para o Explorer do Windows.
// Os formatos suportados e os decodificadores ficam em Formats.h (compartilhado com o DesignPeek).
// Registro por usuário (HKCU): instala sem administrador.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000003 // Windows 10 1703 (SVG no Direct2D)
#endif
#include <windows.h>
#include <shlobj.h>
#include <olectl.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include <new>
#include "Formats.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d2d1.lib")

// {9E85547E-13C6-4C5E-95CC-990C74D9BB3F}
static const CLSID CLSID_DesignThumbs = {0x9e85547e, 0x13c6, 0x4c5e, {0x95, 0xcc, 0x99, 0x0c, 0x74, 0xd9, 0xbb, 0x3f}};
static const wchar_t* kClsidStr = L"{9E85547E-13C6-4C5E-95CC-990C74D9BB3F}";

static HMODULE g_module;
static LONG g_refs;

// ---------- COM: provider ----------

class Provider : public IThumbnailProvider, public IInitializeWithStream {
    LONG ref_ = 1;
    IStream* stream_ = nullptr;
public:
    Provider() { InterlockedIncrement(&g_refs); }
    ~Provider() { if (stream_) stream_->Release(); InterlockedDecrement(&g_refs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(Provider, IThumbnailProvider), QITABENT(Provider, IInitializeWithStream), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }

    IFACEMETHODIMP Initialize(IStream* s, DWORD) {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = s; s->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) {
        if (!stream_) return E_UNEXPECTED;
        // A extensão ajuda a reconhecer RAW de câmera (TIFF por dentro)
        std::wstring ext;
        STATSTG st = {};
        if (SUCCEEDED(stream_->Stat(&st, STATFLAG_DEFAULT)) && st.pwcsName) {
            ext = PathFindExtensionW(st.pwcsName);
            CoTaskMemFree(st.pwcsName);
        }
        bool hasAlpha = false;
        HRESULT hr = fmt::Render(stream_, ext, cx, 0, phbmp, &hasAlpha);
        *alpha = hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        return hr;
    }
};

class Factory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        static const QITAB qit[] = {QITABENT(Factory, IClassFactory), {0}};
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return 2; }
    IFACEMETHODIMP_(ULONG) Release() { return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Provider* p = new (std::nothrow) Provider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) { lock ? InterlockedIncrement(&g_refs) : InterlockedDecrement(&g_refs); return S_OK; }
};
static Factory g_factory;

// ---------- exports ----------

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_module = m; DisableThreadLibraryCalls(m); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_DesignThumbs) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

static LSTATUS SetKey(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (s == ERROR_SUCCESS) {
        s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    return s;
}

static const wchar_t* kThumbKey = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring clsid = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
    if (SetKey(clsid, nullptr, L"DesignThumbs Thumbnail Provider") ||
        SetKey(clsid + L"\\InprocServer32", nullptr, path) ||
        SetKey(clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment"))
        return SELFREG_E_CLASS;
    for (auto ext : fmt::kExtensions)
        if (SetKey(std::wstring(L"Software\\Classes\\") + ext + kThumbKey, nullptr, kClsidStr)) return SELFREG_E_CLASS;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    for (auto ext : fmt::kExtensions) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + ext + kThumbKey;
        wchar_t val[64] = {}; DWORD sz = sizeof(val);
        // só remove se o handler registrado for o nosso
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS &&
            _wcsicmp(val, kClsidStr) == 0)
            RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
