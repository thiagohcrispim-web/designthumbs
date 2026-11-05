// Extras.h - ferramentas de designer: camadas do PSD em PNG, paletas (folha de cores, HEX/CSS, ASE <-> ACO)
// e folha de amostra de fontes.
#pragma once
#include <map>
#include "Ops.h"

namespace extras {
using fmt::Bytes; using fmt::BE;
using std::min; using std::max;

// ---------- camadas do PSD ----------

struct Layer { std::wstring name; int top, left, bottom, right; bool hidden; HBITMAP bmp = nullptr; };

// Decodifica um canal de camada (raw, RLE, ZIP, ZIP com predição) para 8 bits por pixel.
inline bool LayerChannel(const BYTE* p, size_t n, UINT w, UINT h, UINT depth, bool psb, Bytes& out) {
    out.assign((size_t)w * h, 0);
    if (n < 2 || !w || !h) return n >= 2;
    UINT comp = (UINT)BE(p, 2);
    p += 2; n -= 2;
    UINT bpc = depth / 8;
    size_t rowBytes = (size_t)w * bpc;
    Bytes raw;
    if (comp == 0) raw.assign(p, p + min(n, rowBytes * h));
    else if (comp == 1) { // PackBits, com a tabela de tamanhos das linhas
        int cb = psb ? 4 : 2;
        if (n < (size_t)h * cb) return false;
        size_t pos = (size_t)h * cb;
        raw.resize(rowBytes * h);
        for (UINT y = 0; y < h; ++y) {
            size_t len = (size_t)BE(p + (size_t)y * cb, cb), i = 0, o = 0;
            const BYTE* s = p + pos;
            if (pos + len > n) return false;
            BYTE* row = &raw[y * rowBytes];
            while (i < len && o < rowBytes) {
                signed char k = (signed char)s[i++];
                if (k >= 0) { size_t c = min((size_t)k + 1, min(rowBytes - o, len - i)); memcpy(row + o, s + i, c); i += k + 1; o += c; }
                else if (k != -128) { if (i >= len) break; size_t c = min((size_t)(1 - k), rowBytes - o); memset(row + o, s[i++], c); o += c; }
            }
            pos += len;
        }
    } else if (comp == 2 || comp == 3) { // ZIP (zlib), opcionalmente com predição por diferença
        if (n < 2 || !fmt::Inflater(p + 2, n - 2, raw, rowBytes * h + 64).Run()) { if (raw.size() < rowBytes * h) return false; }
        if (comp == 3)
            for (UINT y = 0; y < h; ++y) {
                BYTE* row = &raw[y * rowBytes];
                if (bpc == 1) for (UINT x = 1; x < w; ++x) row[x] += row[x - 1];
                else if (bpc == 2) for (UINT x = 1; x < w; ++x) { UINT v = (BE(row + x * 2, 2) + BE(row + (x - 1) * 2, 2)) & 0xFFFF; row[x * 2] = (BYTE)(v >> 8); row[x * 2 + 1] = (BYTE)v; }
            }
    } else return false;
    if (raw.size() < rowBytes * h) raw.resize(rowBytes * h, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) out[i] = raw[i * bpc]; // byte alto
    return true;
}

// Lê todas as camadas com pixels (grupos e camadas de ajuste ficam de fora).
inline bool PsdLayers(IStream* s, std::vector<Layer>& layers) {
    fmt::PsdHeader ph;
    if (!fmt::ReadPsdHeader(s, &ph) || (ph.depth != 8 && ph.depth != 16) || (ph.mode != 1 && ph.mode != 3)) return false;
    UINT64 pos = 26, len;
    for (int sec = 0; sec < 2; ++sec) { if (!fmt::ReadBE(s, pos, 4, &len)) return false; pos += 4 + len; } // modo de cor, resources
    UINT64 lmLen, liLen;
    int lb = ph.psb ? 8 : 4;
    if (!fmt::ReadBE(s, pos, lb, &lmLen) || !lmLen || !fmt::ReadBE(s, pos + lb, lb, &liLen) || !liLen || liLen > (1ull << 31)) return false;
    Bytes b((size_t)liLen);
    if (!fmt::ReadAt(s, pos + 2 * lb, b.data(), b.size())) return false;
    size_t p = 0;
    auto rd = [&](int k) { UINT64 v = p + k <= b.size() ? BE(&b[p], k) : 0; p += k; return v; };
    int count = std::abs((short)rd(2));
    struct Rec { Layer l; std::vector<std::pair<short, UINT64>> ch; bool skip; };
    std::vector<Rec> recs;
    for (int i = 0; i < count && p < b.size(); ++i) {
        Rec r{};
        r.l.top = (int)rd(4); r.l.left = (int)rd(4); r.l.bottom = (int)rd(4); r.l.right = (int)rd(4);
        int nch = (int)rd(2);
        for (int c = 0; c < nch; ++c) { short id = (short)rd(2); UINT64 cl = rd(ph.psb ? 8 : 4); r.ch.push_back({id, cl}); }
        p += 4 + 4; // "8BIM" + modo de mesclagem
        rd(1); rd(1); // opacidade, clipping
        BYTE flags = (BYTE)rd(1);
        rd(1);
        r.l.hidden = (flags & 2) != 0;
        size_t extra = (size_t)rd(4), end = p + extra;
        size_t ml = (size_t)rd(4); p += ml;                       // máscara
        size_t br = (size_t)rd(4); p += br;                       // blending ranges
        BYTE nl = p < b.size() ? b[p] : 0;                        // nome (Pascal, alinhado a 4)
        std::string ansi(p + 1 + nl <= b.size() ? (const char*)&b[p + 1] : "", p + 1 + nl <= b.size() ? nl : 0);
        p += ((1 + nl + 3) & ~3u);
        r.l.name = fmt::Widen(ansi);
        r.skip = false;
        while (p + 12 <= end) { // blocos adicionais: nome Unicode e marcadores de grupo
            std::string key((const char*)&b[p + 4], 4);
            p += 8;
            size_t bl = (size_t)rd(4);
            if (key == "luni" && bl >= 4) {
                UINT32 nc = (UINT32)BE(&b[p], 4);
                std::wstring w;
                for (UINT32 k = 0; k < nc && p + 4 + k * 2 + 1 < b.size(); ++k) { wchar_t c = (wchar_t)BE(&b[p + 4 + k * 2], 2); if (c) w += c; }
                if (!w.empty()) r.l.name = w;
            }
            if ((key == "lsct" || key == "lsdk") && bl >= 4) { UINT t = (UINT)BE(&b[p], 4); if (t >= 1 && t <= 3) r.skip = true; }
            p += (bl + 1) & ~(size_t)1;
        }
        p = end;
        recs.push_back(r);
    }
    // dados dos canais, na mesma ordem das camadas
    for (auto& r : recs) {
        UINT w = (UINT)max(0, r.l.right - r.l.left), h = (UINT)max(0, r.l.bottom - r.l.top);
        std::map<short, Bytes> planes;
        for (auto& c : r.ch) {
            if (p + c.second > b.size()) return !layers.empty();
            if (!r.skip && w && h && w <= 30000 && h <= 30000 && (c.first >= -1 && c.first <= 2)) {
                Bytes plane;
                if (LayerChannel(&b[p], (size_t)c.second, w, h, ph.depth, ph.psb, plane)) planes[c.first] = std::move(plane);
            }
            p += (size_t)c.second;
        }
        if (r.skip || !w || !h || !planes.count(0)) continue;
        BYTE* bits;
        HBITMAP bmp = fmt::CreateDib(w, h, &bits);
        if (!bmp) continue;
        bool gray = ph.mode == 1;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            BYTE R = planes[0][i], G = gray ? R : (planes.count(1) ? planes[1][i] : R), B = gray ? R : (planes.count(2) ? planes[2][i] : R);
            BYTE A = planes.count(-1) ? planes[-1][i] : 255;
            bits[i * 4] = (BYTE)(B * A / 255); bits[i * 4 + 1] = (BYTE)(G * A / 255); bits[i * 4 + 2] = (BYTE)(R * A / 255); bits[i * 4 + 3] = A;
        }
        r.l.bmp = bmp;
        layers.push_back(r.l);
    }
    return !layers.empty();
}

// ---------- paletas ----------

inline std::wstring SafeName(std::wstring n) {
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!n.empty() && (n.back() == L' ' || n.back() == L'.')) n.pop_back();
    return n.empty() ? L"sem nome" : n;
}

inline void PutBE(Bytes& b, UINT64 v, int n) { for (int i = n - 1; i >= 0; --i) b.push_back((BYTE)(v >> (i * 8))); }

inline Bytes WriteAco(const std::vector<fmt::Swatch>& sw) {
    Bytes b;
    for (int ver = 1; ver <= 2; ++ver) {
        PutBE(b, ver, 2); PutBE(b, sw.size(), 2);
        for (auto& s : sw) {
            PutBE(b, 0, 2); PutBE(b, s.r * 257, 2); PutBE(b, s.g * 257, 2); PutBE(b, s.b * 257, 2); PutBE(b, 0, 2);
            if (ver == 2) {
                PutBE(b, 0, 2); PutBE(b, s.name.size() + 1, 4);
                for (wchar_t c : s.name) PutBE(b, c, 2);
                PutBE(b, 0, 2);
            }
        }
    }
    return b;
}

inline Bytes WriteAse(const std::vector<fmt::Swatch>& sw) {
    Bytes b = {'A', 'S', 'E', 'F', 0, 1, 0, 0};
    PutBE(b, sw.size(), 4);
    for (auto& s : sw) {
        std::wstring name = s.name.empty() ? fmt::Hex(s) : s.name;
        Bytes body;
        PutBE(body, name.size() + 1, 2);
        for (wchar_t c : name) PutBE(body, c, 2);
        PutBE(body, 0, 2);
        body.insert(body.end(), {'R', 'G', 'B', ' '});
        for (BYTE v : {s.r, s.g, s.b}) { float f = v / 255.0f; UINT32 u; memcpy(&u, &f, 4); PutBE(body, u, 4); }
        PutBE(body, 2, 2); // cor normal
        PutBE(b, 1, 2); PutBE(b, body.size(), 4);
        b.insert(b.end(), body.begin(), body.end());
    }
    return b;
}

// Lista de cores em texto: HEX, RGB e variáveis CSS.
inline std::string PaletteText(const std::vector<fmt::Swatch>& sw, const std::wstring& title) {
    auto u8 = [](const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
    };
    std::string t = "\xEF\xBB\xBF/* " + u8(title) + " */\r\n\r\n";
    char line[256];
    for (auto& s : sw) {
        std::wstring hex = fmt::Hex(s);
        snprintf(line, sizeof(line), "%s  rgb(%d, %d, %d)  %s\r\n", u8(hex).c_str(), s.r, s.g, s.b, u8(s.name).c_str());
        t += line;
    }
    t += "\r\n:root {\r\n";
    for (size_t i = 0; i < sw.size(); ++i) {
        std::string var;
        for (char c : u8(sw[i].name)) var += isalnum((BYTE)c) ? (char)tolower((BYTE)c) : (var.empty() || var.back() == '-' ? '\0' : '-');
        var.erase(std::remove(var.begin(), var.end(), '\0'), var.end());
        while (!var.empty() && var.back() == '-') var.pop_back();
        if (var.empty()) var = "cor-" + std::to_string(i + 1);
        snprintf(line, sizeof(line), "  --%s: %s;\r\n", var.c_str(), u8(fmt::Hex(sw[i])).c_str());
        t += line;
    }
    t += "}\r\n";
    return t;
}

} // namespace extras
