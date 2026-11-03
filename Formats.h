// Formats.h - decodificadores compartilhados entre DesignThumbs.dll (miniaturas) e DesignPeek.exe (preview).
// Tudo em um header para manter os dois binários independentes e sem dependências externas.
#pragma once
#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <d2d1_3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Pdf.h"
#include "I18n.h"

namespace fmt {
using std::min; using std::max;
typedef std::vector<BYTE> Bytes;

// Extensões tratadas (registradas como miniatura e decodificadas direto pelo DesignPeek).
static const wchar_t* const kExtensions[] = {
    L".psd", L".psb", L".ai", L".indd", L".indt", L".eps", L".epsf", L".epsi", L".svg", L".svgz",
    L".cdr", L".afdesign", L".afphoto", L".afpub", L".xd", L".sketch", L".kra", L".ora", L".procreate",
    L".ase", L".aco", // paletas de cor
    // câmeras: usa o JPEG de preview embutido no RAW
    L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2", L".dng", L".raf", L".orf",
    L".rw2", L".rwl", L".pef", L".srw", L".3fr", L".erf", L".kdc", L".mrw", L".mef", L".iiq",
};
static const wchar_t* const kRawExtensions[] = {
    L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2", L".dng", L".raf", L".orf",
    L".rw2", L".rwl", L".pef", L".srw", L".3fr", L".erf", L".kdc", L".mrw", L".mef", L".iiq",
};

inline bool InList(const std::wstring& ext, const wchar_t* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i) if (_wcsicmp(ext.c_str(), list[i]) == 0) return true;
    return false;
}
// PDF: só painel de visualização e preview (as miniaturas de PDF ficam com o Edge/Acrobat).
// Fontes: o Windows já gera as miniaturas; o DesignThumbs faz o painel e o preview com a folha de amostra.
static const wchar_t* const kPreviewOnly[] = {L".pdf", L".ttf", L".otf", L".ttc"};
inline bool IsFontExt(const std::wstring& e) { return !_wcsicmp(e.c_str(), L".ttf") || !_wcsicmp(e.c_str(), L".otf") || !_wcsicmp(e.c_str(), L".ttc"); }
inline bool IsPaletteExt(const std::wstring& e) { return !_wcsicmp(e.c_str(), L".ase") || !_wcsicmp(e.c_str(), L".aco"); }
inline bool HasThumbnail(const std::wstring& ext) { return InList(ext, kExtensions, ARRAYSIZE(kExtensions)); }
inline bool Handles(const std::wstring& ext) { return HasThumbnail(ext) || InList(ext, kPreviewOnly, ARRAYSIZE(kPreviewOnly)); }
inline bool IsRaw(const std::wstring& ext) { return InList(ext, kRawExtensions, ARRAYSIZE(kRawExtensions)); }

// ---------- utilidades ----------

inline HBITMAP CreateDib(UINT w, UINT h, BYTE** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)bits, nullptr, 0);
}

inline void FitSize(UINT w, UINT h, UINT cx, UINT* ow, UINT* oh) {
    if (w <= cx && h <= cx) { *ow = w; *oh = h; return; }
    if (w >= h) { *ow = cx; *oh = max(1u, (UINT)((UINT64)h * cx / w)); }
    else { *oh = cx; *ow = max(1u, (UINT)((UINT64)w * cx / h)); }
}

inline UINT64 BE(const BYTE* p, int bytes) { UINT64 v = 0; for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i]; return v; }
inline UINT64 LE(const BYTE* p, int bytes) { UINT64 v = 0; for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i]; return v; }

inline bool ReadAt(IStream* s, UINT64 off, void* dst, size_t n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (FAILED(s->Seek(li, STREAM_SEEK_SET, nullptr))) return false;
    BYTE* p = (BYTE*)dst;
    while (n) {
        ULONG got = 0;
        HRESULT hr = s->Read(p, (ULONG)min(n, (size_t)1 << 24), &got);
        if (FAILED(hr) || got == 0) return false;
        p += got; n -= got;
    }
    return true;
}

inline bool ReadBE(IStream* s, UINT64 off, int bytes, UINT64* v) {
    BYTE b[8];
    if (!ReadAt(s, off, b, bytes)) return false;
    *v = BE(b, bytes);
    return true;
}

inline UINT64 StreamSize(IStream* s) {
    STATSTG st = {};
    return SUCCEEDED(s->Stat(&st, STATFLAG_NONAME)) ? st.cbSize.QuadPart : 0;
}

inline bool ReadHead(IStream* s, Bytes& b, UINT64 limit) {
    UINT64 size = StreamSize(s);
    if (size == 0 || size > (1ull << 31)) return false;
    b.resize((size_t)min(size, limit));
    return ReadAt(s, 0, b.data(), b.size());
}

inline const BYTE* Find(const BYTE* from, const BYTE* end, const char* needle, size_t n) {
    for (const BYTE* it = from; end - it >= (ptrdiff_t)n; ++it) {
        it = (const BYTE*)memchr(it, needle[0], end - it - n + 1);
        if (!it) return nullptr;
        if (memcmp(it, needle, n) == 0) return it;
    }
    return nullptr;
}

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Pixels pré-multiplicados? Some HBITMAPs vêm com alpha zerado ou não pré-multiplicado.
inline bool HasAlpha(const BYTE* p, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) if (p[i * 4 + 3] != 255) return true;
    return false;
}

// ---------- WIC ----------

// Decodifica JPEG/PNG/TIFF/BMP... para um HBITMAP 32bpp pré-multiplicado de até cx px.
// orientation: valor EXIF (1..8) aplicado após o redimensionamento.
inline HRESULT DecodeWic(const BYTE* data, size_t size, UINT cx, HBITMAP* out, bool* alpha = nullptr, int orientation = 1) {
    IWICImagingFactory* f = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) return hr;
    IStream* s = SHCreateMemStream(data, (UINT)size);
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* sc = nullptr; IWICBitmapFlipRotator* rot = nullptr; IWICFormatConverter* conv = nullptr;
    hr = s ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    UINT w = 0, h = 0, ow = 0, oh = 0;
    bool swap = orientation >= 5 && orientation <= 8;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (SUCCEEDED(hr)) {
        FitSize(swap ? h : w, swap ? w : h, cx, &ow, &oh);
        hr = f->CreateBitmapScaler(&sc);
    }
    if (SUCCEEDED(hr)) hr = sc->Initialize(frame, swap ? oh : ow, swap ? ow : oh, WICBitmapInterpolationModeFant);
    IWICBitmapSource* src = sc;
    if (SUCCEEDED(hr) && orientation > 1 && orientation <= 8) {
        static const WICBitmapTransformOptions opts[9] = {
            WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal, WICBitmapTransformRotate180,
            WICBitmapTransformFlipVertical, (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
            WICBitmapTransformRotate90, (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
            WICBitmapTransformRotate270};
        if (SUCCEEDED(f->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(sc, opts[orientation]))) src = rot;
    }
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        BYTE* bits; HBITMAP bmp = CreateDib(ow, oh, &bits);
        hr = bmp ? conv->CopyPixels(nullptr, ow * 4, ow * oh * 4, bits) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) { *out = bmp; if (alpha) *alpha = HasAlpha(bits, (size_t)ow * oh); }
        else if (bmp) DeleteObject(bmp);
    }
    if (conv) conv->Release(); if (rot) rot->Release(); if (sc) sc->Release(); if (frame) frame->Release();
    if (dec) dec->Release(); if (s) s->Release(); f->Release();
    return hr;
}

// ---------- TIFF / EXIF (orientação e câmera) ----------

struct TiffTags { int orientation = 0; std::string make, model; };

inline void ParseTiff(const BYTE* p, size_t n, TiffTags* t) {
    if (n < 8 || !((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M'))) return;
    bool le = p[0] == 'I';
    auto rd = [&](size_t o, int k) -> UINT { return o + k <= n ? (UINT)(le ? LE(p + o, k) : BE(p + o, k)) : 0; };
    UINT ifd = rd(4, 4), cnt = rd(ifd, 2);
    for (UINT i = 0; i < cnt && i < 512; ++i) {
        size_t e = ifd + 2 + (size_t)i * 12;
        UINT tag = rd(e, 2), type = rd(e + 2, 2), count = rd(e + 4, 4);
        if (tag == 0x112) t->orientation = (int)rd(e + 8, 2);
        if ((tag == 0x10F || tag == 0x110) && type == 2 && count < 256) {
            size_t off = count <= 4 ? e + 8 : rd(e + 8, 4);
            if (off + count <= n) {
                std::string v((const char*)p + off, count);
                v.erase(std::find(v.begin(), v.end(), '\0'), v.end());
                while (!v.empty() && v.back() == ' ') v.pop_back();
                (tag == 0x10F ? t->make : t->model) = v;
            }
        }
    }
}

// ---------- JPEG embutido (RAW de câmera) ----------

struct JpegSpan { size_t off = 0, len = 0; UINT w = 0, h = 0; TiffTags exif; };

// Valida um JPEG baseline/progressivo começando em p (FFD8) e calcula seu tamanho.
inline bool ParseJpeg(const BYTE* p, size_t n, JpegSpan* js) {
    size_t i = 2;
    bool sofOk = false;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) return false;
        BYTE m = p[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m == 0xD9) { js->len = i + 2; return sofOk; }
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        size_t L = (size_t)BE(p + i + 2, 2);
        if (L < 2 || i + 2 + L > n) return false;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (m != 0xC0 && m != 0xC1 && m != 0xC2) return false; // lossless (dados RAW): WIC não decodifica
            if (L >= 7) { js->h = (UINT)BE(p + i + 5, 2); js->w = (UINT)BE(p + i + 7, 2); sofOk = js->w && js->h; }
        }
        if (m == 0xE1 && L > 8 && memcmp(p + i + 4, "Exif\0\0", 6) == 0) ParseTiff(p + i + 10, L - 8, &js->exif);
        i += 2 + L;
        if (m == 0xDA) { // dados comprimidos até o próximo marcador
            for (;;) {
                const BYTE* ff = (const BYTE*)memchr(p + i, 0xFF, n - i);
                if (!ff || ff + 1 >= p + n) return false;
                i = ff - p;
                BYTE nx = p[i + 1];
                if (nx != 0x00 && !(nx >= 0xD0 && nx <= 0xD7)) break;
                i += 2;
            }
        }
    }
    return false;
}

inline bool LargestJpeg(const Bytes& b, JpegSpan* best) {
    const BYTE* p = b.data();
    size_t n = b.size(), pos = 0;
    bool found = false;
    while (pos + 4 < n) {
        const BYTE* ff = (const BYTE*)memchr(p + pos, 0xFF, n - pos - 3);
        if (!ff) break;
        pos = ff - p;
        if (p[pos + 1] == 0xD8 && p[pos + 2] == 0xFF) {
            JpegSpan js;
            if (ParseJpeg(p + pos, n - pos, &js)) {
                js.off = pos;
                if (js.w >= 160 && (UINT64)js.w * js.h > (UINT64)best->w * best->h) { *best = js; found = true; }
                pos += js.len;
                continue;
            }
        }
        ++pos;
    }
    return found;
}

inline void RawTags(const Bytes& b, TiffTags* t) {
    if (b.size() > 8 && ((b[0] == 'I' && b[1] == 'I') || (b[0] == 'M' && b[1] == 'M'))) ParseTiff(b.data(), b.size(), t);
    else if (const BYTE* c = Find(b.data(), b.data() + min(b.size(), (size_t)1 << 20), "CMT1", 4)) // CR3
        ParseTiff(c + 4, b.data() + b.size() - (c + 4), t);
}

inline bool RawMagic(const BYTE* m) {
    return (m[0] == 'I' && m[1] == 'I') || (m[0] == 'M' && m[1] == 'M') || memcmp(m + 4, "ftypcrx", 7) == 0 ||
           memcmp(m, "FUJIFILM", 8) == 0;
}

// Lê o começo do arquivo (onde quase sempre fica o preview); se não achar, lê tudo.
inline bool RawPreview(IStream* s, Bytes& b, JpegSpan* js, TiffTags* tags) {
    if (!ReadHead(s, b, 24u << 20)) return false;
    bool ok = LargestJpeg(b, js);
    if ((!ok || js->w < 1000) && StreamSize(s) > b.size()) {
        *js = JpegSpan();
        if (!ReadHead(s, b, 1ull << 31)) return false;
        ok = LargestJpeg(b, js);
    }
    if (ok) RawTags(b, tags);
    return ok;
}

// ---------- inflate (ZIP / SVGZ) ----------

class Inflater {
    const BYTE* in_; size_t len_, pos_ = 0;
    UINT bitbuf_ = 0; int bitcnt_ = 0;
    bool err_ = false;
    Bytes& out_; size_t max_;
    struct Huff { short count[16]; short symbol[288]; };

    int Bits(int need) {
        UINT val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= len_) { err_ = true; return 0; }
            val |= (UINT)in_[pos_++] << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return (int)(val & ((1u << need) - 1));
    }
    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= Bits(1);
            if (err_) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        err_ = true;
        return -1;
    }
    static int Construct(Huff& h, const short* length, int n) {
        for (int len = 0; len <= 15; ++len) h.count[len] = 0;
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) { left <<= 1; left -= h.count[len]; if (left < 0) return left; }
        short offs[16]; offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; ++s) if (length[s]) h.symbol[offs[length[s]]++] = (short)s;
        return left;
    }
    bool Codes(const Huff& lc, const Huff& dc) {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lc);
            if (err_ || sym < 0) return false;
            if (sym < 256) out_.push_back((BYTE)sym);
            else if (sym == 256) return true;
            else {
                sym -= 257;
                if (sym >= 29) return false;
                int len = lbase[sym] + Bits(lext[sym]);
                int ds = Decode(dc);
                if (err_ || ds < 0 || ds >= 30) return false;
                size_t dist = (size_t)(dbase[ds] + Bits(dext[ds]));
                if (err_ || dist > out_.size()) return false;
                size_t from = out_.size() - dist;
                for (int i = 0; i < len; ++i) out_.push_back(out_[from + i]);
            }
            if (out_.size() > max_) return false;
        }
    }
    bool Stored() {
        bitbuf_ = 0; bitcnt_ = 0;
        if (pos_ + 4 > len_) return false;
        size_t len = in_[pos_] | (in_[pos_ + 1] << 8);
        pos_ += 4;
        if (pos_ + len > len_) return false;
        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
        pos_ += len;
        return true;
    }
    bool Fixed() {
        Huff lc, dc;
        short l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        Construct(lc, l, 288);
        for (s = 0; s < 30; ++s) l[s] = 5;
        Construct(dc, l, 30);
        return Codes(lc, dc);
    }
    bool Dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (err_ || nlen > 286 || ndist > 30) return false;
        int i = 0;
        for (; i < ncode; ++i) lengths[order[i]] = (short)Bits(3);
        for (; i < 19; ++i) lengths[order[i]] = 0;
        Huff lc, dc;
        if (Construct(lc, lengths, 19) != 0) return false;
        i = 0;
        while (i < nlen + ndist) {
            int sym = Decode(lc);
            if (err_ || sym < 0) return false;
            if (sym < 16) { lengths[i++] = (short)sym; continue; }
            short len = 0; int rep;
            if (sym == 16) { if (i == 0) return false; len = lengths[i - 1]; rep = 3 + Bits(2); }
            else if (sym == 17) rep = 3 + Bits(3);
            else rep = 11 + Bits(7);
            if (err_ || i + rep > nlen + ndist) return false;
            while (rep--) lengths[i++] = len;
        }
        if (lengths[256] == 0) return false;
        int e = Construct(lc, lengths, nlen);
        if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return false;
        e = Construct(dc, lengths + nlen, ndist);
        if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return false;
        return Codes(lc, dc);
    }
public:
    Inflater(const BYTE* in, size_t len, Bytes& out, size_t maxOut) : in_(in), len_(len), out_(out), max_(maxOut) {}
    bool Run() {
        int last;
        do {
            last = Bits(1);
            int type = Bits(2);
            if (err_) return false;
            bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

inline bool Gunzip(const Bytes& in, Bytes& out) {
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
    BYTE flags = in[3];
    size_t p = 10;
    if (flags & 4) p += 2 + (size_t)LE(&in[p], 2);
    if (flags & 8) while (p < in.size() && in[p++]) {}
    if (flags & 16) while (p < in.size() && in[p++]) {}
    if (flags & 2) p += 2;
    if (p >= in.size()) return false;
    out.reserve((size_t)LE(&in[in.size() - 4], 4));
    return Inflater(&in[p], in.size() - p, out, 256u << 20).Run();
}

// ---------- ZIP: CDR, Affinity, XD, Sketch, Krita, OpenRaster, Procreate ----------
// Todos guardam uma imagem de preview dentro do pacote; lê só o diretório e essa entrada.

inline bool ZipPreview(IStream* s, Bytes& img) {
    UINT64 size = StreamSize(s);
    if (size < 22) return false;
    size_t tailLen = (size_t)min<UINT64>(size, 66000);
    Bytes tail(tailLen);
    if (!ReadAt(s, size - tailLen, tail.data(), tailLen)) return false;
    size_t eocd = SIZE_MAX;
    for (size_t i = tailLen - 22 + 1; i-- > 0;)
        if (memcmp(&tail[i], "PK\5\6", 4) == 0) { eocd = i; break; }
    if (eocd == SIZE_MAX) return false;
    UINT count = (UINT)LE(&tail[eocd + 10], 2);
    UINT64 cdSize = LE(&tail[eocd + 12], 4), cdOff = LE(&tail[eocd + 16], 4);
    if (cdSize > (64u << 20) || cdOff + cdSize > size) return false;
    Bytes cd((size_t)cdSize);
    if (!ReadAt(s, cdOff, cd.data(), cd.size())) return false;

    struct { UINT64 off = 0, comp = 0, raw = 0; UINT method = 0; int score = -1; } best;
    size_t p = 0;
    for (UINT i = 0; i < count && p + 46 <= cd.size(); ++i) {
        if (memcmp(&cd[p], "PK\1\2", 4) != 0) break;
        UINT method = (UINT)LE(&cd[p + 10], 2);
        UINT64 comp = LE(&cd[p + 20], 4), raw = LE(&cd[p + 24], 4);
        size_t nameLen = (size_t)LE(&cd[p + 28], 2), extra = (size_t)LE(&cd[p + 30], 2), comment = (size_t)LE(&cd[p + 32], 2);
        UINT64 local = LE(&cd[p + 42], 4);
        if (p + 46 + nameLen > cd.size()) break;
        std::string name((const char*)&cd[p + 46], nameLen);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        p += 46 + nameLen + extra + comment;
        auto ends = [&](const char* e) { size_t n = strlen(e); return name.size() >= n && name.compare(name.size() - n, n, e) == 0; };
        if (!(ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".bmp"))) continue;
        int score = name.find("mergedimage") != std::string::npos ? 3
                  : name.find("preview") != std::string::npos ? 2
                  : (name.find("thumb") != std::string::npos || name.find("quicklook") != std::string::npos) ? 1 : -1;
        if (score < 0 || (method != 0 && method != 8) || raw > (256u << 20)) continue;
        if (score > best.score || (score == best.score && raw > best.raw)) { best.off = local; best.comp = comp; best.raw = raw; best.method = method; best.score = score; }
    }
    if (best.score < 0) return false;
    BYTE lh[30];
    if (!ReadAt(s, best.off, lh, 30) || memcmp(lh, "PK\3\4", 4) != 0) return false;
    UINT64 data = best.off + 30 + LE(lh + 26, 2) + LE(lh + 28, 2);
    Bytes comp((size_t)best.comp);
    if (!ReadAt(s, data, comp.data(), comp.size())) return false;
    if (best.method == 0) { img.swap(comp); return true; }
    img.reserve((size_t)best.raw);
    return Inflater(comp.data(), comp.size(), img, (size_t)best.raw + 1024).Run();
}

// ---------- XMP: AI / INDD / EPS ----------

inline int B64(BYTE c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

struct XmpImage { size_t pos; Bytes jpg; bool complete; };

inline std::vector<XmpImage> XmpImages(const Bytes& buf) {
    std::vector<XmpImage> list;
    const BYTE* p = buf.data();
    const BYTE* end = p + buf.size();
    static const char tag[] = "xmpGImg:image";
    for (const BYTE* it = p; (it = Find(it, end, tag, sizeof(tag) - 1)) != nullptr;) {
        size_t pos = it - p;
        it += sizeof(tag) - 1;
        if (it < end && *it == '>') ++it;                          // <xmpGImg:image>...</xmpGImg:image>
        else if (it + 1 < end && *it == '=') it += 2;              // xmpGImg:image="..."
        else continue;
        XmpImage img{pos, {}, false};
        unsigned acc = 0; int bits = 0;
        for (; it < end && *it != '<' && *it != '"'; ++it) {
            if (*it == '&') { while (it < end && *it != ';') ++it; continue; } // &#xA;
            int v = B64(*it);
            if (v < 0) continue;
            acc = (acc << 6) | v; bits += 6;
            if (bits >= 8) { bits -= 8; img.jpg.push_back((BYTE)(acc >> bits)); }
        }
        Bytes& j = img.jpg;
        while (!j.empty() && j.back() == 0) j.pop_back();
        img.complete = j.size() > 4 && j[0] == 0xFF && j[1] == 0xD8 && j[j.size() - 2] == 0xFF && j.back() == 0xD9;
        if (!j.empty()) list.push_back(std::move(img));
    }
    return list;
}

// Miniaturas por página (INDD salva uma por página em xmp:PageInfo) ou a melhor miniatura única.
inline std::vector<Bytes> XmpPages(const Bytes& buf) {
    std::vector<XmpImage> imgs = XmpImages(buf);
    std::vector<Bytes> pages;
    // Arquivos salvos várias vezes guardam cópias antigas: usa o último bloco PageInfo completo.
    const BYTE* p = buf.data();
    const BYTE* end = p + buf.size();
    std::vector<std::pair<size_t, size_t>> blocks;
    for (const BYTE* it = p; (it = Find(it, end, "<xmp:PageInfo", 13)) != nullptr; ++it) {
        const BYTE* close = Find(it, end, "</xmp:PageInfo>", 15);
        if (!close) break;
        blocks.push_back({(size_t)(it - p), (size_t)(close - p)});
        it = close;
    }
    for (size_t b = blocks.size(); b-- > 0 && pages.empty();) {
        bool allOk = true;
        std::vector<Bytes> cand;
        for (auto& im : imgs)
            if (im.pos > blocks[b].first && im.pos < blocks[b].second) { allOk &= im.complete; cand.push_back(im.jpg); }
        if (allOk && !cand.empty()) pages.swap(cand);
    }
    if (pages.empty()) {
        const XmpImage* best = nullptr;
        for (auto& im : imgs)
            if (!best || im.complete > best->complete || (im.complete == best->complete && im.jpg.size() > best->jpg.size())) best = &im;
        if (best) pages.push_back(best->jpg);
    }
    return pages;
}

// Todos os valores de uma propriedade XMP, em forma de elemento ou de atributo.
inline std::vector<std::string> XmpValues(const Bytes& buf, const char* name) {
    std::vector<std::string> out;
    const BYTE* p = buf.data();
    const BYTE* end = p + buf.size();
    size_t n = strlen(name);
    for (const BYTE* it = p; (it = Find(it, end, name, n)) != nullptr;) {
        bool startOk = it > p && (it[-1] == '<' || it[-1] == ' ' || it[-1] == '\t' || it[-1] == '\n' || it[-1] == '\r');
        it += n;
        if (!startOk || it >= end) continue;
        char close;
        if (*it == '>') { close = '<'; ++it; }
        else if (*it == '=' && it + 1 < end && (it[1] == '"' || it[1] == '\'')) { close = (char)it[1]; it += 2; }
        else continue;
        const BYTE* e = it;
        while (e < end && *e != close && e - it < 1024) ++e;
        std::string v((const char*)it, e - it);
        for (auto ent : {std::make_pair("&amp;", "&"), std::make_pair("&lt;", "<"), std::make_pair("&gt;", ">"), std::make_pair("&quot;", "\""), std::make_pair("&apos;", "'")})
            for (size_t k; (k = v.find(ent.first)) != std::string::npos;) v.replace(k, strlen(ent.first), ent.second);
        if (!v.empty() && v.find('<') == std::string::npos) out.push_back(v);
        it = e;
    }
    return out;
}

// ---------- PSD / PSB ----------
// Lido direto do stream com Seek: só lê os trechos necessários, mesmo em arquivos
// de centenas de MB ou em unidades na nuvem (Google Drive, OneDrive).

struct PsdHeader { bool psb; UINT channels, h, w, depth, mode; };

inline bool ReadPsdHeader(IStream* s, PsdHeader* ph) {
    BYTE hd[26];
    if (!ReadAt(s, 0, hd, 26) || memcmp(hd, "8BPS", 4) != 0) return false;
    ph->psb = BE(hd + 4, 2) == 2;
    ph->channels = (UINT)BE(hd + 12, 2); ph->h = (UINT)BE(hd + 14, 4); ph->w = (UINT)BE(hd + 18, 4);
    ph->depth = (UINT)BE(hd + 22, 2); ph->mode = (UINT)BE(hd + 24, 2);
    return true;
}

// Usa a imagem composta (alta qualidade). Lê apenas as linhas necessárias.
inline HRESULT PsdComposite(IStream* s, UINT cx, HBITMAP* out) {
    PsdHeader ph;
    if (!ReadPsdHeader(s, &ph)) return E_FAIL;
    UINT channels = ph.channels, h = ph.h, w = ph.w, depth = ph.depth, mode = ph.mode;
    if ((depth != 8 && depth != 16 && depth != 32) || w == 0 || h == 0) return E_FAIL;
    if (mode != 1 && mode != 3 && mode != 4) return E_FAIL; // Gray, RGB, CMYK
    UINT64 pos = 26, len;
    for (int section = 0; section < 3; ++section) {          // color mode, resources, layers
        int lenBytes = (section == 2 && ph.psb) ? 8 : 4;
        if (!ReadBE(s, pos, lenBytes, &len)) return E_FAIL;
        pos += lenBytes + len;
    }
    UINT64 comp;
    if (!ReadBE(s, pos, 2, &comp) || comp > 1) return E_FAIL;
    pos += 2;

    UINT need = mode == 3 ? 3 : mode == 4 ? 4 : 1;
    if (channels < need) return E_FAIL;
    bool alpha = channels > need;
    UINT use = alpha ? need + 1 : need;
    UINT bpc = depth / 8;
    size_t rowBytes = (size_t)w * bpc;
    size_t usedRows = (size_t)use * h;

    // offset de cada linha de cada canal
    std::vector<UINT64> rowOff(usedRows);
    std::vector<UINT> rowLen(usedRows);
    if (comp == 0) {
        for (size_t i = 0; i < usedRows; ++i) { rowOff[i] = pos + (UINT64)i * rowBytes; rowLen[i] = (UINT)rowBytes; }
    } else {
        int cb = ph.psb ? 4 : 2;
        UINT64 totalRows = (UINT64)channels * h;
        Bytes counts(usedRows * cb);
        if (!ReadAt(s, pos, counts.data(), counts.size())) return E_FAIL;
        UINT64 data = pos + totalRows * cb;
        for (size_t i = 0; i < usedRows; ++i) {
            rowOff[i] = data;
            rowLen[i] = (UINT)BE(&counts[i * cb], cb);
            data += rowLen[i];
        }
    }

    UINT ow, oh;
    FitSize(w, h, cx, &ow, &oh);
    BYTE* bits; HBITMAP bmp = CreateDib(ow, oh, &bits);
    if (!bmp) return E_OUTOFMEMORY;
    Bytes row(rowBytes), src;
    Bytes plane((size_t)use * ow);

    for (UINT y = 0; y < oh; ++y) {
        UINT sy = (UINT)((UINT64)y * h / oh);
        for (UINT c = 0; c < use; ++c) {
            size_t idx = (size_t)c * h + sy;
            size_t n = rowLen[idx];
            src.resize(n);
            if (n && !ReadAt(s, rowOff[idx], src.data(), n)) { DeleteObject(bmp); return E_FAIL; }
            const BYTE* rowData = src.data();
            if (comp == 1) { // PackBits
                size_t i = 0, o = 0;
                memset(row.data(), 0, rowBytes);
                while (i < n && o < rowBytes) {
                    signed char k = (signed char)src[i++];
                    if (k >= 0) { size_t cnt = min((size_t)k + 1, min(rowBytes - o, n - i)); memcpy(&row[o], &src[i], cnt); i += k + 1; o += cnt; }
                    else if (k != -128) { if (i >= n) break; size_t cnt = min((size_t)(1 - k), rowBytes - o); memset(&row[o], src[i++], cnt); o += cnt; }
                }
                rowData = row.data();
            } else if (n < rowBytes) { DeleteObject(bmp); return E_FAIL; }
            for (UINT x = 0; x < ow; ++x) {
                size_t sx = (size_t)((UINT64)x * w / ow);
                BYTE v;
                if (bpc == 4) { // 32 bits: float linear
                    UINT32 u = (UINT32)BE(rowData + sx * 4, 4);
                    float f; memcpy(&f, &u, 4);
                    f = f < 0 ? 0 : f > 1 ? 1 : f;
                    v = (BYTE)(powf(f, 1 / 2.2f) * 255 + 0.5f);
                } else v = rowData[sx * bpc]; // byte alto basta para miniatura
                plane[(size_t)c * ow + x] = v;
            }
        }
        BYTE* d = bits + (size_t)y * ow * 4;
        for (UINT x = 0; x < ow; ++x, d += 4) {
            BYTE R, G, B;
            if (mode == 3) { R = plane[x]; G = plane[ow + x]; B = plane[2 * ow + x]; }
            else if (mode == 1) { R = G = B = plane[x]; }
            else { // CMYK do PSD é armazenado invertido (255 = sem tinta)
                UINT C = plane[x], M = plane[ow + x], Y = plane[2 * ow + x], K = plane[3 * ow + x];
                R = (BYTE)(C * K / 255); G = (BYTE)(M * K / 255); B = (BYTE)(Y * K / 255);
            }
            BYTE A = alpha ? plane[need * ow + x] : 255;
            // compõe sobre branco
            d[0] = (BYTE)((B * A + 255 * (255 - A)) / 255);
            d[1] = (BYTE)((G * A + 255 * (255 - A)) / 255);
            d[2] = (BYTE)((R * A + 255 * (255 - A)) / 255);
            d[3] = 255;
        }
    }
    *out = bmp;
    return S_OK;
}

// Bloco de image resources do PSD (pequeno: miniatura, resolução, XMP).
inline bool PsdResources(IStream* s, Bytes& res) {
    UINT64 cm, rl;
    if (!ReadBE(s, 26, 4, &cm) || !ReadBE(s, 30 + cm, 4, &rl) || rl > (64u << 20)) return false;
    res.resize((size_t)rl);
    return ReadAt(s, 34 + cm, res.data(), res.size());
}

inline const BYTE* PsdResource(const Bytes& res, UINT wanted, size_t* outLen) {
    size_t pos = 0, end = res.size();
    while (pos + 12 <= end) {
        if (memcmp(&res[pos], "8BIM", 4) != 0) return nullptr;
        UINT id = (UINT)BE(&res[pos + 4], 2);
        pos += 6;
        pos += (res[pos] + 2) & ~1u;
        if (pos + 4 > end) return nullptr;
        size_t len = (size_t)BE(&res[pos], 4);
        pos += 4;
        if (pos + len > end) return nullptr;
        if (id == wanted) { *outLen = len; return &res[pos]; }
        pos += (len + 1) & ~(size_t)1;
    }
    return nullptr;
}

// Fallback: miniatura JPEG embutida (resource 1036/1033).
inline HRESULT PsdEmbeddedThumb(IStream* s, UINT cx, HBITMAP* out) {
    Bytes res;
    if (!PsdResources(s, res)) return E_FAIL;
    size_t len;
    const BYTE* p = PsdResource(res, 1036, &len);
    if (!p) p = PsdResource(res, 1033, &len);
    return p && len > 28 ? DecodeWic(p + 28, len - 28, cx, out) : E_FAIL;
}

// ---------- EPS ----------

// Preview WMF do cabeçalho binário de EPS (DOS EPS).
inline HRESULT RenderWmf(const BYTE* p, size_t n, UINT cx, HBITMAP* out) {
    if (n > 22 && LE(p, 4) == 0x9AC6CDD7) { p += 22; n -= 22; } // cabeçalho "placeable"
    HENHMETAFILE emf = SetWinMetaFileBits((UINT)n, p, nullptr, nullptr);
    if (!emf) return E_FAIL;
    ENHMETAHEADER hdr;
    GetEnhMetaFileHeader(emf, sizeof(hdr), &hdr);
    UINT w = max(1L, hdr.rclBounds.right - hdr.rclBounds.left), h = max(1L, hdr.rclBounds.bottom - hdr.rclBounds.top), ow, oh;
    FitSize(w, h, cx, &ow, &oh);
    if (ow < cx / 2 && oh < cx / 2) { double k = (double)cx / max(w, h); ow = (UINT)(w * k); oh = (UINT)(h * k); }
    BYTE* bits; HBITMAP bmp = CreateDib(ow, oh, &bits);
    HRESULT hr = E_OUTOFMEMORY;
    if (bmp) {
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 255, (size_t)ow * oh * 4);
        RECT rc = {0, 0, (LONG)ow, (LONG)oh};
        PlayEnhMetaFile(dc, emf, &rc);
        GdiFlush();
        for (size_t i = 0; i < (size_t)ow * oh; ++i) bits[i * 4 + 3] = 255;
        SelectObject(dc, old);
        DeleteDC(dc);
        *out = bmp;
        hr = S_OK;
    }
    DeleteEnhMetaFile(emf);
    return hr;
}

// Preview EPSI: bitmap em hexadecimal entre %%BeginPreview e %%EndPreview.
inline HRESULT EpsiPreview(const Bytes& b, UINT cx, HBITMAP* out) {
    const BYTE* end = b.data() + b.size();
    const BYTE* p = Find(b.data(), end, "%%BeginPreview:", 15);
    if (!p) return E_FAIL;
    int w = 0, h = 0, depth = 0;
    char line[128] = {};
    memcpy(line, p + 15, min<size_t>(127, end - p - 15));
    if (sscanf(line, "%d %d %d", &w, &h, &depth) != 3 || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (depth != 1 && depth != 2 && depth != 4 && depth != 8))
        return E_FAIL;
    while (p < end && *p != '\n' && *p != '\r') ++p;
    const BYTE* stop = Find(p, end, "%%EndPreview", 12);
    if (!stop) stop = end;
    Bytes data;
    int hi = -1;
    for (; p < stop; ++p) {
        int c = *p, v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v; else { data.push_back((BYTE)(hi << 4 | v)); hi = -1; }
    }
    size_t rowBytes = ((size_t)w * depth + 7) / 8;
    if (data.size() < rowBytes * h) return E_FAIL;
    UINT ow, oh;
    FitSize(w, h, cx, &ow, &oh);
    BYTE* bits; HBITMAP bmp = CreateDib(ow, oh, &bits);
    if (!bmp) return E_OUTOFMEMORY;
    int maxv = (1 << depth) - 1;
    for (UINT y = 0; y < oh; ++y) {
        const BYTE* row = &data[(size_t)(y * h / oh) * rowBytes];
        for (UINT x = 0; x < ow; ++x) {
            size_t bit = (size_t)(x * w / ow) * depth;
            int v = (row[bit / 8] >> (8 - depth - bit % 8)) & maxv;
            BYTE g = (BYTE)(v * 255 / maxv);
            BYTE* d = bits + ((size_t)y * ow + x) * 4;
            d[0] = d[1] = d[2] = g; d[3] = 255;
        }
    }
    *out = bmp;
    return S_OK;
}

// ---------- SVG via Direct2D (Windows 10 1703+) ----------

// Tamanho declarado no <svg> (viewBox ou width/height), para manter a proporção.
inline bool SvgSize(const Bytes& b, float* w, float* h) {
    const BYTE* end = b.data() + b.size();
    const BYTE* p = Find(b.data(), end, "<svg", 4);
    if (!p) return false;
    const BYTE* close = (const BYTE*)memchr(p, '>', end - p);
    if (!close) return false;
    std::string tag((const char*)p, close - p);
    auto attr = [&](const char* name) -> std::string {
        for (size_t k = 0; (k = tag.find(name, k)) != std::string::npos; ++k) {
            if (k == 0 || !isspace((unsigned char)tag[k - 1])) continue;
            size_t q = tag.find_first_of("\"'", k);
            if (q == std::string::npos || tag.find('=', k) > q) continue;
            size_t e = tag.find(tag[q], q + 1);
            if (e == std::string::npos) return "";
            return tag.substr(q + 1, e - q - 1);
        }
        return "";
    };
    float x, y, vw, vh;
    std::string vb = attr("viewBox");
    for (char& c : vb) if (c == ',') c = ' ';
    if (sscanf(vb.c_str(), "%f %f %f %f", &x, &y, &vw, &vh) == 4 && vw > 0 && vh > 0) { *w = vw; *h = vh; return true; }
    std::string sw = attr("width"), sh = attr("height");
    if (sw.find('%') == std::string::npos && sh.find('%') == std::string::npos &&
        sscanf(sw.c_str(), "%f", w) == 1 && sscanf(sh.c_str(), "%f", h) == 1 && *w > 0 && *h > 0) return true;
    return false;
}

inline HRESULT SvgRender(const Bytes& buf, UINT cx, HBITMAP* out) {
    float sw = 1, sh = 1;
    if (!SvgSize(buf, &sw, &sh)) sw = sh = 1;
    UINT W = sw >= sh ? cx : max(1u, (UINT)(cx * sw / sh)), H = sw >= sh ? max(1u, (UINT)(cx * sh / sw)) : cx;
    IWICImagingFactory* wic = nullptr; ID2D1Factory* d2d = nullptr; IWICBitmap* wbmp = nullptr;
    ID2D1RenderTarget* rt = nullptr; ID2D1DeviceContext5* dc = nullptr;
    ID2D1SvgDocument* doc = nullptr; ID2D1SvgElement* root = nullptr; IStream* s = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    if (SUCCEEDED(hr)) hr = wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wbmp);
    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        hr = d2d->CreateWicBitmapRenderTarget(wbmp, p, &rt);
    }
    if (SUCCEEDED(hr)) hr = rt->QueryInterface(IID_PPV_ARGS(&dc));
    if (SUCCEEDED(hr)) { s = SHCreateMemStream(buf.data(), (UINT)buf.size()); hr = s ? S_OK : E_OUTOFMEMORY; }
    if (SUCCEEDED(hr)) hr = dc->CreateSvgDocument(s, D2D1::SizeF((float)W, (float)H), &doc);
    if (SUCCEEDED(hr)) {
        doc->GetRoot(&root);
        // Sem viewBox, cria um a partir de width/height para escalar ao tamanho pedido.
        if (root && !root->IsAttributeSpecified(L"viewBox")) {
            float w = 0, h = 0;
            root->GetAttributeValue(L"width", &w); root->GetAttributeValue(L"height", &h);
            if (w > 0 && h > 0) {
                D2D1_SVG_VIEWBOX vb = {0, 0, w, h};
                root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb));
            }
        }
        if (root) { root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, L"100%"); root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, L"100%"); }
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawSvgDocument(doc);
        hr = dc->EndDraw();
    }
    if (SUCCEEDED(hr)) {
        BYTE* bits; HBITMAP bmp = CreateDib(W, H, &bits);
        hr = bmp ? wbmp->CopyPixels(nullptr, W * 4, W * H * 4, bits) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) *out = bmp; else if (bmp) DeleteObject(bmp);
    }
    if (root) root->Release(); if (doc) doc->Release(); if (s) s->Release(); if (dc) dc->Release();
    if (rt) rt->Release(); if (wbmp) wbmp->Release(); if (d2d) d2d->Release(); if (wic) wic->Release();
    return hr;
}

inline bool LooksLikeSvg(const Bytes& b) {
    return Find(b.data(), b.data() + min(b.size(), (size_t)64 << 10), "<svg", 4) != nullptr;
}

// ---------- PDF / AI (leitor nativo do Windows) ----------

inline bool IsPdfLike(const BYTE* magic, const std::wstring& ext) {
    return memcmp(magic, "%PDF", 4) == 0 && (_wcsicmp(ext.c_str(), L".pdf") == 0 || _wcsicmp(ext.c_str(), L".ai") == 0);
}

inline HRESULT PdfRender(IStream* s, UINT cx, int page, HBITMAP* out, int* pages) {
    pdfrt::Document doc;
    if (!doc.Open(s)) return E_FAIL;
    int n = (int)doc.Pages();
    if (n <= 0) return E_FAIL;
    if (pages) *pages = n;
    page = max(0, min(page, n - 1));
    IStream* png = doc.RenderPng((UINT32)page, cx);
    if (!png) return E_FAIL;
    Bytes b;
    HRESULT hr = ReadHead(png, b, 256u << 20) ? DecodeWic(b.data(), b.size(), cx, out) : E_FAIL;
    png->Release();
    return hr;
}

} // namespace fmt
#include "Extra.h"
namespace fmt {

inline bool ReadPalette(IStream* s, const std::wstring& ext, std::vector<Swatch>& sw) {
    Bytes b;
    if (!ReadHead(s, b, 16u << 20)) return false;
    return !_wcsicmp(ext.c_str(), L".ase") ? ParseAse(b, sw) : ParseAco(b, sw);
}

// ---------- entrada única ----------

// Gera a imagem de um arquivo. ext: extensão com ponto (pode ser vazia); cx: lado máximo;
// page: página (INDD com várias páginas); pages recebe o total de páginas.
// hq: preview/painel/conversão (PDF e .ai renderizados página a página); sem hq: miniatura rápida.
inline HRESULT Render(IStream* s, const std::wstring& ext, UINT cx, int page, HBITMAP* out, bool* alpha, int* pages = nullptr, bool hq = false) {
    *alpha = false;
    if (pages) *pages = 1;
    BYTE magic[16] = {};
    ReadAt(s, 0, magic, sizeof(magic));
    if (IsPdfLike(magic, ext) && (hq || _wcsicmp(ext.c_str(), L".pdf") == 0) && SUCCEEDED(PdfRender(s, cx, page, out, pages))) return S_OK;
    if (IsPaletteExt(ext)) {
        std::vector<Swatch> sw;
        return ReadPalette(s, ext, sw) ? RenderPalette(sw, cx, out) : E_FAIL;
    }
    if (IsFontExt(ext) || (ext.empty() && IsFontMagic(magic))) {
        Bytes b;
        return ReadHead(s, b, 64u << 20) ? RenderFont(b, cx, out) : E_FAIL;
    }
    if (memcmp(magic, "8BPS", 4) == 0) {
        HRESULT hr = PsdComposite(s, cx, out);
        return SUCCEEDED(hr) ? hr : PsdEmbeddedThumb(s, cx, out);
    }
    if (IsRaw(ext) || (ext.empty() && RawMagic(magic))) {
        Bytes b; JpegSpan js; TiffTags t;
        if (!RawPreview(s, b, &js, &t)) return E_FAIL;
        int o = t.orientation ? t.orientation : js.exif.orientation;
        HRESULT hr = DecodeWic(b.data() + js.off, js.len, cx, out, nullptr, o);
        return FAILED(hr) && o > 1 ? DecodeWic(b.data() + js.off, js.len, cx, out) : hr; // sem rotação se falhar
    }
    if (memcmp(magic, "PK\3\4", 4) == 0) {
        Bytes img;
        return ZipPreview(s, img) ? DecodeWic(img.data(), img.size(), cx, out, alpha) : E_FAIL;
    }

    Bytes b;
    if (!ReadHead(s, b, 512u << 20) || b.size() < 8) return E_FAIL;
    if (LE(b.data(), 4) == 0xC6D3D0C5 && b.size() >= 30) { // EPS com cabeçalho binário
        UINT64 wOff = LE(&b[12], 4), wLen = LE(&b[16], 4), tOff = LE(&b[20], 4), tLen = LE(&b[24], 4);
        if (tLen && tOff + tLen <= b.size() && SUCCEEDED(DecodeWic(&b[(size_t)tOff], (size_t)tLen, cx, out))) return S_OK;
        if (wLen && wOff + wLen <= b.size() && SUCCEEDED(RenderWmf(&b[(size_t)wOff], (size_t)wLen, cx, out))) return S_OK;
    }
    if (b[0] == 0x1F && b[1] == 0x8B) { // SVGZ
        Bytes svg;
        if (!Gunzip(b, svg)) return E_FAIL;
        b.swap(svg);
    }
    std::vector<Bytes> list = XmpPages(b);
    if (!list.empty()) {
        if (pages) *pages = (int)list.size();
        page = max(0, min(page, (int)list.size() - 1));
        if (SUCCEEDED(DecodeWic(list[page].data(), list[page].size(), cx, out))) return S_OK;
    }
    if (SUCCEEDED(EpsiPreview(b, cx, out))) return S_OK;
    if (LooksLikeSvg(b) && SUCCEEDED(SvgRender(b, cx, out))) { *alpha = true; return S_OK; }
    return E_FAIL;
}

// ---------- informações para o DesignPeek ----------

struct Info {
    UINT w = 0, h = 0;          // pixels nativos (0 = desconhecido)
    double dpi = 0;
    UINT bits = 0;              // bits por canal
    std::wstring color, app, pageSize, camera, note;
    double pageWmm = 0, pageHmm = 0; // tamanho físico da página (INDD/AI), se conhecido
    UINT pages = 0;
    std::vector<std::wstring> fonts, fontFamilies;
};

inline void XmpInfo(const Bytes& b, Info* in) {
    auto last = [&](const char* name) { auto v = XmpValues(b, name); return v.empty() ? std::string() : v.back(); };
    if (in->app.empty()) in->app = Widen(last("xmp:CreatorTool"));
    std::string np = last("xmpTPg:NPages");
    if (!np.empty()) in->pages = (UINT)atoi(np.c_str());
    std::string sw = last("stDim:w"), sh = last("stDim:h"), su = last("stDim:unit");
    if (!sw.empty() && !sh.empty()) {
        double w = atof(sw.c_str()), h = atof(sh.c_str()), k = 0;
        if (su == "Points") k = 25.4 / 72; else if (su == "Inches") k = 25.4; else if (su == "Millimeters") k = 1; else if (su == "Centimeters") k = 10;
        wchar_t t[64];
        if (k > 0) { swprintf(t, 64, L"%.0f × %.0f mm", w * k, h * k); in->pageWmm = w * k; in->pageHmm = h * k; }
        else swprintf(t, 64, L"%.0f × %.0f %ls", w, h, Widen(su).c_str());
        in->pageSize = t;
    }
    std::vector<std::string> fam = XmpValues(b, "stFnt:fontFamily"), face = XmpValues(b, "stFnt:fontFace"), name = XmpValues(b, "stFnt:fontName");
    size_t n = fam.empty() ? name.size() : fam.size();
    for (size_t i = 0; i < n; ++i) {
        std::wstring family = Widen(fam.empty() ? name[i] : fam[i]);
        std::wstring label = family;
        if (!fam.empty() && face.size() == fam.size() && !face[i].empty() && face[i] != "Regular") label += L" " + Widen(face[i]);
        if (std::find(in->fonts.begin(), in->fonts.end(), label) == in->fonts.end()) { in->fonts.push_back(label); in->fontFamilies.push_back(family); }
    }
}

inline void WicInfo(const BYTE* data, size_t size, Info* in) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
    IStream* s = SHCreateMemStream(data, (UINT)size);
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    if (s && SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
        fr->GetSize(&in->w, &in->h);
        double dx = 0, dy = 0;
        if (SUCCEEDED(fr->GetResolution(&dx, &dy)) && dx > 1 && dx != 96) in->dpi = dx;
    }
    if (fr) fr->Release(); if (dec) dec->Release(); if (s) s->Release(); f->Release();
}

inline void GetInfo(IStream* s, const std::wstring& ext, Info* in) {
    BYTE magic[16] = {};
    ReadAt(s, 0, magic, sizeof(magic));
    PsdHeader ph;
    if (ReadPsdHeader(s, &ph)) {
        const wchar_t* modes[] = {L"Bitmap", T(L"Escala de cinza", L"Grayscale", L"Escala de grises"), T(L"Indexado", L"Indexed", L"Indexado"), L"RGB", L"CMYK", L"", L"", T(L"Multicanal", L"Multichannel", L"Multicanal"), L"Duotone", L"Lab"};
        in->w = ph.w; in->h = ph.h; in->bits = ph.depth;
        in->color = ph.mode < 10 ? modes[ph.mode] : L"";
        Bytes res; size_t len;
        if (PsdResources(s, res)) {
            if (const BYTE* r = PsdResource(res, 1005, &len)) if (len >= 16) {
                double d = BE(r, 4) / 65536.0;
                in->dpi = BE(r + 4, 2) == 2 ? d * 2.54 : d; // px/cm -> dpi
            }
            if (const BYTE* x = PsdResource(res, 1060, &len)) XmpInfo(Bytes(x, x + len), in);
        }
        return;
    }
    if (IsRaw(ext) || (ext.empty() && RawMagic(magic))) {
        Bytes b; JpegSpan js; TiffTags t;
        if (RawPreview(s, b, &js, &t)) {
            int o = t.orientation ? t.orientation : js.exif.orientation;
            bool swap = o >= 5 && o <= 8;
            in->w = swap ? js.h : js.w; in->h = swap ? js.w : js.h;
            std::string cam = t.make.empty() ? js.exif.make : t.make, model = t.model.empty() ? js.exif.model : t.model;
            if (!model.empty() && model.compare(0, cam.size(), cam) == 0) cam.clear(); // "Canon Canon EOS R5"
            in->camera = Widen(cam.empty() ? model : cam + " " + model);
            in->note = T(L"preview embutido", L"embedded preview", L"vista previa incrustada");
        }
        return;
    }
    if (IsPaletteExt(ext)) {
        std::vector<Swatch> sw;
        if (ReadPalette(s, ext, sw)) { wchar_t t[64]; swprintf(t, 64, T(L"%u cores", L"%u colors", L"%u colores"), (unsigned)sw.size()); in->pageSize = t; }
        in->note = T(L"clique numa cor para copiar o HEX", L"click a color to copy its HEX", L"haz clic en un color para copiar el HEX");
        return;
    }
    if (IsFontExt(ext)) {
        Bytes b; FontFile ff;
        if (ReadHead(s, b, 64u << 20) && ff.Open(b)) {
            in->pageSize = ff.family + (ff.style.empty() ? L"" : L" " + ff.style);
            wchar_t t[64]; swprintf(t, 64, T(L"%u glifos", L"%u glyphs", L"%u glifos"), ff.glyphs); in->note = t;
            in->fonts.push_back(ff.family); in->fontFamilies.push_back(ff.family);
        }
        return;
    }
    if (IsPdfLike(magic, ext)) { // páginas e tamanho físico pelo leitor de PDF
        pdfrt::Document doc;
        pdfrt::Size sz;
        if (doc.Open(s)) {
            in->pages = doc.Pages();
            if (doc.PageSize(0, &sz)) {
                in->pageWmm = sz.Width / 96 * 25.4; in->pageHmm = sz.Height / 96 * 25.4;
                wchar_t t[64];
                swprintf(t, 64, L"%.0f × %.0f mm", in->pageWmm, in->pageHmm);
                in->pageSize = t;
            }
        }
        Bytes b;
        if (ReadHead(s, b, 256u << 20)) { // fontes e programa (XMP), sem sobrescrever o que o PDF informou
            Info x;
            XmpInfo(b, &x);
            in->app = x.app; in->fonts = x.fonts; in->fontFamilies = x.fontFamilies;
        }
        return;
    }
    if (memcmp(magic, "PK\3\4", 4) == 0) {
        Bytes img;
        if (ZipPreview(s, img)) WicInfo(img.data(), img.size(), in);
        in->note = T(L"preview embutido", L"embedded preview", L"vista previa incrustada");
        in->w = in->h = 0; // tamanho do preview, não do documento
        return;
    }
    Bytes b;
    if (!ReadHead(s, b, 512u << 20)) return;
    if (b.size() > 2 && b[0] == 0x1F && b[1] == 0x8B) { Bytes svg; if (Gunzip(b, svg)) b.swap(svg); }
    XmpInfo(b, in);
    float w, h;
    if (in->app.empty() && in->pageSize.empty() && LooksLikeSvg(b) && SvgSize(b, &w, &h)) {
        wchar_t t[64];
        swprintf(t, 64, L"%g × %g", w, h);
        in->pageSize = t;
        in->pageWmm = w / 96 * 25.4; in->pageHmm = h / 96 * 25.4; // unidades do SVG = px a 96 dpi
        in->note = T(L"vetor", L"vector", L"vector");
    }
}

} // namespace fmt
