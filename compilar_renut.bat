@echo off
rem tekken6-nx: recompila el juego para PC (incluye los shaders capturados al jugar). Cierra el juego antes.
rem Si se corta la luz, vuelve a hacer doble clic: retoma donde iba.
cd /d "%~dp0"
call "%~dp0tools\build_pc.bat"
if errorlevel 1 (echo. & echo *** La compilacion fallo: revisa los mensajes de arriba. ***) else (echo. & echo Listo: ya puedes volver a jugar con jugar_renut.bat)
pause
