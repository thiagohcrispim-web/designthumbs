// PdfEdit.h - juntar, dividir e extrair páginas de PDF sem renderizar: os objetos originais (texto, vetores,
// fontes, imagens) são copiados e renumerados. Lê tabelas xref clássicas e xref streams (PDF 1.5+), incluindo
// objetos dentro de object streams. PDFs com senha (/Encrypt) não são suportados.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Formats.h"

namespace pdfedit {
using fmt::Bytes;
using std::min; using std::max;

inline bool IsWs(BYTE c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }
inline bool IsDelim(BYTE c) { return IsWs(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')' || c == '%' || c == '{' || c == '}'; }

// Pula string literal (com parênteses aninhados e escapes), string hex ou comentário a partir de s[i].
inline size_t SkipSpecial(const std::string& s, size_t i) {
    if (s[i] == '(') {
        int depth = 0;
        for (; i < s.size(); ++i) {
            if (s[i] == '\\') { ++i; continue; }
            if (s[i] == '(') ++depth;
            else if (s[i] == ')' && --depth == 0) return i + 1;
        }
        return s.size();
    }
    if (s[i] == '<' && i + 1 < s.size() && s[i + 1] != '<') { size_t e = s.find('>', i); return e == std::string::npos ? s.size() : e + 1; }
    if (s[i] == '%') { while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i; return i; }
    return i;
}

// Valor de uma chave num dicionário (texto), como texto bruto: número, nome, "N G R", [...] ou <<...>>.
inline std::string Get(const std::string& d, const char* key) {
    size_t klen = strlen(key);
    int depth = 0;
    for (size_t i = 0; i < d.size();) {
        char c = d[i];
        if (c == '(' || c == '%' || (c == '<' && i + 1 < d.size() && d[i + 1] != '<')) { i = SkipSpecial(d, i); continue; }
        if (c == '<' && d[i + 1] == '<') { ++depth; i += 2; continue; }
        if (c == '>' && i + 1 < d.size() && d[i + 1] == '>') { --depth; i += 2; continue; }
        if (c == '[') { ++depth; ++i; continue; }
        if (c == ']') { --depth; ++i; continue; }
        if (depth == 1 && c == '/' && d.compare(i, klen, key) == 0 && (i + klen >= d.size() || IsDelim((BYTE)d[i + klen]))) {
            size_t v = i + klen;
            while (v < d.size() && IsWs((BYTE)d[v])) ++v;
            size_t e = v;
            if (d.compare(v, 2, "<<") == 0) { // dicionário aninhado
                int dd = 0;
                for (; e < d.size();) {
                    if (d.compare(e, 2, "<<") == 0) { ++dd; e += 2; continue; }
                    if (d[e] == '(' || d[e] == '<') { e = SkipSpecial(d, e); continue; }
                    if (d.compare(e, 2, ">>") == 0) { e += 2; if (--dd == 0) break; continue; }
                    ++e;
                }
            } else if (d[v] == '[') {
                int dd = 0;
                for (; e < d.size();) {
                    if (d.compare(e, 2, "<<") == 0 || d.compare(e, 2, ">>") == 0) { e += 2; continue; }
                    if (d[e] == '(' || d[e] == '<') { e = SkipSpecial(d, e); continue; }
                    if (d[e] == '[') ++dd;
                    else if (d[e] == ']' && --dd == 0) { ++e; break; }
                    ++e;
                }
            } else if (d[v] == '(' || d[v] == '<') e = SkipSpecial(d, v);
            else {
                if (d[v] == '/') ++e;
                while (e < d.size() && !IsDelim((BYTE)d[e])) ++e;
                // referência "N G R"?
                size_t a = e; while (a < d.size() && IsWs((BYTE)d[a])) ++a;
                size_t b = a; while (b < d.size() && isdigit((BYTE)d[b])) ++b;
                size_t r = b; while (r < d.size() && IsWs((BYTE)d[r])) ++r;
                if (b > a && r < d.size() && d[r] == 'R' && (r + 1 >= d.size() || IsDelim((BYTE)d[r + 1]))) e = r + 1;
            }
            return d.substr(v, e - v);
        }
        ++i;
    }
    return "";
}

inline int RefNum(const std::string& v) { int n = 0, g = 0; char r = 0; return sscanf(v.c_str(), "%d %d %c", &n, &g, &r) == 3 && r == 'R' ? n : -1; }

// Todas as referências "N G R" num texto (fora de strings e comentários).
inline std::vector<int> Refs(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if ((c == '<' || c == '>') && i + 1 < s.size() && s[i + 1] == c) { i += 2; continue; } // << e >>
        if (c == '(' || c == '%' || c == '<') { i = SkipSpecial(s, i); continue; }
        if (isdigit((BYTE)c) && (i == 0 || IsDelim((BYTE)s[i - 1]))) {
            size_t a = i; while (a < s.size() && isdigit((BYTE)s[a])) ++a;
            size_t b = a; while (b < s.size() && IsWs((BYTE)s[b])) ++b;
            size_t g = b; while (g < s.size() && isdigit((BYTE)s[g])) ++g;
            size_t r = g; while (r < s.size() && IsWs((BYTE)s[r])) ++r;
            if (b > a && g > b && r > g && r < s.size() && s[r] == 'R' && (r + 1 >= s.size() || IsDelim((BYTE)s[r + 1]))) {
                out.push_back(atoi(s.c_str() + i));
                i = r + 1;
                continue;
            }
            i = a;
            continue;
        }
        ++i;
    }
    return out;
}

// Troca cada "N G R" por map[N] ("M 0 R" ou "null").
inline std::string Renumber(const std::string& s, const std::map<int, int>& map) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if ((c == '<' || c == '>') && i + 1 < s.size() && s[i + 1] == c) { out.append(s, i, 2); i += 2; continue; } // << e >>
        if (c == '(' || c == '%' || c == '<') { size_t e = SkipSpecial(s, i); out.append(s, i, e - i); i = e; continue; }
        if (isdigit((BYTE)c) && (i == 0 || IsDelim((BYTE)s[i - 1]))) {
            size_t a = i; while (a < s.size() && isdigit((BYTE)s[a])) ++a;
            size_t b = a; while (b < s.size() && IsWs((BYTE)s[b])) ++b;
            size_t g = b; while (g < s.size() && isdigit((BYTE)s[g])) ++g;
            size_t r = g; while (r < s.size() && IsWs((BYTE)s[r])) ++r;
            if (b > a && g > b && r > g && r < s.size() && s[r] == 'R' && (r + 1 >= s.size() || IsDelim((BYTE)s[r + 1]))) {
                auto it = map.find(atoi(s.c_str() + i));
                out += it == map.end() || it->second <= 0 ? std::string("null") : std::to_string(it->second) + " 0 R";
                i = r + 1;
                continue;
            }
            out.append(s, i, a - i);
            i = a;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Remove "/Key valor" de um dicionário.
inline std::string RemoveKey(const std::string& d, const char* key) {
    std::string v = Get(d, key);
    if (v.empty()) return d;
    size_t k = std::string::npos, from = 0;
    for (;;) { // a ocorrência da chave cujo valor é `v`
        size_t p = d.find(key, from);
        if (p == std::string::npos) break;
        size_t q = p + strlen(key);
        while (q < d.size() && IsWs((BYTE)d[q])) ++q;
        if (d.compare(q, v.size(), v) == 0) { k = p; break; }
        from = p + 1;
    }
    if (k == std::string::npos) return d;
    size_t e = d.find(v, k) + v.size();
    return d.substr(0, k) + d.substr(e);
}

// Insere "/Key valor" logo depois do "<<" inicial.
inline std::string AddKey(const std::string& d, const std::string& kv) {
    size_t p = d.find("<<");
    return p == std::string::npos ? d : d.substr(0, p + 2) + kv + " " + d.substr(p + 2);
}

struct Obj { std::string dict; Bytes stream; bool isStream = false, ok = false; };

class File {
public:
    Bytes data;
    std::string trailer;
    std::map<int, std::pair<int, size_t>> xref; // num -> (tipo 1: offset | tipo 2: nº do objstm, índice)
    std::string error;

    bool Load(const std::wstring& path) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { error = "open"; return false; }
        LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
        if (sz.QuadPart > (1ll << 30)) { CloseHandle(h); error = "size"; return false; }
        data.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        bool ok = ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
        CloseHandle(h);
        if (!ok || data.size() < 32 || memcmp(data.data(), "%PDF", 4) != 0) { error = "notpdf"; return false; }
        if (!ReadXrefChain() || Get(trailer, "/Root").empty()) { xref.clear(); trailer.clear(); ScanObjects(); }
        if (!Get(trailer, "/Encrypt").empty()) { error = "encrypted"; return false; }
        if (Get(trailer, "/Root").empty()) { error = "noroot"; return false; }
        return true;
    }

    Obj Load(int num) {
        Obj o;
        auto it = xref.find(num);
        if (it == xref.end()) return o;
        if (it->second.first == 1) return ParseAt(it->second.second);
        return FromObjStm(it->second.second >> 32, (int)(it->second.second & 0xFFFFFFFF), num);
    }

    // Páginas na ordem do documento, com atributos herdados (Resources, MediaBox, CropBox, Rotate).
    struct Page { int num; std::string inherited; };
    std::vector<Page> Pages() {
        std::vector<Page> out;
        Obj cat = Load(RefNum(Get(trailer, "/Root")));
        Walk(RefNum(Get(cat.dict, "/Pages")), "", out, 0);
        return out;
    }

private:
    std::string Text(size_t a, size_t b) { return std::string((const char*)&data[a], (const char*)&data[min(b, data.size())]); }

    // Dicionário que começa em `p` ("<<"), até o ">>" correspondente.
    size_t DictEnd(const std::string& s, size_t p) {
        int depth = 0;
        for (size_t i = p; i < s.size();) {
            if (s[i] == '(' || s[i] == '%' || (s[i] == '<' && i + 1 < s.size() && s[i + 1] != '<')) { i = SkipSpecial(s, i); continue; }
            if (s.compare(i, 2, "<<") == 0) { ++depth; i += 2; continue; }
            if (s.compare(i, 2, ">>") == 0) { i += 2; if (--depth == 0) return i; continue; }
            ++i;
        }
        return s.size();
    }

    int IntValue(const std::string& v) {
        int n = RefNum(v);
        if (n < 0) return atoi(v.c_str());
        Obj o = Load(n);
        return atoi(o.dict.c_str());
    }

    // Objeto "N G obj ... endobj" no offset.
    Obj ParseAt(size_t off) {
        Obj o;
        if (off >= data.size()) return o;
        std::string head = Text(off, off + 64);
        size_t ob = head.find("obj");
        if (ob == std::string::npos) return o;
        size_t start = off + ob + 3;
        // limite: até "endobj" ou "stream"
        const BYTE* p = data.data();
        const BYTE* end = p + data.size();
        const BYTE* s = fmt::Find(p + start, end, "stream", 6);
        const BYTE* e = fmt::Find(p + start, end, "endobj", 6);
        if (!e) e = end;
        if (s && s < e) { // objeto com stream
            std::string pre = Text(start, s - p);
            size_t d = pre.find("<<");
            o.dict = d == std::string::npos ? pre : pre.substr(d, DictEnd(pre, d) - d);
            size_t ds = (s - p) + 6;
            if (ds < data.size() && data[ds] == '\r') ++ds;
            if (ds < data.size() && data[ds] == '\n') ++ds;
            int len = IntValue(Get(o.dict, "/Length"));
            if (len < 0 || ds + len > data.size() || Text(ds + len, ds + len + 20).find("endstream") == std::string::npos) {
                const BYTE* es = fmt::Find(p + ds, end, "endstream", 9); // /Length errado: procura o fim
                len = es ? (int)(es - p - ds) : 0;
                while (len > 0 && (data[ds + len - 1] == '\n' || data[ds + len - 1] == '\r')) --len;
            }
            o.stream.assign(p + ds, p + ds + len);
            o.isStream = true;
        } else {
            o.dict = Text(start, e - p);
            while (!o.dict.empty() && IsWs((BYTE)o.dict.back())) o.dict.pop_back();
            size_t a = 0; while (a < o.dict.size() && IsWs((BYTE)o.dict[a])) ++a;
            o.dict.erase(0, a);
        }
        o.ok = true;
        return o;
    }

    // Dados decodificados de um stream (só FlateDecode, com preditor PNG).
    bool Decode(const Obj& o, Bytes& out) {
        std::string f = Get(o.dict, "/Filter");
        if (f.empty()) { out = o.stream; return true; }
        if (f.find("FlateDecode") == std::string::npos || f.find('/', f.find("FlateDecode") + 1) != std::string::npos) return false;
        if (o.stream.size() < 2) return false;
        if (!fmt::Inflater(&o.stream[2], o.stream.size() - 2, out, 256u << 20).Run() && out.empty()) return false; // pula o cabeçalho zlib
        std::string parms = Get(o.dict, "/DecodeParms");
        int pred = atoi(Get(parms, "/Predictor").c_str()), cols = atoi(Get(parms, "/Columns").c_str());
        if (pred >= 10 && cols > 0) { // preditor PNG (linha a linha)
            Bytes res;
            size_t row = cols + 1;
            Bytes prev(cols, 0);
            for (size_t r = 0; r + row <= out.size(); r += row) {
                BYTE type = out[r];
                Bytes cur(out.begin() + r + 1, out.begin() + r + row);
                for (int i = 0; i < cols; ++i) {
                    BYTE up = prev[i], left = i ? cur[i - 1] : 0, ul = i ? prev[i - 1] : 0;
                    if (type == 1) cur[i] += left;
                    else if (type == 2) cur[i] += up;
                    else if (type == 3) cur[i] += (BYTE)((left + up) / 2);
                    else if (type == 4) { int pp = left + up - ul, pa = abs(pp - left), pb = abs(pp - up), pc = abs(pp - ul); cur[i] += pa <= pb && pa <= pc ? left : pb <= pc ? up : ul; }
                }
                res.insert(res.end(), cur.begin(), cur.end());
                prev = cur;
            }
            out.swap(res);
        }
        return true;
    }

    std::map<int, std::pair<std::string, Bytes>> objStmCache_;
    Obj FromObjStm(size_t stmNum, int index, int num) {
        Obj o;
        auto& c = objStmCache_[(int)stmNum];
        if (c.second.empty()) {
            Obj s = ParseAt(xref.count((int)stmNum) && xref[(int)stmNum].first == 1 ? xref[(int)stmNum].second : SIZE_MAX);
            if (!s.ok || !Decode(s, c.second)) return o;
            c.first = s.dict;
        }
        int n = atoi(Get(c.first, "/N").c_str()), first = atoi(Get(c.first, "/First").c_str());
        std::string hdr((const char*)c.second.data(), min((size_t)first, c.second.size()));
        std::vector<std::pair<int, int>> items;
        const char* q = hdr.c_str();
        for (int i = 0; i < n; ++i) { int on, oo, used; if (sscanf(q, "%d %d%n", &on, &oo, &used) != 2) break; items.push_back({on, oo}); q += used; }
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].first != num && (int)i != index) continue;
            size_t a = first + items[i].second, b = i + 1 < items.size() ? first + items[i + 1].second : c.second.size();
            if (a > c.second.size()) return o;
            o.dict.assign((const char*)&c.second[a], (const char*)&c.second[min(b, c.second.size())]);
            while (!o.dict.empty() && IsWs((BYTE)o.dict.back())) o.dict.pop_back();
            o.ok = true;
            return o;
        }
        return o;
    }

    bool ReadXrefChain() {
        std::string tail = Text(data.size() > 1100 ? data.size() - 1100 : 0, data.size());
        size_t sx = tail.rfind("startxref");
        if (sx == std::string::npos) return false;
        size_t off = (size_t)atoll(tail.c_str() + sx + 9);
        std::set<size_t> seen;
        while (off && off < data.size() && seen.insert(off).second) {
            std::string tr;
            if (!ReadXrefAt(off, &tr)) return false;
            if (trailer.empty()) trailer = tr;
            std::string stm = Get(tr, "/XRefStm"); // arquivos híbridos
            if (!stm.empty()) { std::string t2; ReadXrefAt((size_t)atoll(stm.c_str()), &t2); }
            std::string prev = Get(tr, "/Prev");
            off = prev.empty() ? 0 : (size_t)atoll(prev.c_str());
        }
        return !xref.empty();
    }

    bool ReadXrefAt(size_t off, std::string* tr) {
        size_t a = off; while (a < data.size() && IsWs(data[a])) ++a;
        if (Text(a, a + 4) == "xref") { // tabela clássica
            size_t p = a + 4;
            for (;;) {
                while (p < data.size() && IsWs(data[p])) ++p;
                if (Text(p, p + 7) == "trailer") {
                    std::string t = Text(p + 7, p + 7 + 8192);
                    size_t d = t.find("<<");
                    if (d == std::string::npos) return false;
                    *tr = t.substr(d, DictEnd(t, d) - d);
                    return true;
                }
                unsigned start = 0, count = 0; int used = 0;
                std::string line = Text(p, p + 40);
                if (sscanf(line.c_str(), "%u %u%n", &start, &count, &used) != 2) return false;
                p += used;
                while (p < data.size() && IsWs(data[p])) ++p;
                for (unsigned i = 0; i < count && p + 18 <= data.size(); ++i, p += 20) {
                    std::string e = Text(p, p + 18);
                    if (e.size() >= 17 && e[17] == 'n' && !xref.count(start + i)) xref[start + i] = {1, (size_t)atoll(e.c_str())};
                    // tolera finais de linha de 1 byte
                    if (p + 19 < data.size() && !IsWs(data[p + 19]) && IsWs(data[p + 18])) --p;
                }
            }
        }
        Obj o = ParseAt(a); // xref stream
        if (!o.ok || !o.isStream || Get(o.dict, "/Type").find("XRef") == std::string::npos) return false;
        Bytes d;
        if (!Decode(o, d)) return false;
        int w[3] = {};
        sscanf(Get(o.dict, "/W").c_str(), "[ %d %d %d", &w[0], &w[1], &w[2]);
        int size = atoi(Get(o.dict, "/Size").c_str());
        std::vector<int> idx;
        std::string ix = Get(o.dict, "/Index");
        if (ix.empty()) idx = {0, size};
        else { const char* q = ix.c_str() + 1; int v, used; while (sscanf(q, "%d%n", &v, &used) == 1) { idx.push_back(v); q += used; } }
        size_t rec = w[0] + w[1] + w[2], pos = 0;
        auto field = [&](size_t at, int n) { UINT64 v = 0; for (int i = 0; i < n; ++i) v = v << 8 | d[at + i]; return v; };
        for (size_t s = 0; s + 1 < idx.size(); s += 2)
            for (int i = 0; i < idx[s + 1] && pos + rec <= d.size(); ++i, pos += rec) {
                UINT64 type = w[0] ? field(pos, w[0]) : 1, f2 = field(pos + w[0], w[1]), f3 = field(pos + w[0] + w[1], w[2]);
                int num = idx[s] + i;
                if (xref.count(num)) continue;
                if (type == 1) xref[num] = {1, (size_t)f2};
                else if (type == 2) xref[num] = {2, (size_t)(f2 << 32 | f3)};
            }
        *tr = o.dict;
        return true;
    }

    // Sem xref válida: procura "N G obj" no arquivo (o último vence) e o trailer/catálogo.
    void ScanObjects() {
        std::string all((const char*)data.data(), data.size());
        for (size_t p = all.find(" obj"); p != std::string::npos; p = all.find(" obj", p + 4)) {
            size_t g = p; while (g > 0 && isdigit((BYTE)all[g - 1])) --g;
            if (g == p || g == 0 || !IsWs((BYTE)all[g - 1])) continue;
            size_t n = g - 1; while (n > 0 && IsWs((BYTE)all[n - 1])) --n;
            size_t s = n; while (s > 0 && isdigit((BYTE)all[s - 1])) --s;
            if (s == n) continue;
            xref[atoi(all.c_str() + s)] = {1, s};
        }
        size_t t = all.rfind("trailer");
        if (t != std::string::npos) { size_t d = all.find("<<", t); if (d != std::string::npos) trailer = all.substr(d, DictEnd(all, d) - d); }
        if (Get(trailer, "/Root").empty()) // procura o catálogo
            for (auto& x : xref) { Obj o = Load(x.first); if (o.ok && Get(o.dict, "/Type") == "/Catalog") { trailer = "<< /Root " + std::to_string(x.first) + " 0 R >>"; break; } }
    }

    void Walk(int num, std::string inherited, std::vector<Page>& out, int depth) {
        if (num < 0 || depth > 64) return;
        Obj o = Load(num);
        if (!o.ok) return;
        for (const char* k : {"/Resources", "/MediaBox", "/CropBox", "/Rotate"}) {
            std::string v = Get(o.dict, k);
            if (!v.empty()) { inherited = RemoveKey(inherited.empty() ? "<<>>" : inherited, k); inherited = AddKey(inherited, std::string(k) + " " + v); }
        }
        std::string type = Get(o.dict, "/Type");
        if (type == "/Page" || (type.empty() && Get(o.dict, "/Kids").empty())) { out.push_back({num, inherited}); return; }
        for (int kid : Refs(Get(o.dict, "/Kids"))) Walk(kid, inherited, out, depth + 1);
    }
};

// Uma entrada: arquivo + páginas (0-based) a copiar, na ordem.
struct Part { File* file; std::vector<int> pages; };

// Monta um PDF novo com as páginas pedidas. Objetos compartilhados (fontes, imagens) são copiados uma vez por arquivo.
inline bool Assemble(const std::vector<Part>& parts, const std::wstring& outPath) {
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offs = {0, 0}; // 1 = catálogo, 2 = árvore de páginas
    std::vector<int> pageNums;
    auto newObj = [&]() { offs.push_back(0); return (int)offs.size(); };
    for (auto& part : parts) {
        File& f = *part.file;
        std::vector<File::Page> pages = f.Pages();
        std::set<int> wanted;
        std::map<int, int> map; // número original -> novo (0 = null)
        for (int pi : part.pages) if (pi >= 0 && pi < (int)pages.size()) wanted.insert(pages[pi].num);
        // descobre todos os objetos usados pelas páginas escolhidas (sem seguir /Parent nem outras páginas)
        std::vector<int> stack;
        for (int pi : part.pages) if (pi >= 0 && pi < (int)pages.size() && !map.count(pages[pi].num)) { map[pages[pi].num] = newObj(); stack.push_back(pages[pi].num); }
        std::map<int, Obj> objs;
        while (!stack.empty()) {
            int n = stack.back(); stack.pop_back();
            Obj o = f.Load(n);
            if (!o.ok) { map[n] = 0; continue; }
            std::string body = o.dict;
            if (wanted.count(n)) body = RemoveKey(body, "/Parent");
            for (int r : Refs(body)) {
                if (map.count(r)) continue;
                Obj ro = f.Load(r);
                std::string t = ro.ok ? Get(ro.dict, "/Type") : "";
                if (!ro.ok || ((t == "/Page" || t == "/Pages") && !wanted.count(r))) { map[r] = 0; continue; } // links para fora viram null
                map[r] = newObj();
                stack.push_back(r);
            }
            objs[n] = o;
        }
        for (auto& kv : objs) {
            int n = kv.first;
            Obj& o = kv.second;
            std::string body = o.dict;
            if (wanted.count(n)) { // página: novo pai e atributos herdados
                body = RemoveKey(body, "/Parent");
                for (auto& pg : pages) if (pg.num == n)
                    for (const char* k : {"/Resources", "/MediaBox", "/CropBox", "/Rotate"})
                        if (Get(body, k).empty() && !Get(pg.inherited, k).empty()) body = AddKey(body, std::string(k) + " " + Get(pg.inherited, k));
                body = AddKey(body, "/Parent 2 0 R");
            }
            body = Renumber(body, map);
            int nn = map[n];
            offs[nn - 1] = out.size();
            out += std::to_string(nn) + " 0 obj\n";
            if (o.isStream) {
                body = RemoveKey(body, "/Length");
                body = AddKey(body, "/Length " + std::to_string(o.stream.size()));
                out += body + "\nstream\n";
                out.append((const char*)o.stream.data(), o.stream.size());
                out += "\nendstream";
            } else out += body;
            out += "\nendobj\n";
        }
        for (int pi : part.pages) if (pi >= 0 && pi < (int)pages.size()) pageNums.push_back(map[pages[pi].num]);
    }
    if (pageNums.empty()) return false;
    offs[0] = out.size();
    out += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    offs[1] = out.size();
    out += "2 0 obj\n<< /Type /Pages /Count " + std::to_string(pageNums.size()) + " /Kids [";
    for (int p : pageNums) out += std::to_string(p) + " 0 R ";
    out += "] >>\nendobj\n";
    size_t xr = out.size();
    out += "xref\n0 " + std::to_string(offs.size() + 1) + "\n0000000000 65535 f \n";
    char line[32];
    for (size_t o : offs) { snprintf(line, sizeof(line), "%010zu 00000 n \n", o); out += line; }
    out += "trailer\n<< /Size " + std::to_string(offs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xr) + "\n%%EOF\n";
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr) && w == out.size();
    CloseHandle(h);
    if (!ok) DeleteFileW(outPath.c_str());
    return ok;
}

// "1-3, 5, 8-" -> índices 0-based válidos (vazio = todas).
inline std::vector<int> ParseRange(const std::wstring& s, int count) {
    std::vector<int> out;
    std::wstring t;
    for (wchar_t c : s) t += c == L';' ? L',' : c;
    if (t.find_first_not_of(L" ,") == std::wstring::npos) { for (int i = 0; i < count; ++i) out.push_back(i); return out; }
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find(L',', p);
        std::wstring part = t.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
        p = e == std::wstring::npos ? t.size() : e + 1;
        int a = 0, b = 0;
        size_t dash = part.find(L'-');
        if (dash == std::wstring::npos) { a = b = _wtoi(part.c_str()); }
        else {
            a = _wtoi(part.substr(0, dash).c_str());
            std::wstring rest = part.substr(dash + 1);
            b = rest.find_first_not_of(L" ") == std::wstring::npos ? count : _wtoi(rest.c_str());
            if (!a) a = 1;
        }
        for (int i = a; i <= b && i <= count; ++i) if (i >= 1) out.push_back(i - 1);
    }
    return out;
}

} // namespace pdfedit
