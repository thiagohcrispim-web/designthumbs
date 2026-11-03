@echo off
rem Remove o DesignThumbs do seu usuario
"%~dp0DesignPeek.exe" /quit
"%~dp0DesignPeek.exe" /noautostart
taskkill /f /im DesignPeek.exe >nul 2>&1
regsvr32 /u /s "%~dp0DesignThumbs.dll" && echo Removido.
rem libera a DLL para poder ser substituida/apagada
taskkill /f /im prevhost.exe >nul 2>&1
taskkill /f /im dllhost.exe >nul 2>&1
taskkill /f /im explorer.exe >nul 2>&1
start explorer.exe
