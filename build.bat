@echo off
rem Rode no "x64 Native Tools Command Prompt for VS"
cl /nologo /O2 /EHsc /LD /DUNICODE /D_UNICODE DesignThumbs.cpp /link /DEF:DesignThumbs.def /OUT:DesignThumbs.dll
rc /nologo DesignPeek.rc
cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE DesignPeek.cpp DesignPeek.res /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib dwmapi.lib
del *.obj *.exp *.lib 2>nul
