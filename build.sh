#!/bin/sh
# Compila a partir do Linux com MinGW (g++-mingw-w64-x86-64-posix).
# Precisa dos headers d2d1_3.h/d2d1svg.h recentes do mingw-w64 em $D2D_INC.
set -e
CXX=x86_64-w64-mingw32-g++
FLAGS="-O2 -DUNICODE -D_UNICODE -static -s ${D2D_INC:+-I$D2D_INC}"
$CXX $FLAGS -shared -o DesignThumbs.dll DesignThumbs.cpp DesignThumbs.def -lshlwapi -lwindowscodecs -ld2d1 -lole32 -luuid -lgdi32
x86_64-w64-mingw32-windres -c 65001 DesignPeek.rc -O coff -o DesignPeek.res.o
$CXX $FLAGS -municode -mwindows -D_WIN32_WINNT=0x0A00 -o DesignPeek.exe DesignPeek.cpp DesignPeek.res.o -lshlwapi -lole32 -loleaut32 -luuid -lgdi32 -ldwmapi -lshell32 -ld2d1 -ldwrite -lwindowscodecs
