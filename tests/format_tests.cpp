// Leitores de arquivo e comparação de versão: arquivos corrompidos falham sem derrubar o programa.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000003
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include "../Formats.h"
#include "../Update.h"
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }

static void Put(fmt::Bytes& b, UINT64 v, int bytes, bool big) {
    for (int i = 0; i < bytes; ++i) b.push_back((BYTE)(v >> 8 * (big ? bytes - 1 - i : i)));
}
// Cabeçalho PSD + as três seções vazias + tipo de compressão.
static fmt::Bytes Psd(UINT channels, UINT h, UINT w, UINT compression) {
    fmt::Bytes b = {'8', 'B', 'P', 'S', 0, 1, 0, 0, 0, 0, 0, 0};
    Put(b, channels, 2, true); Put(b, h, 4, true); Put(b, w, 4, true); Put(b, 8, 2, true); Put(b, 3, 2, true); // 8 bits, RGB
    for (int i = 0; i < 3; ++i) Put(b, 0, 4, true);
    Put(b, compression, 2, true);
    return b;
}
static HRESULT Render(const fmt::Bytes& data, const wchar_t* ext, HBITMAP* bmp) {
    IStream* s = SHCreateMemStream(data.data(), (UINT)data.size());
    bool alpha = false;
    HRESULT hr = s ? fmt::Render(s, ext, 256, 0, bmp, &alpha) : E_OUTOFMEMORY;
    if (s) s->Release();
    return hr;
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        HBITMAP bmp = nullptr;
        // PSD bom, 2 x 2, sem compressão: continua abrindo
        fmt::Bytes good = Psd(3, 2, 2, 0);
        for (int i = 0; i < 12; ++i) good.push_back((BYTE)(i * 20));
        Check(SUCCEEDED(Render(good, L".psd", &bmp)) && bmp, "valid PSD renders");
        DeleteObject(bmp); bmp = nullptr;
        // tamanhos absurdos: falha, sem pedir memória nem derrubar o processo
        Check(FAILED(Render(Psd(3, 0xFFFFFFFFu, 1, 0), L".psd", &bmp)), "PSD with absurd height is refused");
        Check(FAILED(Render(Psd(3, 1, 0xFFFFFFFFu, 1), L".psd", &bmp)), "PSD with absurd width is refused");
        Check(FAILED(Render(Psd(3, 300000, 300000, 1), L".psd", &bmp)), "truncated huge PSD is refused");
        // linha comprimida que diz ter 4 GB
        fmt::Bytes rle = Psd(3, 1, 1, 1);
        for (int i = 0; i < 3; ++i) Put(rle, 0xFFFF, 2, true);
        Check(FAILED(Render(rle, L".psd", &bmp)), "PSD with oversized packed row is refused");
        // ZIP cuja prévia diz ocupar 4 GB comprimidos
        fmt::Bytes zip = {'P', 'K', 1, 2};
        zip.resize(46, 0);
        Put(zip, 0, 0, false);
        zip[20] = zip[21] = zip[22] = zip[23] = 0xFF; // tamanho comprimido
        zip[24] = 16;                                  // tamanho real
        const char name[] = "preview.png";
        zip[28] = (BYTE)(sizeof(name) - 1);
        zip.insert(zip.end(), name, name + sizeof(name) - 1);
        UINT cdSize = (UINT)zip.size();
        fmt::Bytes eocd = {'P', 'K', 5, 6, 0, 0, 0, 0, 1, 0, 1, 0};
        Put(eocd, cdSize, 4, false); Put(eocd, 0, 4, false); Put(eocd, 0, 2, false);
        zip.insert(zip.end(), eocd.begin(), eocd.end());
        IStream* zs = SHCreateMemStream(zip.data(), (UINT)zip.size());
        fmt::Bytes img;
        Check(zs && !fmt::ZipPreview(zs, img), "ZIP entry larger than the file is refused");
        if (zs) zs->Release();
        // versões
        Check(upd::Compare(L"9.8.9", L"9.8.10") < 0, "9.8.10 is newer than 9.8.9");
        Check(upd::Compare(L"v9.8.7", L"9.8.7") == 0, "leading v is ignored");
        Check(upd::Compare(L"10.0.0", L"9.9.9") > 0, "major version wins");
        std::wcout << L"format tests passed: " << checks << L" checks\n";
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
