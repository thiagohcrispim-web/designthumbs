// Offline installation of the complete, pinned engine resource. No global Inkscape installation.
#pragma once
#include "VectorConvert.h"

namespace vbundle {
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
inline bool Prepare(const std::wstring& root, std::wstring& staging, std::wstring& error) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(3), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size) { error = L"O instalador não contém o motor vetorial. Baixe o instalador completo do Vizu."; return false; }
    staging = Unique(root, L"vector-engine-stage-");
    if (staging.empty() || !Owned(root, staging) || !CreateDirectoryW(staging.c_str(), nullptr)) {
        error = L"Não foi possível preparar a pasta do motor vetorial."; return false;
    }
    auto fail = [&]() { Remove(root, staging); staging.clear(); return false; };
    std::wstring archive = staging + L"\\engine.7z";
    HANDLE file = CreateFileW(archive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool saved = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, size, &written, nullptr) && written == size;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!saved) { error = L"Não foi possível gravar o pacote do motor. Verifique o espaço disponível."; return fail(); }
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
    vec::WriteNew(staging + L"\\VIZU-BUNDLE.txt", L"Inkscape 1.4.2 (f4327f4) portable x64\r\n"
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
} // namespace vbundle
