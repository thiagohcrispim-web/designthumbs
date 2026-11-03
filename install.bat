@echo off
rem Instala para o seu usuario (NAO precisa de administrador)
cd /d "%~dp0"
rem remove a marca "baixado da internet" (evita a tela do SmartScreen)
powershell -NoProfile -Command "Get-ChildItem -LiteralPath '%~dp0' | Unblock-File" >nul 2>&1
"%~dp0DesignPeek.exe" /quit
regsvr32 /s "%~dp0DesignThumbs.dll" && echo Miniaturas instaladas.
"%~dp0DesignPeek.exe" /autostart
taskkill /f /im prevhost.exe >nul 2>&1
taskkill /f /im dllhost.exe >nul 2>&1
taskkill /f /im explorer.exe >nul
del /f /q /a "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db" 2>nul
start explorer.exe
start "" "%~dp0DesignPeek.exe"
echo Pronto.
