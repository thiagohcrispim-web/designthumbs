// Original document -> Inkscape -> audited SVG -> SVG/EPS. Never uses a preview bitmap.
#pragma once
#include "I18n.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <xmllite.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vec {
inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
inline std::wstring Trim(std::wstring s) {
    size_t a = s.find_first_not_of(L" \t\r\n\"'");
    return a == std::wstring::npos ? L"" : s.substr(a, s.find_last_not_of(L" \t\r\n\"'") - a + 1);
}
inline bool IsInput(const std::wstring& path) {
    std::wstring e = Lower(PathFindExtensionW(path.c_str()));
    return e == L".cdr" || e == L".svg"; // Explicit scope; no hidden PDF/EPS dependencies.
}
inline bool Exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::wstring Quote(const std::wstring& s) {
    std::wstring q = L"\""; size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        q += c; slashes = 0;
    }
    q.append(slashes * 2, L'\\'); return q + L"\"";
}
inline bool Read(const std::wstring& path, std::string& s, DWORD limit = 100u << 20) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {}; DWORD n = 0;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= limit;
    if (ok) { s.resize((size_t)size.QuadPart); ok = ReadFile(h, &s[0], (DWORD)s.size(), &n, nullptr) && n == s.size(); }
    CloseHandle(h); return ok;
}
inline bool WriteNew(const std::wstring& path, const std::wstring& text) {
    std::string s = Utf8(text); DWORD n = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
    CloseHandle(h); if (!ok) DeleteFileW(path.c_str()); return ok;
}
struct Temp {
    std::wstring dir;
    std::wstring At(const wchar_t* name) const { return dir + L"\\" + name; }
    Temp() {
        wchar_t base[32768] = {}, guid[40] = {}; GUID id;
        DWORD n = GetTempPathW(ARRAYSIZE(base), base);
        if (n && n < ARRAYSIZE(base) && SUCCEEDED(CoCreateGuid(&id))) {
            StringFromGUID2(id, guid, ARRAYSIZE(guid));
            dir = std::wstring(base) + L"Vizu-vector-" + guid;
            if (!CreateDirectoryW(dir.c_str(), nullptr)) dir.clear();
        }
    }
    ~Temp() {
        if (dir.empty()) return;
        WIN32_FIND_DATAW d; HANDLE f = FindFirstFileW(At(L"*").c_str(), &d);
        if (f != INVALID_HANDLE_VALUE) {
            do if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(At(d.cFileName).c_str()); while (FindNextFileW(f, &d));
            FindClose(f);
        }
        RemoveDirectoryW(dir.c_str());
    }
    Temp(const Temp&) = delete; Temp& operator=(const Temp&) = delete;
};
inline std::wstring BundledEngine(const std::wstring& directory) {
    for (auto relative : {L"\\vector-engine\\inkscape\\bin\\inkscape.com", L"\\vector-engine\\inkscape\\bin\\inkscape.exe"}) {
        std::wstring candidate = directory + relative;
        if (Exists(candidate)) return candidate;
    }
    return L"";
}
inline std::wstring Engine() {
    wchar_t path[32768] = {};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    PathRemoveFileSpecW(path);
    std::wstring bundled = BundledEngine(path);
    if (!bundled.empty()) return bundled;
    DWORD n = GetEnvironmentVariableW(L"VIZU_INKSCAPE", path, ARRAYSIZE(path));
    if (n) return n < ARRAYSIZE(path) && Exists(path) ? path : L"";
    for (auto env : {L"ProgramW6432", L"ProgramFiles"}) {
        n = GetEnvironmentVariableW(env, path, ARRAYSIZE(path));
        if (!n || n >= ARRAYSIZE(path)) continue;
        for (auto exe : {L"\\Inkscape\\bin\\inkscape.com", L"\\Inkscape\\bin\\inkscape.exe"}) {
            std::wstring candidate = std::wstring(path) + exe;
            if (Exists(candidate)) return candidate;
        }
    }
    return L"";
}
// Edição instalada: 0 = Vizu Lite (prévia e miniaturas), 1 = Vizu (+ conversões), 2 = Vizu Ultra (+ SVG/EPS vetorial).
enum { kLite = 0, kStandard = 1, kUltra = 2 };
inline DWORD Edition() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\DesignThumbs", L"Edition", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v > kUltra ? kStandard : v;
    // Sem a chave (atualização de versões antigas): quem já tinha o conversor da 9.7 fica no Ultra; os demais no Vizu.
    size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\DesignThumbs", L"VectorConverterInstalled", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v)
        return kUltra;
    return kStandard;
}
inline bool Selected() { return Edition() == kUltra; }
inline bool Available() { return Selected() && !Engine().empty(); }
inline bool Run(const std::wstring& engine, const std::vector<std::wstring>& args,
                const std::wstring& logPath, std::wstring& log, std::wstring& error, DWORD timeout = 300000) {
    std::wstring cmd = Quote(engine);
    for (const auto& a : args) cmd += L" " + Quote(a);
    std::vector<wchar_t> command(cmd.begin(), cmd.end()); command.push_back(0);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE output = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        error = T(L"Não foi possível criar o log do motor.", L"Couldn't create the engine log.", L"No se pudo crear el registro del motor."); return false;
    }
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> attr(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr.data());
    bool initialized = InitializeProcThreadAttributeList(list, 1, 0, &size) != FALSE;
    HANDLE inherit[] = {output, input};
    bool ready = initialized && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    STARTUPINFOEXW si = {}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = list;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input; si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = output;
    PROCESS_INFORMATION pi = {};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ready = ready && job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    bool started = ready && CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                       nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD last = GetLastError(), code = 1;
    if (started) {
        if (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == (DWORD)-1) {
            TerminateProcess(pi.hProcess, 1); error = T(L"Falha ao isolar o processo de conversão.", L"Couldn't isolate the conversion process.", L"No se pudo aislar el proceso de conversión.");
        } else if (WaitForSingleObject(pi.hProcess, timeout) != WAIT_OBJECT_0) {
            TerminateJobObject(job, 1); error = T(L"Tempo limite no processo (", L"Conversion timed out (", L"Tiempo límite en el proceso (") + std::to_wstring(timeout / 1000) + L" s).";
        } else GetExitCodeProcess(pi.hProcess, &code);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else error = T(L"Não foi possível iniciar o Inkscape (Windows ", L"Couldn't start Inkscape (Windows ", L"No se pudo iniciar Inkscape (Windows ") + std::to_wstring(last) + L").";
    if (job) CloseHandle(job);
    if (initialized) DeleteProcThreadAttributeList(list);
    CloseHandle(input); CloseHandle(output);
    std::string bytes; if (Read(logPath, bytes, 4u << 20)) log += Wide(bytes) + L"\n";
    if (started && code && error.empty()) error = T(L"O Inkscape recusou o documento (código ", L"Inkscape rejected the document (code ", L"Inkscape rechazó el documento (código ") + std::to_wstring(code) + L").";
    return started && code == 0 && error.empty();
}

struct Audit {
    unsigned images = 0, objects = 0, paths = 0, groups = 0, texts = 0, pages = 0;
    std::wstring width, height, viewBox;
    std::set<std::wstring> epsBlocks;
};
inline bool Alpha(const std::wstring& s) {
    wchar_t* end = nullptr; double value = wcstod(s.c_str(), &end);
    if (end == s.c_str()) return true;
    if (*end == L'%') { value /= 100; ++end; }
    while (iswspace(*end)) ++end;
    return *end != 0 || value != 1;
}
inline bool AuditSvg(const std::wstring& path, Audit& a, std::wstring& error) {
    IStream* stream = nullptr; IXmlReader* reader = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (SUCCEEDED(hr)) hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&reader, nullptr);
    if (SUCCEEDED(hr)) hr = reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (SUCCEEDED(hr)) hr = reader->SetProperty(XmlReaderProperty_MaxElementDepth, 256);
    if (SUCCEEDED(hr)) hr = reader->SetInput(stream);
    std::set<std::wstring> ids, refs;
    bool root = false; XmlNodeType type;
    UINT definitionDepth = ~0u; unsigned renderable = 0;
    const wchar_t* svgNs = L"http://www.w3.org/2000/svg";
    while (SUCCEEDED(hr) && (hr = reader->Read(&type)) == S_OK) {
        UINT depth = 0; reader->GetDepth(&depth);
        // XmlLite reports end-element depth one level below its opening element.
        if (type == XmlNodeType_EndElement && definitionDepth != ~0u && depth == definitionDepth + 1) definitionDepth = ~0u;
        if (type != XmlNodeType_Element) continue;
        const wchar_t *name = nullptr, *ns = nullptr;
        reader->GetLocalName(&name, nullptr); reader->GetNamespaceUri(&ns, nullptr);
        std::wstring tag = name ? name : L"";
        if (!root) {
            root = true;
            if (tag != L"svg" || !ns || std::wstring(ns) != svgNs) { error = T(L"Documento SVG inválido.", L"Invalid SVG document.", L"Documento SVG no válido."); break; }
        }
        bool svg = ns && std::wstring(ns) == svgNs;
        if (svg && definitionDepth == ~0u && !reader->IsEmptyElement())
            for (const wchar_t* container : {L"defs", L"symbol", L"clipPath", L"mask", L"pattern", L"marker"})
                if (tag == container) { definitionDepth = depth; break; }
        if (svg && tag == L"image") ++a.images;
        if (svg && (tag == L"feImage" || tag == L"foreignObject" || tag == L"script" ||
                    tag == L"style" || tag == L"animate" || tag == L"animateTransform" || tag == L"set")) {
            error = T(L"SVG recusado: elemento ", L"SVG rejected: element ", L"SVG rechazado: elemento ") + tag + T(L" (bitmap, conteúdo ativo ou estilo não auditável).", L" (bitmap, active content or style that can't be checked).", L" (mapa de bits, contenido activo o estilo no verificable)."); break;
        }
        if (svg) {
            if (tag == L"path") ++a.paths;
            if (tag == L"g") ++a.groups;
            if (tag == L"text") ++a.texts;
            for (const wchar_t* shape : {L"path", L"rect", L"circle", L"ellipse", L"line", L"polyline", L"polygon", L"text", L"use", L"image"})
                if (tag == shape) { ++a.objects; if (definitionDepth == ~0u) ++renderable; break; }
            for (const wchar_t* effect : {L"filter", L"mask", L"pattern", L"meshgradient", L"meshGradient"})
                if (tag == effect) a.epsBlocks.insert(tag);
        }
        if (tag == L"page") ++a.pages;
        std::map<std::wstring, std::wstring> attrs;
        if (reader->MoveToFirstAttribute() == S_OK) {
            do {
                const wchar_t *key = nullptr, *val = nullptr;
                reader->GetLocalName(&key, nullptr); reader->GetValue(&val, nullptr);
                attrs[key ? key : L""] = val ? val : L"";
            } while (reader->MoveToNextAttribute() == S_OK);
            reader->MoveToElement();
        }
        if (tag == L"svg" && a.width.empty()) { a.width = attrs[L"width"]; a.height = attrs[L"height"]; a.viewBox = attrs[L"viewBox"]; }
        if (attrs.count(L"id") && !ids.insert(attrs[L"id"]).second) { error = T(L"IDs duplicados no SVG.", L"Duplicate IDs in the SVG.", L"IDs duplicados en el SVG."); break; }
        for (const auto& p : attrs) {
            std::wstring key = Lower(p.first), val = Trim(p.second), low = Lower(val);
            if (key.size() > 2 && key.substr(0, 2) == L"on") { error = T(L"Evento ativo no SVG.", L"Active event in the SVG.", L"Evento activo en el SVG."); break; }
            if (key == L"href") {
                bool embedded = tag == L"image" && low.compare(0, 11, L"data:image/") == 0 &&
                    (low.compare(11, 4, L"png;") == 0 || low.compare(11, 5, L"jpeg;") == 0 || low.compare(11, 4, L"jpg;") == 0);
                if (embedded) continue; // bitmap embutido no próprio documento (comum em CDR)
                if (val.empty() || val[0] != L'#') { error = T(L"Referência externa no SVG.", L"External reference in the SVG.", L"Referencia externa en el SVG."); break; }
                refs.insert(val.substr(1));
            }
            size_t pos = 0;
            while ((pos = low.find(L"url(", pos)) != std::wstring::npos) {
                size_t end = low.find(L')', pos + 4);
                std::wstring ref = Trim(val.substr(pos + 4, end - (pos + 4)));
                if (end == std::wstring::npos || ref.empty() || ref[0] != L'#') { error = T(L"Recurso externo no SVG.", L"External resource in the SVG.", L"Recurso externo en el SVG."); break; }
                refs.insert(ref.substr(1)); pos = end + 1;
            }
            if (!error.empty()) break;
        }
        if (!error.empty()) break;
        std::wstring style = attrs[L"style"]; size_t start = 0;
        while (start < style.size()) {
            size_t end = style.find(L';', start), colon = style.find(L':', start);
            if (colon != std::wstring::npos && (end == std::wstring::npos || colon < end))
                attrs[Lower(Trim(style.substr(start, colon - start)))] = Trim(style.substr(colon + 1, end == std::wstring::npos ? end : end - colon - 1));
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        for (const auto& p : attrs) {
            std::wstring key = Lower(p.first), val = Lower(Trim(p.second));
            if ((key == L"opacity" || key == L"fill-opacity" || key == L"stroke-opacity" || key == L"stop-opacity") && Alpha(val)) a.epsBlocks.insert(L"transparência");
            if ((key == L"filter" || key == L"mask" || key == L"mix-blend-mode" || key == L"isolation") &&
                val != L"none" && val != L"normal" && val != L"auto") a.epsBlocks.insert(key);
            if (key == L"fill" || key == L"stroke" || key == L"stop-color") {
                if (val.find(L"rgba(") != std::wstring::npos || val.find(L"hsla(") != std::wstring::npos ||
                    val.find(L'/') != std::wstring::npos || val == L"transparent" ||
                    (val.size() && val[0] == L'#' && (val.size() == 5 || val.size() == 9))) a.epsBlocks.insert(L"cor com alfa");
            }
        }
    }
    if (reader) reader->Release(); if (stream) stream->Release();
    if (error.empty() && (FAILED(hr) || !root)) error = T(L"XML inválido ou DTD proibido no SVG.", L"Invalid XML or forbidden DTD in the SVG.", L"XML no válido o DTD prohibido en el SVG.");
    if (error.empty() && !renderable) error = T(L"O importador não produziu objetos vetoriais desenháveis; saída vazia recusada.", L"The importer produced no drawable vector objects; empty output refused.", L"El importador no produjo objetos vectoriales dibujables; salida vacía rechazada.");
    if (error.empty()) for (const auto& ref : refs) if (!ids.count(ref)) { error = T(L"Referência interna quebrada no SVG.", L"Broken internal reference in the SVG.", L"Referencia interna rota en el SVG."); break; }
    if (a.pages > 1) a.epsBlocks.insert(L"múltiplas páginas");
    return error.empty();
}
inline bool AuditEps(const std::wstring& path, bool allowImages, std::wstring& error) {
    std::string s;
    if (!Read(path, s) || s.find("%!PS-Adobe-") != 0 || s.substr(0, 100).find("EPSF-") == std::string::npos || s.find("%%BoundingBox:") == std::string::npos) {
        error = T(L"Cabeçalho EPS inválido.", L"Invalid EPS header.", L"Cabecera EPS no válida."); return false;
    }
    auto a = s.find("%%BeginProlog"), b = s.find("%%EndProlog");
    if (allowImages) return true; // bitmaps do original: o operador image é legítimo
    if (a != std::string::npos && b != std::string::npos && b > a) s.erase(a, b + 11 - a);
    s += '\n'; // Flush the last PostScript token, even without a trailing newline.
    // Engine-generated Cairo EPS only: reject image invocation in the page body.
    std::string token; bool comment = false;
    for (unsigned char c : s) {
        if (c == '%') comment = true;
        if (c == '\n' || c == '\r') comment = false;
        if (comment) continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') token += (char)c;
        else {
            if (token == "image" || token == "colorimage" || token == "imagemask" || token == "cairo_image" || token == "cairo_imagemask") {
                error = T(L"EPS contém operador de imagem; exportação vetorial recusada.", L"EPS contains an image operator; vector export refused.", L"El EPS contiene un operador de imagen; exportación vectorial rechazada."); return false;
            }
            token.clear();
        }
    }
    return true;
}
struct Result { bool ok = false; std::wstring error, report; };
inline Result Convert(const std::wstring& source, const std::wstring& destination, bool eps, bool curves) {
    Result r; Temp tmp;
    if (!Selected()) { r.error = T(L"O conversor vetorial não foi selecionado na instalação do Vizu.", L"The vector converter wasn't selected in the Vizu installation.", L"El conversor vectorial no se seleccionó en la instalación de Vizu."); return r; }
    if (!IsInput(source)) { r.error = T(L"Conversão vetorial aceita CDR ou SVG. Uma thumbnail não contém os vetores originais.", L"Vector conversion accepts CDR or SVG. A thumbnail doesn't contain the original vectors.", L"La conversión vectorial acepta CDR o SVG. Una miniatura no contiene los vectores originales."); return r; }
    if (tmp.dir.empty()) { r.error = T(L"Não foi possível criar a pasta temporária.", L"Couldn't create the temporary folder.", L"No se pudo crear la carpeta temporal."); return r; }
    std::wstring engine = Engine();
    if (engine.empty()) {
        r.error = T(L"O motor vetorial incluído não foi encontrado. Execute o instalador completo do Vizu para reparar a instalação.", L"The bundled vector engine wasn't found. Run the full Vizu installer to repair the installation.", L"No se encontró el motor vectorial incluido. Ejecuta el instalador completo de Vizu para reparar la instalación."); return r;
    }
    std::string input;
    if (!Read(source, input)) { r.error = T(L"Arquivo ausente, vazio ou maior que 100 MiB.", L"File missing, empty or larger than 100 MiB.", L"Archivo ausente, vacío o mayor de 100 MiB."); return r; }
    bool isSvg = Lower(PathFindExtensionW(source.c_str())) == L".svg";
    std::wstring snapshot = tmp.At(isSvg ? L"source.svg" : L"source.cdr");
    // CREATE_NEW snapshot of the bytes inspected; all subsequent steps use it.
    HANDLE snap = CreateFileW(snapshot.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    DWORD written = 0;
    bool copied = snap != INVALID_HANDLE_VALUE && WriteFile(snap, input.data(), (DWORD)input.size(), &written, nullptr) && written == input.size();
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    if (!copied) { r.error = T(L"Falha ao criar cópia de trabalho do documento.", L"Couldn't create a working copy of the document.", L"No se pudo crear una copia de trabajo del documento."); return r; }
    input.clear();
    if (isSvg) { Audit before; if (!AuditSvg(snapshot, before, r.error)) return r; }
    std::wstring logs, svg = tmp.At(L"document.svg"), output = svg;
    std::vector<std::wstring> args = {snapshot, L"--export-type=svg", L"--export-plain-svg", L"--export-area-page", L"--export-filename=" + svg};
    if (curves) args.push_back(L"--export-text-to-path");
    if (!Run(engine, args, tmp.At(L"engine.log"), logs, r.error)) { r.error += L"\n" + logs; return r; }
    Audit audit;
    if (!AuditSvg(svg, audit, r.error)) { r.error += L"\n" + logs; return r; }
    if (curves && audit.texts) { r.error = T(L"O motor não converteu todos os textos em curvas.", L"The engine didn't convert all text to curves.", L"El motor no convirtió todos los textos en curvas."); return r; }
    if (eps && !audit.epsBlocks.empty()) {
        r.error = T(L"EPS bloqueado para evitar rasterização ou perda de efeitos: ", L"EPS blocked to avoid rasterization or loss of effects: ", L"EPS bloqueado para evitar la rasterización o la pérdida de efectos: ");
        for (const auto& reason : audit.epsBlocks) r.error += reason + L"; ";
        r.error += T(L"use SVG para este documento.", L"use SVG for this document.", L"usa SVG para este documento."); return r;
    }
    if (eps) {
        output = tmp.At(L"output.eps");
        args = {svg, L"--export-type=eps", L"--export-area-page", L"--export-ignore-filters", L"--export-filename=" + output};
        if (curves) args.push_back(L"--export-text-to-path");
        if (!Run(engine, args, tmp.At(L"engine.log"), logs, r.error) || !AuditEps(output, audit.images > 0, r.error)) { r.error += L"\n" + logs; return r; }
    }
    // Destination chosen with UniquePath, but CREATE_NEW semantics still prevent races/overwrites.
    if (!CopyFileW(output.c_str(), destination.c_str(), TRUE)) { r.error = T(L"Não foi possível salvar a saída; o destino pode já existir.", L"Couldn't save the output; the destination may already exist.", L"No se pudo guardar la salida; el destino puede existir ya."); return r; }
    r.ok = true;
    std::wstring report = L"Vizu — conversão vetorial\r\nOrigem: " + source + L"\r\nMotor: " + engine +
        L"\r\nObjetos: " + std::to_wstring(audit.objects) + L"; caminhos: " + std::to_wstring(audit.paths) +
        L"; grupos: " + std::to_wstring(audit.groups) + L"; textos: " + std::to_wstring(audit.texts) +
        L"\r\nDimensões SVG: " + audit.width + L" x " + audit.height + L"; viewBox: " + audit.viewBox +
        L"\r\nTextos em curvas: " + (curves ? L"sim" : L"não") +
        L"\r\nSem rasterização. A auditoria estrutural não certifica equivalência visual com o CDR.\r\n"
        L"Fontes, cores CMYK/spot/ICC e perdas silenciosas do importador precisam de revisão.\r\n"
        L"Bitmaps embutidos no original: " + std::to_wstring(audit.images) + L" (mantidos como imagem).\r\n"
        L"EPS pode reorganizar grupos e textos. Transparências, filtros e máscaras são bloqueados no EPS.\r\n\r\nLog do motor:\r\n" + logs;
    r.report = destination + L".vizu.txt";
    if (!WriteNew(r.report, report)) r.report.clear();
    return r;
}
} // namespace vec
