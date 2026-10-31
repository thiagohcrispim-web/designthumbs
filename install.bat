@echo off
rem Execute como Administrador
cd /d "%~dp0"
regsvr32 /s "%~dp0DesignThumbs.dll" && echo Miniaturas instaladas.
rem DesignPeek (Espaco = preview) inicia com o Windows
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v DesignPeek /t REG_SZ /d "\"%~dp0DesignPeek.exe\"" /f >nul
taskkill /f /im explorer.exe >nul
del /f /q /a "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db" 2>nul
start explorer.exe
rem inicia sem privilegios de admin (precisa conversar com o Explorer)
timeout /t 3 /nobreak >nul
explorer.exe "%~dp0DesignPeek.exe"
echo Pronto.
