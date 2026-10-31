@echo off
rem Execute como Administrador
"%~dp0DesignPeek.exe" /quit
"%~dp0DesignPeek.exe" /noautostart
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v DesignPeek /f >nul 2>&1
regsvr32 /u /s "%~dp0DesignThumbs.dll" && echo Removido.
rem libera a DLL para poder ser substituida/apagada
taskkill /f /im dllhost.exe >nul 2>&1
taskkill /f /im explorer.exe >nul 2>&1
start explorer.exe
