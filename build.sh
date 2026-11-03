#!/bin/sh
# Compila o DesignThumbs a partir do Linux com MinGW (pacote g++-mingw-w64-x86-64-posix).
# Usa os headers mais novos do mingw-w64 em third_party/mingw-w64-headers.
set -e
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++
RC=x86_64-w64-mingw32-windres
FLAGS="-O2 -DUNICODE -D_UNICODE -static -s -Ithird_party/mingw-w64-headers"
$RC -c 65001 DesignThumbs.rc -O coff -o DesignThumbs.res.o
$CXX $FLAGS -shared -o DesignThumbs.dll DesignThumbs.cpp DesignThumbs.res.o DesignThumbs.def -lshlwapi -lwindowscodecs -ld2d1 -ldwrite -lole32 -luuid -lgdi32 -lruntimeobject -lshcore
$RC -c 65001 DesignPeek.rc -O coff -o DesignPeek.res.o # embute a DLL no EXE
$CXX $FLAGS -municode -mwindows -D_WIN32_WINNT=0x0A00 -o DesignPeek.exe DesignPeek.cpp DesignPeek.res.o -lshlwapi -lole32 -loleaut32 -luuid -lgdi32 -ldwmapi -lshell32 -ld2d1 -ldwrite -lwindowscodecs -lruntimeobject -lshcore -lwinhttp -lurlmon
cp DesignPeek.exe DesignThumbs-Setup.exe # o próprio DesignPeek é o instalador
