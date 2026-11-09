#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include "../VectorConvert.h"
#include <iostream>
#include <stdexcept>

static unsigned checks = 0;
static void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        vec::Temp t;
        Check(!t.dir.empty(), "temp directory");
        auto audit = [&](const std::wstring& body, bool expected, bool epsBlocked = false) {
            auto file = t.At(L"source.svg"); DeleteFileW(file.c_str());
            Check(vec::WriteNew(file, L"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50\">" + body + L"</svg>"), "write fixture");
            vec::Audit a; std::wstring error;
            bool result = vec::AuditSvg(file, a, error);
            if (result != expected) std::wcerr << L"Fixture: " << body << L"\nDiagnostic: " << error << L"\n";
            Check(result == expected, "SVG audit result");
            if (expected) Check(!a.epsBlocks.empty() == epsBlocked, "EPS blockers");
        };
        audit(L"<g transform=\"translate(2 3)\"><path d=\"M0 0C1 2 3 4 5 6\"/></g>", true);
        audit(L"<image href=\"data:image/png;base64,AAAA\"/>", false);
        audit(L"<path d=\"M0 0L1 1\"/><style>path{fill:red}</style>", false);
        audit(L"<use href=\"https://example.com/a.svg#a\"/>", false);
        audit(L"<use href=\"#missing\"/>", false);
        audit(L"<defs><path id=\"p\" d=\"M0 0L10 10\"/></defs><use href=\"#p\"/>", true);
        audit(L"<rect width=\"10\" height=\"10\" opacity=\".5\"/>", true, true);
        audit(L"<rect width=\"10\" height=\"10\" style=\"fill-opacity:50%\"/>", true, true);
        audit(L"<defs><linearGradient id=\"g\"/></defs><path fill=\"url(#g)\" d=\"M0 0L1 1\"/>", true, true);
        audit(L"", false);
        audit(L"<defs><path id=\"unused\" d=\"M0 0L10 10\"/></defs>", false);
        Check(vec::Quote(L"C:\\nome com espaço\\") == L"\"C:\\nome com espaço\\\\\"", "Windows argument escaping");
        Check(!vec::IsInput(L"preview.png") && vec::IsInput(L"desenho.CDR"), "input types");
        auto eps = t.At(L"output.eps"); std::wstring error;
        Check(vec::WriteNew(eps, L"%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 10 10\n%%BeginProlog\n/cairo_image { image } def\n%%EndProlog\n0 0 moveto 10 10 lineto stroke\n"), "EPS fixture");
        Check(vec::AuditEps(eps, error), "unused image helper allowed");
        DeleteFileW(eps.c_str());
        Check(vec::WriteNew(eps, L"%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 10 10\n1 1 cairo_image\n"), "raster EPS fixture");
        Check(!vec::AuditEps(eps, error), "raster EPS blocked");
        if (argc == 4) {
            SetEnvironmentVariableW(L"VIZU_INKSCAPE", argv[1]);
            std::wstring base = argv[3];
            for (bool asEps : {false, true}) {
                std::wstring out = base + (asEps ? L".eps" : L".svg");
                vec::Result r = vec::Convert(argv[2], out, asEps, false);
                if (!r.ok) std::wcerr << r.error << L"\n";
                Check(r.ok, "real engine conversion");
                auto duplicate = vec::Convert(argv[2], out, asEps, false);
                Check(!duplicate.ok, "never overwrite an existing output");
            }
        }
        std::cout << checks << " checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        CoUninitialize(); return 1;
    }
    CoUninitialize(); return 0;
}
