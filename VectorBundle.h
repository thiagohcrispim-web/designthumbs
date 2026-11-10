// Vizu Ultra: baixa o Inkscape portátil oficial (versão e SHA-256 fixos), extrai numa pasta privada do Vizu.
// O instalador continua pequeno; só quem escolhe o Ultra baixa o motor, uma única vez.
#pragma once
#include "VectorConvert.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <atomic>
#include <functional>

namespace vbundle {
static const wchar_t* kUrl = L"https://media.inkscape.org/dl/resources/file/inkscape-1.4.2_2025-05-13_f4327f4-x64.7z";
static const char* kSha256 = "757e0358512630e65505a4cca89885369087f2908469439bc41b5a346604cbd4";
static const wchar_t* kMarker = L"Inkscape 1.4.2 (f4327f4) portable x64";
inline std::string FileSha256(const std::wstring& path) {
    std::string hex;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return hex;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<BYTE> buf(1 << 20); DWORD n = 0; bool ok = true;
        while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), n, 0));
        BYTE digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, 32, 0)))
            for (BYTE b : digest) { char t[3]; snprintf(t, 3, "%02x", b); hex += t; }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}
// Progresso e cancelamento do download do motor.
using Progress = std::function<void(unsigned long long got, unsigned long long total, double bytesPerSec)>;
inline std::atomic<bool>& Cancel() { static std::atomic<bool> flag{false}; return flag; }
// Texto do progresso: "42 MB de 99 MB · 3,1 MB/s · ~20 s".
inline std::wstring Status(unsigned long long got, unsigned long long total, double bps) {
    auto mb = [](unsigned long long bytes) { return std::to_wstring((bytes + 524288) / 1048576); };
    unsigned long long tenths = (unsigned long long)(bps / 104857.6 + 0.5); // MB/s com uma casa decimal
    std::wstring text = L"Baixando o motor vetorial · " + mb(got) + L" MB de " + mb(total) + L" MB · " +
        std::to_wstring(tenths / 10) + L"," + std::to_wstring(tenths % 10) + L" MB/s";
    if (total > got && bps > 1) {
        unsigned long long s = (unsigned long long)((total - got) / bps);
        text += s >= 90 ? L" · ~" + std::to_wstring(s / 60) + L" min" : L" · ~" + std::to_wstring(s) + L" s";
    }
    return text;
}
// Baixa com progresso real. Retoma de onde parou (HTTP Range) e tenta de novo em falhas de rede.
inline bool Fetch(const std::wstring& url, const std::wstring& dest, const Progress& onProgress, std::wstring& error) {
    URL_COMPONENTS u = {sizeof(u)}; wchar_t host[256] = {}, path[2048] = {};
    u.lpszHostName = host; u.dwHostNameLength = ARRAYSIZE(host); u.lpszUrlPath = path; u.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &u)) { error = L"Endereço do motor inválido."; return false; }
    HINTERNET session = WinHttpOpen(L"Vizu-Installer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = L"Não foi possível iniciar a conexão de rede."; return false; }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    HANDLE file = CreateFileW(dest.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file == INVALID_HANDLE_VALUE) { WinHttpCloseHandle(session); error = L"Não foi possível gravar o motor vetorial."; return false; }
    for (int attempt = 0; attempt < 4 && !ok && !Cancel(); ++attempt) {
        if (attempt) Sleep(1500 * attempt);
        LARGE_INTEGER size = {}; GetFileSizeEx(file, &size);
        unsigned long long offset = (unsigned long long)size.QuadPart;
        HINTERNET conn = WinHttpConnect(session, host, u.nPort, 0);
        HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        std::wstring range = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n" : L"";
        if (req && WinHttpSendRequest(req, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(), (DWORD)-1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            DWORD length = 0; sz = sizeof(length);
            bool hasLength = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &length, &sz, WINHTTP_NO_HEADER_INDEX) != FALSE;
            bool resumed = status == 206 && offset > 0;
            if (status == 416 && offset > 0) { // o arquivo parcial já está completo ou inválido: recomeça
                SetFilePointerEx(file, {}, nullptr, FILE_BEGIN); SetEndOfFile(file); offset = 0;
            } else if (status == 200 || status == 206) {
                if (!resumed) { SetFilePointerEx(file, {}, nullptr, FILE_BEGIN); SetEndOfFile(file); offset = 0; }
                LARGE_INTEGER pos = {}; pos.QuadPart = (LONGLONG)offset; SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);
                unsigned long long total = hasLength ? offset + length : 0, got = offset;
                std::vector<BYTE> buf(65536); DWORD n = 0; bool finished = false;
                ULONGLONG last = GetTickCount64(), lastMark = last; unsigned long long lastGot = got;
                while (!Cancel()) {
                    if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) break;
                    if (n == 0) { finished = true; break; }
                    DWORD written = 0;
                    if (!WriteFile(file, buf.data(), n, &written, nullptr) || written != n) break;
                    got += n;
                    ULONGLONG now = GetTickCount64();
                    if (now - last >= 400) {
                        if (onProgress) onProgress(got, total, (got - lastGot) * 1000.0 / (double)(now - lastMark + 1));
                        lastMark = now; lastGot = got; last = now;
                    }
                }
                if (onProgress) onProgress(got, total, 0);
                ok = finished && !Cancel() && (total == 0 || got == total);
            }
        }
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
    }
    CloseHandle(file); WinHttpCloseHandle(session);
    if (Cancel()) error = L"Instalação cancelada.";
    else if (!ok) error = L"Não foi possível baixar o motor vetorial (inkscape.org). Verifique a internet e tente de novo, ou instale a edição Vizu.";
    return ok;
}

// Motor já instalado na versão certa? (atualizações não baixam nem extraem de novo)
inline bool Current(const std::wstring& root) {
    std::string marker;
    return !vec::BundledEngine(root).empty() && vec::Read(root + L"\\vector-engine\\VIZU-BUNDLE.txt", marker) &&
           marker.find(vec::Utf8(kMarker)) != std::string::npos;
}
inline std::wstring Full(const std::wstring& path) {
    wchar_t result[32768] = {};
    DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(result), result, nullptr);
    return n && n < ARRAYSIZE(result) ? result : L"";
}
inline bool Owned(const std::wstring& parent, const std::wstring& child) {
    std::wstring root = vec::Lower(Full(parent)), target = vec::Lower(Full(child));
    if (root.empty() || target.empty()) return false;
    if (root.back() != L'\\') root += L'\\';
    if (target.compare(0, root.size(), root) != 0) return false;
    std::wstring leaf = target.substr(root.size());
    return leaf.find_first_of(L"\\/") == std::wstring::npos &&
        (leaf == L"vector-engine" || leaf.compare(0, 20, L"vector-engine-stage-") == 0 || leaf.compare(0, 18, L"vector-engine-old-") == 0);
}
inline bool EraseTree(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    // Delete junctions/symlinks themselves; never walk through them.
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        return DeleteFileW(path.c_str()) != FALSE;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(path.c_str()) != FALSE;
    WIN32_FIND_DATAW data; HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    bool ok = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            ok = EraseTree(path + L"\\" + data.cFileName) && ok;
        } while (FindNextFileW(find, &data));
        DWORD error = GetLastError(); FindClose(find);
        if (error != ERROR_NO_MORE_FILES) ok = false;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) ok = false;
    return RemoveDirectoryW(path.c_str()) != FALSE && ok;
}
inline bool Remove(const std::wstring& parent, const std::wstring& child) {
    if (!Owned(parent, child)) return false;
    std::wstring path = Full(child);
    // The portable package includes Python paths beyond MAX_PATH, especially after backup renames.
    if (path.compare(0, 4, L"\\\\?\\") != 0)
        path = path.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
    return EraseTree(path);
}
inline std::wstring Unique(const std::wstring& root, const wchar_t* prefix) {
    GUID id; wchar_t name[40] = {};
    if (FAILED(CoCreateGuid(&id))) return L"";
    StringFromGUID2(id, name, ARRAYSIZE(name));
    return Full(root) + L"\\" + prefix + name;
}
inline bool Prepare(const std::wstring& root, std::wstring& staging, std::wstring& error, const Progress& onProgress = nullptr) {
    staging = Unique(root, L"vector-engine-stage-");
    if (staging.empty() || !Owned(root, staging) || !CreateDirectoryW(staging.c_str(), nullptr)) {
        error = L"Não foi possível preparar a pasta do motor vetorial."; return false;
    }
    auto fail = [&]() { Remove(root, staging); staging.clear(); return false; };
    std::wstring archive = staging + L"\\engine.7z";
    // Integridade: se o arquivo baixado não confere, descarta e baixa de novo uma vez.
    for (int pass = 0; pass < 2; ++pass) {
        if (!Fetch(kUrl, archive, onProgress, error)) return fail();
        if (FileSha256(archive) == kSha256) break;
        DeleteFileW(archive.c_str());
        if (pass == 1) { error = L"O motor vetorial baixado não confere com a assinatura esperada (SHA-256). Instalação recusada."; return fail(); }
    }
    wchar_t system[32768] = {}; GetSystemDirectoryW(system, ARRAYSIZE(system));
    std::wstring log;
    if (!vec::Run(std::wstring(system) + L"\\tar.exe", {L"-xf", archive, L"-C", staging}, staging + L"\\install.log", log, error, 300000)) {
        error = L"Falha ao extrair o motor vetorial incluído.\n" + error + L"\n" + log; return fail();
    }
    DeleteFileW(archive.c_str());
    std::wstring engine = staging + L"\\inkscape\\bin\\inkscape.exe";
    if (!vec::Exists(engine) || !vec::Exists(staging + L"\\inkscape\\bin\\libcdr-0.1.dll") ||
        !vec::Exists(staging + L"\\inkscape\\share\\inkscape\\doc\\COPYING")) {
        error = L"Pacote do motor incompleto."; return fail();
    }
    log.clear();
    if (!vec::Run(engine, {L"--version"}, staging + L"\\install.log", log, error) || log.find(L"Inkscape 1.4.2") == std::wstring::npos) {
        error = L"O motor instalado não passou na verificação de inicialização.\n" + error + L"\n" + log; return fail();
    }
    DeleteFileW((staging + L"\\install.log").c_str());
    vec::WriteNew(staging + L"\\VIZU-BUNDLE.txt", std::wstring(kMarker) + L"\r\n"
        L"Archive SHA256: 757e0358512630e65505a4cca89885369087f2908469439bc41b5a346604cbd4\r\n"
        L"Original licenses: inkscape/share/inkscape/doc\r\n"
        L"Sources: https://inkscape.org/release/inkscape-1.4.2/source/\r\n");
    return true;
}
inline bool Commit(const std::wstring& root, const std::wstring& staging, std::wstring& error) {
    if (!Owned(root, staging)) { error = L"Pasta temporária do motor inválida."; return false; }
    std::wstring target = Full(root) + L"\\vector-engine", backup = Unique(root, L"vector-engine-old-");
    DWORD attr = GetFileAttributesW(target.c_str());
    bool hadOld = attr != INVALID_FILE_ATTRIBUTES;
    if (backup.empty() || (hadOld && (attr & FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = L"A pasta do motor contém um redirecionamento não permitido."; return false;
    }
    if (hadOld && !MoveFileExW(target.c_str(), backup.c_str(), 0)) {
        error = L"O motor vetorial está em uso. Feche a conversão em andamento e tente novamente."; return false;
    }
    if (!MoveFileExW(staging.c_str(), target.c_str(), 0)) {
        if (hadOld) MoveFileExW(backup.c_str(), target.c_str(), 0);
        error = L"Não foi possível concluir a instalação do motor vetorial."; return false;
    }
    if (hadOld) Remove(root, backup); // Locked remnants are retried by Cleanup.
    return true;
}
inline bool Cleanup(const std::wstring& root, bool uninstall) {
    bool ok = true;
    if (uninstall) ok = Remove(root, Full(root) + L"\\vector-engine");
    for (auto pattern : {L"\\vector-engine-stage-*", L"\\vector-engine-old-*"}) {
        WIN32_FIND_DATAW data; HANDLE find = FindFirstFileW((root + pattern).c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) continue;
        do { ok = Remove(root, Full(root) + L"\\" + data.cFileName) && ok; } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return ok;
}
inline bool Configure(const std::wstring& root, bool enabled, std::wstring& error, const Progress& onProgress = nullptr) {
    if (!enabled) {
        if (!Cleanup(root, true)) { error = L"Não foi possível remover o conversor anterior. Feche os processos de conversão e tente novamente."; return false; }
        return true;
    }
    if (Current(root)) { Cleanup(root, false); return true; }
    std::wstring staging;
    if (!Prepare(root, staging, error, onProgress)) return false;
    if (!Commit(root, staging, error)) { Remove(root, staging); return false; }
    Cleanup(root, false);
    return true;
}
} // namespace vbundle
