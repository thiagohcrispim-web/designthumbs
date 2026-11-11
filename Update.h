// Update.h - procura versão nova nas Releases do GitHub (repositório público) e baixa o instalador.
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <urlmon.h>
#include "VectorBundle.h"
#include <string>

namespace upd {

static const wchar_t* kVersion = L"9.8.7";
static const wchar_t* kRepoApi = L"/repos/thiagohcrispim-web/designthumbs/releases/latest";
// Alternativa às Releases: arquivo no próprio repositório com a versão e o link do instalador.
static const wchar_t* kLatestJson = L"/thiagohcrispim-web/designthumbs/main/latest.json";

// "v8.1.2" -> {8,1,2}; compara versões.
inline int Compare(const std::wstring& a, const std::wstring& b) {
    int x[3] = {}, y[3] = {};
    swscanf(a.c_str() + (a.size() && (a[0] == L'v' || a[0] == L'V')), L"%d.%d.%d", &x[0], &x[1], &x[2]);
    swscanf(b.c_str() + (b.size() && (b[0] == L'v' || b[0] == L'V')), L"%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

inline std::string HttpsGet(const wchar_t* host, const wchar_t* path) {
    std::string body;
    HINTERNET s = WinHttpOpen(L"Vizu-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 5000, 5000, 8000, 8000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < (4u << 20)) {
                std::string chunk(avail, 0);
                DWORD got = 0;
                if (!WinHttpReadData(r, &chunk[0], avail, &got) || !got) break;
                body.append(chunk, 0, got);
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return body;
}

// Valor de uma chave string no JSON da release, a partir de `from`.
inline std::string JsonString(const std::string& j, const char* key, size_t from = 0, size_t* at = nullptr) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return "";
    p = j.find('"', j.find(':', p + k.size()) + 1);
    size_t e = j.find('"', p + 1);
    if (p == std::string::npos || e == std::string::npos) return "";
    if (at) *at = e;
    return j.substr(p + 1, e - p - 1);
}

struct Release { std::wstring tag, url; std::string sha256; };

// Última versão publicada e o link do instalador: latest.json do repositório (ou a última Release).
inline bool Latest(Release* out) {
    std::string lj = HttpsGet(L"raw.githubusercontent.com", kLatestJson);
    std::string ver = JsonString(lj, "version"), link = JsonString(lj, "url");
    if (!ver.empty() && !link.empty()) {
        out->tag.assign(ver.begin(), ver.end());
        out->url.assign(link.begin(), link.end());
        out->sha256 = JsonString(lj, "sha256");
        return true;
    }
    std::string j = HttpsGet(L"api.github.com", kRepoApi);
    if (j.empty()) return false;
    std::string tag = JsonString(j, "tag_name");
    size_t pos = 0;
    for (;;) {
        std::string url = JsonString(j, "browser_download_url", pos, &pos);
        if (url.empty()) break;
        if (url.size() > 9 && url.compare(url.size() - 9, 9, "Setup.exe") == 0) {
            out->tag.assign(tag.begin(), tag.end());
            out->url.assign(url.begin(), url.end());
            return !tag.empty();
        }
    }
    return false;
}

// Baixa o instalador para a pasta temporária.
inline bool Download(const std::wstring& url, std::wstring* path, const std::string& sha256 = "") {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    *path = std::wstring(tmp) + L"Vizu-Setup.exe";
    DeleteFileW(path->c_str());
    if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), path->c_str(), 0, nullptr))) return false;
    std::string want = sha256; for (auto& c : want) c = (char)tolower((unsigned char)c);
    if (!want.empty() && vbundle::FileSha256(*path) != want) { DeleteFileW(path->c_str()); return false; } // download corrompido ou adulterado
    return true;
}

} // namespace upd
