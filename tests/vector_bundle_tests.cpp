#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include "../VectorBundle.h"
#include <iostream>
#include <stdexcept>
static int checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    vec::Temp root;
    try {
        Check(!root.dir.empty(), "isolated test directory");
        std::wstring stage, error;
        Check(!vbundle::Owned(root.dir, root.dir + L"\\..\\vector-engine"), "reject escaped path");
        Check(!vbundle::Remove(root.dir, root.dir + L"\\unrelated"), "reject unrelated directory");
        Check(vec::WriteNew(root.At(L"source.svg"), L"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50\"><path d=\"M1 1C5 30 50 40 90 1Z\"/></svg>"), "test source");
        Check(vbundle::Prepare(root.dir, stage, error), "extract and verify embedded engine");
        Check(vbundle::Commit(root.dir, stage, error), "install embedded engine");
        std::wstring engine = vec::BundledEngine(root.dir);
        Check(!engine.empty(), "discover installed engine automatically");
        SetEnvironmentVariableW(L"VIZU_INKSCAPE", engine.c_str());
        auto svg = vec::Convert(root.At(L"source.svg"), root.At(L"document.svg"), false, false);
        if (!svg.ok) std::wcerr << svg.error << L"\n";
        Check(svg.ok, "SVG conversion using installed engine");
        auto eps = vec::Convert(root.At(L"source.svg"), root.At(L"output.eps"), true, false);
        if (!eps.ok) std::wcerr << eps.error << L"\n";
        Check(eps.ok, "EPS conversion using installed engine");
        DeleteFileW(root.At(L"document.svg.vizu.txt").c_str());
        DeleteFileW(root.At(L"output.eps.vizu.txt").c_str());
        // A failed replacement must preserve a functioning previous engine.
        std::wstring missing = root.dir + L"\\vector-engine-stage-missing";
        Check(!vbundle::Commit(root.dir, missing, error), "failed replacement rejected");
        Check(!vec::BundledEngine(root.dir).empty(), "old engine preserved after failed replacement");
        error.clear();
        Check(vbundle::Prepare(root.dir, stage, error), "prepare reinstall");
        Check(vbundle::Commit(root.dir, stage, error), "replace previous engine");
        Check(vbundle::Cleanup(root.dir, false), "cleanup replacement remnants");
        Check(!vec::BundledEngine(root.dir).empty(), "engine present after reinstall");
        Check(vbundle::Cleanup(root.dir, true), "uninstall owned engine");
        Check(vec::BundledEngine(root.dir).empty(), "engine removed");
        Check(vec::Exists(root.At(L"source.svg")), "unrelated source preserved");
        std::cout << checks << " bundle checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        vbundle::Cleanup(root.dir, true);
        CoUninitialize(); return 1;
    }
    CoUninitialize(); return 0;
}
