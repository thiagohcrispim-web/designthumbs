#!/bin/sh
# Compila o DesignThumbs a partir do Linux com MinGW (pacote g++-mingw-w64-x86-64-posix).
# Usa os headers mais novos do mingw-w64 em third_party/mingw-w64-headers.
set -e
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++
RC=x86_64-w64-mingw32-windres
FLAGS="-O2 -DUNICODE -D_UNICODE -static -s -ffunction-sections -fdata-sections -Wl,--gc-sections -Ithird_party/mingw-w64-headers"
$RC -c 65001 DesignThumbs.rc -O coff -o DesignThumbs.res.o
$CXX $FLAGS -shared -o DesignThumbs.dll DesignThumbs.cpp DesignThumbs.res.o DesignThumbs.def -lshlwapi -lwindowscodecs -ld2d1 -ldwrite -lole32 -luuid -lgdi32 -lruntimeobject -lshcore
# libwebp (só o codificador): compila uma vez em build/webp
mkdir -p build/webp
if [ ! -f build/webp/libwebp.a ]; then
  for f in third_party/libwebp/src/enc/*.c third_party/libwebp/src/dsp/*.c third_party/libwebp/src/utils/*.c third_party/libwebp/sharpyuv/*.c; do
    x86_64-w64-mingw32-gcc -O2 -Ithird_party/libwebp -DWEBP_USE_THREAD -c "$f" -o "build/webp/$(echo "$f" | tr / _).o"
  done
  x86_64-w64-mingw32-ar rcs build/webp/libwebp.a build/webp/*.o
fi
$RC -c 65001 Vizu.rc -O coff -o Vizu.res.o # embute a DLL no EXE
$CXX $FLAGS -municode -mwindows -D_WIN32_WINNT=0x0A00 -Wl,-Map=build/Vizu.map -o Vizu.exe Vizu.cpp Vizu.res.o build/webp/libwebp.a -lshlwapi -lole32 -loleaut32 -luuid -lgdi32 -ldwmapi -lshell32 -ld2d1 -ldwrite -lwindowscodecs -lruntimeobject -lshcore -lwinhttp -lurlmon -lcomctl32 -lcomdlg32 -lxmllite -lbcrypt -lmscms
cp Vizu.exe Vizu-Setup.exe # o próprio Vizu é o instalador
