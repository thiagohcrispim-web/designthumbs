// Jobs.h - executa as ações do menu circular sobre uma lista de arquivos: converter para um formato
// (com edições), editar mantendo o formato, ferramentas de PDF e extras (camadas, paletas, fontes).
#pragma once
#include <functional>
#include "Convert.h"
#include "Ops.h"
#include "PdfEdit.h"
#include "Extras.h"
#include "VectorConvert.h"

namespace jobs {
using std::min; using std::max;
using conv::DirOf; using conv::BaseOf; using conv::UniquePath; using conv::ExtOf;

enum Action {
    kToFormat,     // converter para `format` (com `edit`)
    kEditSame,     // aplicar `edit` mantendo o formato (girar, cinza, sem metadados...)
    kPdfSplit,     // uma página por arquivo
    kPdfExtract,   // páginas `pages` num PDF novo
    kPdfCompress,  // páginas viram JPG
    kPsdLayers,    // camadas do PSD em PNG
    kPaletteSheet, // folha de cores PNG + PDF
    kPaletteText,  // HEX / RGB / CSS em .txt
    kPaletteSwap,  // ASE <-> ACO
    kFontSheet,    // folha de amostra PNG + PDF
};

struct Job {
    Action action = kToFormat;
    ops::Format format = ops::kPng;
    ops::Edit edit;
    float quality = 0.92f;
    float webpQuality = 0.75f; // WEBP: compressão forte (como os conversores online)
    UINT dpi = 72;
    bool pdfRaw = false;
    bool textToPaths = false; // SVG/EPS: preserve text unless explicitly requested.
    std::wstring pages;    // intervalo ("1-3, 5"); vazio = todas
    std::wstring suffix;   // acrescentado ao nome ("(girado)")
};

struct Result { std::vector<std::wstring> outputs; int failed = 0; std::vector<std::wstring> errors; };

inline bool IsPdfFile(const std::wstring& f) {
    std::wstring e = ExtOf(f);
    if (e != L".pdf" && e != L".ai") return false;
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    char m[5] = {}; DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, m, 4, &got, nullptr); CloseHandle(h); }
    return !memcmp(m, "%PDF", 4);
}

inline std::wstring Named(const std::wstring& base, const std::wstring& suffix) { return suffix.empty() ? base : base + L" " + suffix; }

// Formato de saída para "editar mantendo o formato".
inline ops::Format SameFormat(const std::wstring& f) {
    std::wstring e = ExtOf(f);
    if (e == L".jpg" || e == L".jpeg" || e == L".jfif") return ops::kJpg;
    if (e == L".webp") return ops::kWebp;
    if (e == L".tif" || e == L".tiff") return ops::kTiff;
    if (e == L".bmp") return ops::kBmp;
    if (e == L".gif") return ops::kGif;
    return ops::kPng;
}

// Páginas de um arquivo como bitmaps editados (respeitando o intervalo).
inline std::vector<HBITMAP> Frames(const std::wstring& f, const Job& j, ops::Format outFmt, fmt::Info* info) {
    std::vector<HBITMAP> out;
    conv::FileInfo(f, info);
    UINT cx = conv::TargetSize(*info, j.dpi);
    int pages = 1;
    HBITMAP first = nullptr; bool alpha;
    if (!conv::RenderFile(f, cx, 0, &first, &alpha, &pages)) return out;
    std::vector<int> wanted = pdfedit::ParseRange(j.pages, pages);
    bool opaque = !ops::HasAlpha(outFmt);
    for (int p : wanted) {
        HBITMAP b = nullptr;
        if (p == 0) { b = first; first = nullptr; }
        else if (!conv::RenderFile(f, cx, p, &b, &alpha, nullptr)) continue;
        if (j.edit.Any() || opaque) { HBITMAP e = ops::Apply(b, j.edit, opaque); DeleteObject(b); b = e; }
        if (b) out.push_back(b);
    }
    if (first) DeleteObject(first);
    return out;
}

// Página A4 ou do tamanho da imagem, em pontos.
inline void PageSize(const fmt::Info& in, UINT w, UINT h, UINT dpi, double* ptW, double* ptH) {
    if (in.pageWmm > 0 && in.pageHmm > 0) { *ptW = in.pageWmm / 25.4 * 72; *ptH = *ptW * h / w; }
    else { *ptW = w * 72.0 / dpi; *ptH = h * 72.0 / dpi; }
}

inline bool AddPdfPage(conv::PdfWriter& pdf, HBITMAP b, const fmt::Info& in, const Job& j) {
    BYTE* px; UINT w, h;
    std::vector<BYTE> data;
    if (!ops::Pixels(b, &px, &w, &h) || !(j.pdfRaw ? conv::RgbBytes(b, data) : conv::JpegBytes(b, j.quality, data))) return false;
    double pw, ph;
    PageSize(in, w, h, j.dpi, &pw, &ph);
    pdf.AddImagePage(data, w, h, pw, ph, !j.pdfRaw);
    return true;
}

inline Result Run(const std::vector<std::wstring>& files, const Job& j, const std::function<void(size_t, size_t)>& progress) {
    Result r;
    auto done = [&](size_t i) { progress(i + 1, files.size()); };
    if (files.empty()) return r;

    // Vector exports MUST precede Frames(): a rendered thumbnail has already lost the vectors.
    if (j.action == kToFormat && (j.format == ops::kSvg || j.format == ops::kEps)) {
        for (size_t i = 0; i < files.size(); ++i) {
            if (j.edit.Any() || !j.pages.empty()) {
                ++r.failed;
                r.errors.push_back(BaseOf(files[i]) + L": edições de pixels e intervalos de páginas não são aplicáveis à conversão vetorial.");
            } else {
                std::wstring out = UniquePath(DirOf(files[i]), Named(BaseOf(files[i]), j.suffix), ops::Ext(j.format));
                vec::Result v = vec::Convert(files[i], out, j.format == ops::kEps, j.textToPaths);
                if (v.ok) r.outputs.push_back(out);
                else { ++r.failed; r.errors.push_back(BaseOf(files[i]) + L": " + v.error); }
            }
            done(i);
        }
        return r;
    }

    // ---- PDF de saída ----
    if (j.action == kToFormat && j.format == ops::kPdf) {
        bool allPdf = !j.edit.Any();
        for (auto& f : files) allPdf &= IsPdfFile(f);
        std::wstring base = BaseOf(files[0]);
        if (files.size() > 1) base += L" (+" + std::to_wstring(files.size() - 1) + L")";
        if (allPdf) { // junta mantendo o original (texto e vetores)
            if (files.size() == 1 && j.pages.empty() && ExtOf(files[0]) == L".ai") { // .ai já é PDF
                std::wstring out = UniquePath(DirOf(files[0]), BaseOf(files[0]), L".pdf");
                if (CopyFileW(files[0].c_str(), out.c_str(), TRUE)) r.outputs.push_back(out); else r.failed++;
                done(0);
                return r;
            }
            std::vector<pdfedit::File> pf(files.size());
            std::vector<pdfedit::Part> parts;
            bool ok = true;
            for (size_t i = 0; i < files.size(); ++i) {
                if (!pf[i].Load(files[i])) { ok = false; break; }
                parts.push_back({&pf[i], pdfedit::ParseRange(j.pages, (int)pf[i].Pages().size())});
                done(i);
            }
            std::wstring out = UniquePath(DirOf(files[0]), base, L".pdf");
            if (ok && pdfedit::Assemble(parts, out)) { r.outputs.push_back(out); return r; }
            // PDF que o copiador não entende (com senha, danificado): cai no modo imagem abaixo
        }
        conv::PdfWriter pdf;
        for (size_t i = 0; i < files.size(); ++i) {
            fmt::Info in;
            std::vector<HBITMAP> fr = Frames(files[i], j, ops::kJpg, &in);
            bool any = false;
            for (HBITMAP b : fr) { any |= AddPdfPage(pdf, b, in, j); DeleteObject(b); }
            if (!any) r.failed++;
            done(i);
        }
        std::wstring out = UniquePath(DirOf(files[0]), base, L".pdf");
        if (pdf.PageCount() && pdf.Save(out)) r.outputs.push_back(out); else if (pdf.PageCount()) r.failed = (int)files.size();
        return r;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring& f = files[i];
        std::wstring dir = DirOf(f), base = BaseOf(f);
        bool ok = false;
        switch (j.action) {
        case kToFormat:
        case kEditSame: {
            ops::Format of = j.action == kEditSame ? SameFormat(f) : j.format;
            fmt::Info in;
            std::vector<HBITMAP> fr = Frames(f, j, of, &in);
            float q = of == ops::kWebp ? j.webpQuality : j.quality;
            if (of == ops::kTiff && fr.size() > 1) { // TIFF de várias páginas
                std::wstring out = UniquePath(dir, Named(base, j.suffix), ops::Ext(of));
                if (ops::Save(fr, out, of, j.quality, j.dpi)) { r.outputs.push_back(out); ok = true; }
            } else
                for (size_t k = 0; k < fr.size(); ++k) {
                    std::wstring name = Named(base, j.suffix) + (fr.size() > 1 ? L" - p" + std::to_wstring(k + 1) : L"");
                    std::wstring out = UniquePath(dir, name, ops::Ext(of));
                    if (ops::Save({fr[k]}, out, of, q, j.dpi)) { r.outputs.push_back(out); ok = true; }
                }
            for (HBITMAP b : fr) DeleteObject(b);
            break;
        }
        case kPdfSplit:
        case kPdfExtract: {
            pdfedit::File pf;
            if (!IsPdfFile(f) || !pf.Load(f)) break;
            int n = (int)pf.Pages().size();
            std::vector<int> pages = pdfedit::ParseRange(j.action == kPdfExtract ? j.pages : L"", n);
            if (j.action == kPdfExtract) {
                std::wstring out = UniquePath(dir, Named(base, j.suffix), L".pdf");
                ok = pdfedit::Assemble({{&pf, pages}}, out);
                if (ok) r.outputs.push_back(out);
            } else
                for (int p : pages) {
                    std::wstring out = UniquePath(dir, base + L" - p" + std::to_wstring(p + 1), L".pdf");
                    if (pdfedit::Assemble({{&pf, {p}}}, out)) { r.outputs.push_back(out); ok = true; }
                }
            break;
        }
        case kPdfCompress: {
            Job jj = j;
            jj.dpi = 150; jj.pdfRaw = false;
            fmt::Info in;
            std::vector<HBITMAP> fr = Frames(f, jj, ops::kJpg, &in);
            conv::PdfWriter pdf;
            for (HBITMAP b : fr) { AddPdfPage(pdf, b, in, jj); DeleteObject(b); }
            std::wstring out = UniquePath(dir, Named(base, j.suffix), L".pdf");
            ok = pdf.PageCount() && pdf.Save(out);
            if (ok) r.outputs.push_back(out);
            break;
        }
        case kPsdLayers: {
            IStream* s = nullptr;
            std::vector<extras::Layer> layers;
            if (SUCCEEDED(SHCreateStreamOnFileEx(f.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) { extras::PsdLayers(s, layers); s->Release(); }
            if (layers.empty()) break;
            std::wstring folder = UniquePath(dir, Named(base, j.suffix), L"");
            CreateDirectoryW(folder.c_str(), nullptr);
            int k = 0;
            for (auto& l : layers) { // a primeira da lista é a de baixo no Photoshop
                wchar_t num[8]; swprintf(num, 8, L"%02d", ++k);
                std::wstring out = UniquePath(folder, std::wstring(num) + L" - " + extras::SafeName(l.name), L".png");
                if (ops::Save({l.bmp}, out, ops::kPng, 1, 0)) ok = true;
                DeleteObject(l.bmp);
            }
            if (ok) r.outputs.push_back(folder);
            break;
        }
        case kPaletteSheet:
        case kPaletteText:
        case kPaletteSwap: {
            IStream* s = nullptr;
            std::vector<fmt::Swatch> sw;
            if (SUCCEEDED(SHCreateStreamOnFileEx(f.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) { fmt::ReadPalette(s, ExtOf(f), sw); s->Release(); }
            if (sw.empty()) break;
            if (j.action == kPaletteSheet) {
                HBITMAP b = nullptr;
                if (FAILED(fmt::RenderPalette(sw, 1600, &b))) break;
                std::wstring png = UniquePath(dir, Named(base, j.suffix), L".png"), pdfPath = UniquePath(dir, Named(base, j.suffix), L".pdf");
                if (ops::Save({b}, png, ops::kPng, 1, 0)) { r.outputs.push_back(png); ok = true; }
                conv::PdfWriter pdf; fmt::Info in; Job jj = j; jj.dpi = 200;
                if (AddPdfPage(pdf, b, in, jj) && pdf.Save(pdfPath)) r.outputs.push_back(pdfPath);
                DeleteObject(b);
            } else if (j.action == kPaletteText) {
                std::string t = extras::PaletteText(sw, base);
                std::wstring out = UniquePath(dir, Named(base, j.suffix), L".txt");
                if (ops::WriteFileBytes(out, t.data(), t.size())) { r.outputs.push_back(out); ok = true; }
            } else {
                bool toAco = ExtOf(f) == L".ase";
                fmt::Bytes b = toAco ? extras::WriteAco(sw) : extras::WriteAse(sw);
                std::wstring out = UniquePath(dir, base, toAco ? L".aco" : L".ase");
                if (ops::WriteFileBytes(out, b.data(), b.size())) { r.outputs.push_back(out); ok = true; }
            }
            break;
        }
        case kFontSheet: {
            IStream* s = nullptr;
            fmt::Bytes data;
            if (SUCCEEDED(SHCreateStreamOnFileEx(f.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s))) { fmt::ReadHead(s, data, 64u << 20); s->Release(); }
            HBITMAP b = nullptr;
            if (data.empty() || FAILED(fmt::RenderFont(data, 1600, &b))) break;
            std::wstring png = UniquePath(dir, Named(base, j.suffix), L".png"), pdfPath = UniquePath(dir, Named(base, j.suffix), L".pdf");
            if (ops::Save({b}, png, ops::kPng, 1, 0)) { r.outputs.push_back(png); ok = true; }
            conv::PdfWriter pdf; fmt::Info in; in.pageWmm = 210; in.pageHmm = 273;
            if (AddPdfPage(pdf, b, in, j) && pdf.Save(pdfPath)) r.outputs.push_back(pdfPath);
            DeleteObject(b);
            break;
        }
        }
        if (!ok) r.failed++;
        done(i);
    }
    return r;
}

} // namespace jobs
